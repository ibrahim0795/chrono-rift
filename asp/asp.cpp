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
#include "../shared/shared_utils.h"
#include "../game_logic.h"

using namespace std;

// Globals 
static GameState* gs      = nullptr;
static bool       running = true;

// SIGSTOP / SIGCONT for the ultimate ability are delivered to the
// entire ASP process and handled automatically by the OS.

// enemy_decide now lives in game_logic.cpp

// Enemy thread
struct EnemyThreadArg { int enemy_index; };

static void* enemy_thread_func(void* arg) {
    EnemyThreadArg* earg = (EnemyThreadArg*)arg;
    const int idx = earg->enemy_index;

    //Register this thread's tid for targeted stun signals
    gs->asp_thread_tids[idx] = pthread_self();

    // Unblock SIGUSR1 in this thread so only the correct thread
    // receives the stun signal.
    unblock_sigusr1_current_thread();

    while (running && !gs->is_game_over) {
        if (idx >= gs->num_enemies) { usleep(100000); continue; }

        Entity& e = gs->enemies[idx];
        if (!e.is_alive)  { usleep(100000); continue; }

        // Only act when it is this enemy's turn.
        int my_turn = MAX_PLAYERS + idx;
        if (gs->active_turn != my_turn) { usleep(50000); continue; }

        // Decide and post action 
        pthread_mutex_lock(&gs->memory_lock);
        enemy_decide(gs, idx);
        pthread_mutex_unlock(&gs->memory_lock);

        // Wait for the Arbiter to consume and clear the action.
        while (gs->pending_action != ACTION_NONE && !gs->is_game_over)
            usleep(20000);
    }

    return nullptr;
}

// main 
int main() {
    install_stun_and_term_handlers(&running);

    cout << "ASP: Connecting to shared memory..." << endl;
    gs = attach_shared_memory("ASP");

    gs->asp_pid = getpid();
    srand(ROLL_SEED + 1);

    // Zero out thread-tid array, Arbiter checks for 0 before pthread_kill.
    memset(gs->asp_thread_tids, 0, sizeof(gs->asp_thread_tids));

    cout << "ASP: Connected.  Enemies = " << gs->num_enemies << endl;

    // Spawn one thread per enemy slot (extras idle if num_enemies < MAX_ENEMIES).
    pthread_t      threads[MAX_ENEMIES];
    EnemyThreadArg args[MAX_ENEMIES];

    for (int i = 0; i < MAX_ENEMIES; i++) {
        args[i].enemy_index = i;
        int rc = pthread_create(&threads[i], nullptr, enemy_thread_func, &args[i]);
        if (rc != 0) {
            cerr << "ASP: failed to create enemy thread " << i << ": "
                 << strerror(rc) << endl;
            exit(1);
        }
    }

    for (int i = 0; i < MAX_ENEMIES; i++)
        pthread_join(threads[i], nullptr);

    if (munmap(gs, sizeof(GameState)) == -1)
        perror("ASP: munmap");
    cout << "ASP: exiting." << endl;
    return 0;
}
