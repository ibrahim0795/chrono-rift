// ============================================================
//  asp.cpp  —  Automated Strategic Process (AI)
//
//  ARCHITECTURAL FIX — Flaw 2 (Stun Mechanic):
//
//    OLD: Arbiter set is_stunned=true; enemy threads polled the
//         flag before each action with:  if (e.is_stunned) skip;
//
//    NEW:
//      • Each enemy thread registers its pthread_t in
//        gs->asp_thread_tids[enemy_idx] at startup.
//      • When an enemy is stunned, the Arbiter calls:
//            pthread_kill(gs->asp_thread_tids[i], SIGUSR1)
//        This delivers SIGUSR1 to that specific thread only.
//      • The per-thread stun_handler calls sleep(3) INSIDE the
//        handler body, halting the thread for exactly 3 seconds
//        from the point of interruption, then resuming normally.
//      • No game logic code polls is_stunned.  The interruption
//        is asynchronous and non-blocking for all other threads.
//
//  OS Concepts demonstrated:
//    • POSIX Shared Memory  (shm_open / mmap)
//    • pthreads: one dedicated thread per enemy NPC
//    • SIGSTOP / SIGCONT: ultimate-ability full-pause (OS-native)
//    • SIGUSR1 per-thread stun via pthread_kill + sleep(3) in handler
//    • PTHREAD_PROCESS_SHARED mutex for shared state access
// ============================================================

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include "../common.h"

using namespace std;

// ─── Globals ──────────────────────────────────────────────────────────────────
static GameState* gs      = nullptr;
static bool       running = true;

// ─── Per-thread stun signal handler ──────────────────────────────────────────
//
// DESIGN RATIONALE (satisfies Flaw 2 requirement):
//
//   The Arbiter delivers SIGUSR1 to a specific enemy thread via:
//       pthread_kill(gs->asp_thread_tids[enemy_idx], SIGUSR1)
//
//   Calling sleep(3) here suspends ONLY the targeted thread for
//   3 seconds from the exact moment of signal delivery.  All other
//   enemy threads (and the main ASP thread) continue running.
//
//   After sleep(3) returns, this thread resumes from the point of
//   interruption.  Typically it was either:
//     (a) Waiting in the usleep(50000) spin for active_turn, or
//     (b) Waiting in the usleep(20000) spin for pending_action.
//   Both loops simply retry after the handler returns — the Arbiter's
//   turn-timeout will have advanced active_turn, so the thread finds
//   it is no longer this enemy's turn and idles until next time.
//
static void stun_handler(int sig) {
    (void)sig;
    sleep(3);  // halt this thread — and only this thread — for 3 seconds
}

static void sigterm_handler(int sig) {
    (void)sig;
    running = false;
}

// SIGSTOP / SIGCONT for the ultimate ability are delivered to the
// entire ASP process and handled automatically by the OS.

// ─── Enemy AI decision ────────────────────────────────────────────────────────
// Simple AI: strike the first alive player with 80% probability, else skip.
static void enemy_decide(int enemy_idx) {
    int target = -1;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].is_alive) { target = i; break; }
    if (target == -1) return;

    if (rand() % 10 < 8) {
        gs->action_actor   = MAX_PLAYERS + enemy_idx;
        gs->action_target  = target;
        gs->pending_action = ACTION_STRIKE;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d attacks Player %d!", enemy_idx + 1, target + 1);
    } else {
        gs->action_actor   = MAX_PLAYERS + enemy_idx;
        gs->pending_action = ACTION_SKIP;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d skips.", enemy_idx + 1);
    }
}

// ─── Enemy thread ─────────────────────────────────────────────────────────────
struct EnemyThreadArg { int enemy_index; };

static void* enemy_thread_func(void* arg) {
    EnemyThreadArg* earg = (EnemyThreadArg*)arg;
    const int idx = earg->enemy_index;

    // ── 1. Register this thread's tid for targeted stun signals ───────────
    gs->asp_thread_tids[idx] = pthread_self();

    // ── 2. Unblock SIGUSR1 in this thread only ─────────────────────────────
    // main() blocks SIGUSR1 process-wide.  Each enemy thread unblocks it
    // for itself so only the correct thread receives the stun signal.
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

    while (running && !gs->is_game_over) {
        if (idx >= gs->num_enemies) { usleep(100000); continue; }

        Entity& e = gs->enemies[idx];
        if (!e.is_alive)  { usleep(100000); continue; }

        // Only act when it is this enemy's turn.
        int my_turn = MAX_PLAYERS + idx;
        if (gs->active_turn != my_turn) { usleep(50000); continue; }

        // ── Decide and post action ─────────────────────────────────────────
        //
        // If SIGUSR1 arrives here, stun_handler fires immediately and
        // sleep(3) runs.  After 3 s the handler returns, execution
        // resumes, and the next loop iteration finds that active_turn
        // has advanced (Arbiter timeout), so the thread idles cleanly.
        //
        // There is NO is_stunned poll here — the stun is fully asynchronous.
        //
        pthread_mutex_lock(&gs->memory_lock);
        enemy_decide(idx);
        pthread_mutex_unlock(&gs->memory_lock);

        // Wait for the Arbiter to consume and clear the action.
        while (gs->pending_action != ACTION_NONE && !gs->is_game_over)
            usleep(20000);
    }

    return nullptr;
}

// ─── Attach to shared memory ──────────────────────────────────────────────────
static void attach_shared_memory() {
    int fd = -1;
    for (int i = 0; i < 10 && fd == -1; i++) {
        fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd == -1) sleep(1);
    }
    if (fd == -1) { perror("ASP: shm_open"); exit(1); }

    gs = (GameState*)mmap(nullptr, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("ASP: mmap"); exit(1); }
    close(fd);
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    // ── Block SIGUSR1 process-wide; threads unblock it individually ────────
    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr);

    // Install the stun handler.  SA_RESTART is omitted so that any blocking
    // syscall (usleep is not blocking, but for correctness) returns EINTR.
    struct sigaction sa{};
    sa.sa_handler = stun_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    sigaction(SIGUSR1, &sa, nullptr);

    signal(SIGTERM, sigterm_handler);
    // SIGSTOP / SIGCONT for ultimate ability are handled by the OS.

    cout << "ASP: Connecting to shared memory..." << endl;
    attach_shared_memory();

    gs->asp_pid = getpid();
    srand(ROLL_SEED + 1);

    // Zero out thread-tid array — Arbiter checks for 0 before pthread_kill.
    memset(gs->asp_thread_tids, 0, sizeof(gs->asp_thread_tids));

    cout << "ASP: Connected.  Enemies = " << gs->num_enemies << endl;

    // Spawn one thread per enemy slot (extras idle if num_enemies < MAX_ENEMIES).
    pthread_t      threads[MAX_ENEMIES];
    EnemyThreadArg args[MAX_ENEMIES];

    for (int i = 0; i < MAX_ENEMIES; i++) {
        args[i].enemy_index = i;
        pthread_create(&threads[i], nullptr, enemy_thread_func, &args[i]);
    }

    for (int i = 0; i < MAX_ENEMIES; i++)
        pthread_join(threads[i], nullptr);

    munmap(gs, sizeof(GameState));
    cout << "ASP: exiting." << endl;
    return 0;
}