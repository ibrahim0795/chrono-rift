// ============================================================
//  asp.cpp  —  Automated Strategic Process (AI)
//
//  UPDATED:
//    • Removed kill(getpid(), SIGSTOP) from stun handler.
//      Stun is now purely flag-based via is_stunned in shared memory.
//      The arbiter sets/clears the flag with a timer thread.
//    • Enemy threads check is_stunned flag before acting.
//    • All existing OS concepts preserved.
//
//  OS Concepts used:
//    • POSIX Shared Memory  (shm_open, mmap)
//    • pthreads: one dedicated thread per enemy NPC
//    • SIGSTOP/SIGCONT: ultimate ability pause (sent by arbiter)
//    • SIGUSR1: stun signal (now flag-based — no self-stop)
//    • Mutex: protect shared state access
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
#include "../common.h"

using namespace std;

// ─── Globals ──────────────────────────────────────────────────────────────────
GameState* gs      = nullptr;
bool       running = true;

// ─── Signal handlers ──────────────────────────────────────────────────────────

// Stun: now purely flag-based. Arbiter sets is_stunned=true and clears after 3s.
// We no longer self-stop here — that would freeze ALL enemy threads, not just one.
void stun_handler(int sig) {
    (void)sig;
    // Flag is handled by arbiter. Nothing to do here.
}

void sigterm_handler(int sig) {
    (void)sig;
    running = false;
}

// SIGSTOP/SIGCONT for ultimate ability are handled by the OS automatically.

// ─── Enemy AI decision ────────────────────────────────────────────────────────
// Simple AI: always strike the first alive player (80% chance), else skip.
void enemy_decide(int enemy_idx) {
    int target = -1;
    for (int i = 0; i < gs->num_players; i++) {
        if (gs->players[i].is_alive) { target = i; break; }
    }
    if (target == -1) return;

    int roll = rand() % 10;
    if (roll < 8) {
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

// ─── Enemy thread — one per NPC ───────────────────────────────────────────────
struct EnemyThreadArg { int enemy_index; };

void* enemy_thread_func(void* arg) {
    EnemyThreadArg* earg = (EnemyThreadArg*)arg;
    int idx = earg->enemy_index;

    while (running && !gs->is_game_over) {
        // Safety: bail if this slot is dead
        if (idx >= gs->num_enemies) { usleep(100000); continue; }
        Entity& e = gs->enemies[idx];
        if (!e.is_alive) { usleep(100000); continue; }

        // Only act on this enemy's turn
        int my_turn = MAX_PLAYERS + idx;
        if (gs->active_turn != my_turn) {
            usleep(50000);
            continue;
        }

        // Stunned: skip turn (arbiter will advance turn via timeout)
        if (e.is_stunned) {
            gs->action_actor   = my_turn;
            gs->pending_action = ACTION_SKIP;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Enemy %d is stunned — skips!", idx + 1);
            // Wait for arbiter to clear action
            while (gs->pending_action != ACTION_NONE && !gs->is_game_over)
                usleep(20000);
            continue;
        }

        // Decide and write action
        pthread_mutex_lock(&gs->memory_lock);
        enemy_decide(idx);
        pthread_mutex_unlock(&gs->memory_lock);

        // Wait until arbiter processes our action
        while (gs->pending_action != ACTION_NONE && !gs->is_game_over)
            usleep(20000);
    }

    return nullptr;
}

// ─── Attach to shared memory ──────────────────────────────────────────────────
void attach_shared_memory() {
    int fd = -1;
    for (int i = 0; i < 10; i++) {
        fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd != -1) break;
        sleep(1);
    }
    if (fd == -1) { perror("ASP: shm_open"); exit(1); }
    gs = (GameState*)mmap(0, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("ASP: mmap"); exit(1); }
    close(fd);
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGUSR1,  stun_handler);
    signal(SIGTERM,  sigterm_handler);
    // SIGSTOP/SIGCONT handled by OS for ultimate ability pause

    cout << "ASP: Connecting to shared memory..." << endl;
    attach_shared_memory();

    gs->asp_pid = getpid();
    srand(ROLL_SEED + 1);
    cout << "ASP: Connected. Enemies = " << gs->num_enemies << endl;

    // Pre-create MAX_ENEMIES threads (some may idle if num_enemies < MAX_ENEMIES)
    pthread_t threads[MAX_ENEMIES];
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