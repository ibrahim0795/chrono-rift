// ============================================================
//  asp.cpp  —  Automated Strategic Process (AI)
//  OS Concepts used:
//    • POSIX Shared Memory  (shm_open, mmap)
//    • pthreads: one dedicated thread per enemy NPC
//    • SIGSTOP/SIGCONT: ultimate ability pause (sent by arbiter)
//    • SIGUSR1: stun signal
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

// ─── Globals ─────────────────────────────────────────────────────────────────
GameState* gs      = nullptr;
bool       running = true;

// ─── Stun handler ────────────────────────────────────────────────────────────
void stun_handler(int sig) {
    if (sig == SIGUSR1) {
        kill(getpid(), SIGSTOP);
    }
}

// ─── Enemy AI decision ───────────────────────────────────────────────────────
// Simple AI: always strike a random alive player
void enemy_decide(int enemy_idx) {
    // Find a random alive player
    int target = -1;
    for (int i = 0; i < gs->num_players; i++) {
        if (gs->players[i].is_alive) { target = i; break; }
    }
    if (target == -1) return;

    // 80% strike, 20% skip
    int roll = rand() % 10;
    if (roll < 8) {
        gs->action_actor   = MAX_PLAYERS + enemy_idx;
        gs->action_target  = target;
        gs->pending_action = ACTION_STRIKE;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d attacks Player %d!", enemy_idx, target + 1);
    } else {
        gs->action_actor   = MAX_PLAYERS + enemy_idx;
        gs->pending_action = ACTION_SKIP;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d skips.", enemy_idx);
    }
}

// ─── Enemy thread — one per NPC ──────────────────────────────────────────────
struct EnemyThreadArg { int enemy_index; };

void* enemy_thread_func(void* arg) {
    EnemyThreadArg* earg = (EnemyThreadArg*)arg;
    int idx = earg->enemy_index;

    while (running && !gs->is_game_over) {
        Entity& e = gs->enemies[idx];

        if (!e.is_alive) break;

        // Only act when it's this enemy's turn
        int my_turn = MAX_PLAYERS + idx;
        if (gs->active_turn != my_turn) {
            usleep(50000);
            continue;
        }

        // Stunned — skip
        if (e.is_stunned) {
            gs->action_actor   = my_turn;
            gs->pending_action = ACTION_SKIP;
            continue;
        }

        // Decide action (AI logic)
        pthread_mutex_lock(&gs->memory_lock);
        enemy_decide(idx);
        pthread_mutex_unlock(&gs->memory_lock);

        // Wait until arbiter processes our action
        while (gs->pending_action != ACTION_NONE && !gs->is_game_over)
            usleep(20000);
    }

    return nullptr;
}

// ─── Attach to shared memory ─────────────────────────────────────────────────
void attach_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
    if (fd == -1) { perror("ASP: shm_open"); exit(1); }
    gs = (GameState*)mmap(0, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("ASP: mmap"); exit(1); }
    close(fd);
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGUSR1, stun_handler);
    // SIGSTOP/SIGCONT are handled by the OS automatically for the
    // Ultimate Ability pause — no handler needed

    cout << "ASP: Connecting to shared memory..." << endl;
    for (int i = 0; i < 10; i++) {
        int fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd != -1) { close(fd); break; }
        sleep(1);
    }
    attach_shared_memory();

    gs->asp_pid = getpid();
    srand(ROLL_SEED + 1);
    cout << "ASP: Connected. Enemies = " << gs->num_enemies << endl;

    // Create one thread per enemy
    pthread_t threads[MAX_ENEMIES];
    EnemyThreadArg args[MAX_ENEMIES];

    for (int i = 0; i < gs->num_enemies; i++) {
        args[i].enemy_index = i;
        pthread_create(&threads[i], nullptr, enemy_thread_func, &args[i]);
    }

    for (int i = 0; i < gs->num_enemies; i++)
        pthread_join(threads[i], nullptr);

    munmap(gs, sizeof(GameState));
    cout << "ASP: exiting." << endl;
    return 0;
}