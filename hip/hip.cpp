// ============================================================
//  hip.cpp  —  Human Interfacing Process
//
//  ARCHITECTURAL FIX — Flaw 1 (Input Architecture):
//
//    OLD: hip.cpp was idle; all SFML clicks were written directly
//         to gs->pending_action inside the Arbiter render thread.
//
//    NEW:
//      • One pthread is created per player-controlled character.
//      • The Arbiter render thread writes a UIEvent struct into the
//        named FIFO (HIP_PIPE_PATH) whenever the player clicks a
//        UI element.  It does NOT touch pending_action for players.
//      • Each HIP player thread reads UIEvents addressed to it,
//        then pushes a PlayerInputEvent into its per-player
//        PlayerActionBuffer in shared memory.
//      • The Arbiter main loop pops from PlayerActionBuffer to
//        obtain player decisions — it is now a pure engine/visualizer.
//
//  ARCHITECTURAL FIX — Flaw 2 (Stun Mechanic):
//
//    OLD: Arbiter set is_stunned=true; threads polled the flag.
//
//    NEW:
//      • Each player thread registers its pthread_t in
//        gs->hip_thread_tids[player_idx] at startup.
//      • When a player is stunned, the Arbiter calls:
//            pthread_kill(gs->hip_thread_tids[i], SIGUSR1)
//        This delivers SIGUSR1 to that specific thread only.
//      • The per-thread stun_handler calls sleep(3) INSIDE the
//        handler body, halting the thread for exactly 3 seconds
//        from the point of interruption, then resuming normally.
//      • No flag is ever polled. The interruption is fully
//        asynchronous. Other player threads are unaffected.
//
//  OS Concepts demonstrated:
//    • POSIX Shared Memory  (shm_open / mmap)
//    • pthreads: one dedicated thread per player character
//    • Named FIFO / pipe    (mkfifo / open / read / write)
//    • SIGUSR1 per-thread delivery via pthread_kill
//    • Async-safe sleep(3) inside signal handler for 3-s stun
//    • PTHREAD_PROCESS_SHARED mutex for ring-buffer access
// ============================================================

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
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
//   The Arbiter sends SIGUSR1 to this specific thread via:
//       pthread_kill(gs->hip_thread_tids[player_idx], SIGUSR1)
//
//   Calling sleep(3) here suspends ONLY this thread for 3 seconds
//   from the exact moment of interruption — it does not affect any
//   other player thread, the main thread, or any other process.
//
//   sleep() is listed as async-signal-safe by POSIX and is therefore
//   legal to call inside a signal handler.
//
//   When sleep(3) returns, the handler exits and the OS resumes this
//   thread at the instruction that was interrupted (typically the
//   read() call in the event loop, which will return EINTR — we handle
//   that by simply retrying the read, effectively waiting for the next
//   turn assignment from the Arbiter).
//
static void stun_handler(int sig) {
    (void)sig;
    sleep(3);  // halt this thread — and only this thread — for 3 seconds
}

static void sigterm_handler(int sig) {
    (void)sig;
    running = false;
}

// ─── Attach to shared memory ──────────────────────────────────────────────────
static void attach_shared_memory() {
    int fd = -1;
    for (int i = 0; i < 10 && fd == -1; i++) {
        fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd == -1) sleep(1);
    }
    if (fd == -1) { perror("HIP: shm_open"); exit(1); }

    gs = (GameState*)mmap(nullptr, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("HIP: mmap"); exit(1); }
    close(fd);
}

// ─── Push to player action ring-buffer (called from player thread) ────────────
static void push_player_action(int player_idx, int action, int target, int weapon_id) {
    pthread_mutex_lock(&gs->memory_lock);
    PlayerActionBuffer& buf = gs->player_buffers[player_idx];
    if (buf.count < ACTION_BUF_CAP) {
        PlayerInputEvent& ev = buf.events[buf.tail];
        ev.action    = action;
        ev.target    = target;
        ev.weapon_id = weapon_id;
        ev.valid     = true;
        buf.tail  = (buf.tail + 1) % ACTION_BUF_CAP;
        buf.count++;
    }
    // If full, silently drop — this cannot happen in normal play because
    // the Arbiter drains the buffer before granting the next turn.
    pthread_mutex_unlock(&gs->memory_lock);
}

// ─── Open the read end of the HIP FIFO ───────────────────────────────────────
static int open_hip_pipe_read() {
    int fd = -1;
    for (int i = 0; i < 30 && fd == -1; i++) {
        fd = open(HIP_PIPE_PATH, O_RDONLY);
        if (fd == -1) usleep(300000);
    }
    if (fd == -1) { perror("HIP: open pipe read"); exit(1); }
    return fd;
}

