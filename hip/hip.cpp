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
#include "../shared/shared_utils.h"
#include "../game_logic.h"

using namespace std;

// Process-level globals 
static GameState* gs      = nullptr;
static bool       running = true;


static void push_player_action_locked(int player_idx, int action, int target, int weapon_id)
{
    pthread_mutex_lock(&gs->memory_lock);
    push_player_action(gs, player_idx, action, target, weapon_id);
    pthread_mutex_unlock(&gs->memory_lock);
}

// Player thread 
struct PlayerThreadArg { int player_idx; };

static void* player_thread_func(void* arg) {
    const int idx = ((PlayerThreadArg*)arg)->player_idx;

    // register this pthread_t so Arbiter can target it
    gs->hip_thread_tids[idx] = pthread_self();

    // unblock SIGUSR1 in this thread only
    unblock_sigusr1_current_thread();

    cout << "HIP: Player " << idx + 1
         << " thread ready  (tid=" << pthread_self() << ")" << endl;

    while (running && !gs->is_game_over) {

        // Skip dead players immediately
        if (!gs->players[idx].is_alive) {
            usleep(50000);
            continue;
        }

        UIEvent uev{};
        bool    got_event = false;

        pthread_mutex_lock(&gs->memory_lock);

        int found_idx = -1;
        for (int i = 0; i < gs->ui_events.count; i++) {
            if (gs->ui_events.events[i].player_idx == idx) {
                found_idx = i;
                uev = gs->ui_events.events[i];
                break;
            }
        }

        if (found_idx != -1) {
            // Remove the event with an in-place left-shift of the remainder.
            for (int j = found_idx; j < gs->ui_events.count - 1; j++)
                gs->ui_events.events[j] = gs->ui_events.events[j + 1];
            gs->ui_events.count--;
            got_event = true;
        }

        pthread_mutex_unlock(&gs->memory_lock);

        if (got_event) {
            // Forward the resolved action to the Arbiter via the ring-buffer.
            push_player_action_locked(idx, uev.action, uev.target, uev.weapon_id);
        } else {
            usleep(10000);   // 10 ms poll interval
        }
    }

    cout << "HIP: Player " << idx + 1 << " thread exiting." << endl;
    return nullptr;
}

// main
int main() {
    install_stun_and_term_handlers(&running);

    // Attach to shared memory created by Arbiter
    cout << "HIP: Connecting to shared memory..." << endl;
    gs = attach_shared_memory("HIP");

    gs->hip_pid = getpid();
    for (int i = 0; i < gs->num_players; i++)
        gs->players[i].pid = getpid();

    // Zero thread-tid array; each thread fills its own slot at startup.
    memset(gs->hip_thread_tids, 0, sizeof(gs->hip_thread_tids));

    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->player_buffers[i].head  = 0;
        gs->player_buffers[i].tail  = 0;
        gs->player_buffers[i].count = 0;
    }

    cout << "HIP: Connected.  Players = " << gs->num_players << endl;
    cout << "HIP: Spawning " << gs->num_players << " player thread(s)." << endl;

    // Spawn one pthread per player 
    pthread_t       tids[MAX_PLAYERS];
    PlayerThreadArg args[MAX_PLAYERS];

    for (int i = 0; i < gs->num_players; i++) {
        args[i].player_idx = i;
        int rc = pthread_create(&tids[i], nullptr, player_thread_func, &args[i]);
        if (rc != 0) {
            cerr << "HIP: failed to create player thread " << i << ": "
                 << strerror(rc) << endl;
            exit(1);
        }
    }

    // Wait for game end or SIGTERM 
    while (running && !gs->is_game_over)
        usleep(100000);

    running = false;   // signal threads to exit their poll loops

    for (int i = 0; i < gs->num_players; i++)
        pthread_join(tids[i], nullptr);

    if (munmap(gs, sizeof(GameState)) == -1)
        perror("HIP: munmap");
    cout << "HIP: exiting." << endl;
    return 0;
}