// ─── Player thread ─────────────────────────────────────────────────────────────
struct PlayerThreadArg {
    int player_idx;
    int pipe_fd;        // shared read-end of the named FIFO
};

static void* player_thread_func(void* arg) {
    PlayerThreadArg* a   = (PlayerThreadArg*)arg;
    const int idx        = a->player_idx;
    const int pipefd     = a->pipe_fd;

    // ── 1. Register this thread's tid so Arbiter can pthread_kill() it ────
    gs->hip_thread_tids[idx] = pthread_self();

    // ── 2. Unblock SIGUSR1 in this thread only ─────────────────────────────
    // The main thread (and therefore all threads by inheritance) has SIGUSR1
    // blocked.  We selectively unblock it here so that only the correct
    // player thread can receive a stun signal.
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

    cout << "HIP: Player " << idx + 1
         << " thread ready  (tid=" << pthread_self() << ")" << endl;

    while (running && !gs->is_game_over) {

        // ── Wait for this player's turn ────────────────────────────────────
        while (running && !gs->is_game_over && gs->active_turn != idx)
            usleep(20000);

        if (!running || gs->is_game_over) break;
        if (!gs->players[idx].is_alive)   { usleep(50000); continue; }

        // ── Read one UIEvent addressed to this player from the FIFO ────────
        //
        // The Arbiter render thread writes exactly one UIEvent per player
        // action click into HIP_PIPE_PATH.  Because all player threads share
        // the same file descriptor, we discard events addressed to other
        // players and retry.
        //
        // If SIGUSR1 fires (stun) while we are blocked in read(), the system
        // call returns -1 / EINTR.  sleep(3) has already run inside the
        // stun_handler at that point.  We simply retry the read, which means
        // the thread resumes polling for its event after the stun expires.
        //
        UIEvent uev;
        bool got = false;
        while (!got && running && !gs->is_game_over && gs->active_turn == idx) {
            ssize_t n = read(pipefd, &uev, sizeof(UIEvent));
            if (n == (ssize_t)sizeof(UIEvent)) {
                if (uev.player_idx == idx)
                    got = true;
                // else: event for a different player — discard and retry
            } else if (n == -1 && errno == EINTR) {
                // Interrupted by SIGUSR1 stun — sleep(3) already ran.
                // Retry the read; if the turn has moved on, outer loop exits.
                continue;
            } else {
                usleep(10000);
            }
        }

        if (!got) continue;

        // ── Push the resolved action into shared memory ────────────────────
        push_player_action(idx, uev.action, uev.target, uev.weapon_id);

        // Small yield so Arbiter can detect the new entry promptly.
        usleep(10000);
    }

    cout << "HIP: Player " << idx + 1 << " thread exiting." << endl;
    return nullptr;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    // ── Block SIGUSR1 in main (and all threads by inheritance) ────────────
    // Each player thread individually unblocks it for itself, ensuring that
    // only the targeted player thread ever receives the stun signal.
    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr);

    // Install the per-thread stun handler.
    // SA_RESTART is intentionally omitted so that read() returns EINTR,
    // letting the player thread detect and handle the stun cleanly.
    struct sigaction sa{};
    sa.sa_handler = stun_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    sigaction(SIGUSR1, &sa, nullptr);

    signal(SIGTERM, sigterm_handler);

    cout << "HIP: Connecting to shared memory..." << endl;
    attach_shared_memory();

    gs->hip_pid = getpid();
    for (int i = 0; i < gs->num_players; i++)
        gs->players[i].pid = getpid();

    // Initialise action buffers and zero out thread-tid array.
    memset(gs->hip_thread_tids, 0, sizeof(gs->hip_thread_tids));
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->player_buffers[i].head  = 0;
        gs->player_buffers[i].tail  = 0;
        gs->player_buffers[i].count = 0;
    }

    cout << "HIP: Connected.  Players = " << gs->num_players << endl;
    cout << "HIP: Opening event pipe from Arbiter..." << endl;
    int pipe_fd = open_hip_pipe_read();
    cout << "HIP: Pipe open.  Spawning " << gs->num_players
         << " player thread(s)." << endl;

    // Spawn one pthread per player.
    pthread_t         tids[MAX_PLAYERS];
    PlayerThreadArg   args[MAX_PLAYERS];

    for (int i = 0; i < gs->num_players; i++) {
        args[i].player_idx = i;
        args[i].pipe_fd    = pipe_fd;
        pthread_create(&tids[i], nullptr, player_thread_func, &args[i]);
    }

    // Wait until the game ends.
    while (running && !gs->is_game_over)
        usleep(100000);

    running = false;

    for (int i = 0; i < gs->num_players; i++)
        pthread_join(tids[i], nullptr);

    close(pipe_fd);
    munmap(gs, sizeof(GameState));
    cout << "HIP: exiting." << endl;
    return 0;
}