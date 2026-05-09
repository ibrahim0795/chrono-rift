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

// Process-level globals 
static GameState* gs      = nullptr;
static bool       running = true;

static void stun_handler(int) {
    sleep(3);  
}

static void sigterm_handler(int) {
    running = false;
}

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

static void push_player_action(int player_idx, int action, int target, int weapon_id)
{
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
    pthread_mutex_unlock(&gs->memory_lock);
}

// Player thread 
struct PlayerThreadArg { int player_idx; };

static void* player_thread_func(void* arg) {
    const int idx = ((PlayerThreadArg*)arg)->player_idx;

    // register this pthread_t so Arbiter can target it
    gs->hip_thread_tids[idx] = pthread_self();

    // unblock SIGUSR1 in this thread only
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

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
            push_player_action(idx, uev.action, uev.target, uev.weapon_id);
        } else {
            usleep(10000);   // 10 ms poll interval
        }
    }

    cout << "HIP: Player " << idx + 1 << " thread exiting." << endl;
    return nullptr;
}

// main
int main() {

    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr);

    struct sigaction sa{};
    sa.sa_handler = stun_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;   // no SA_RESTART
    sigaction(SIGUSR1, &sa, nullptr);

    signal(SIGTERM, sigterm_handler);

    // Attach to shared memory created by Arbiter
    cout << "HIP: Connecting to shared memory..." << endl;
    attach_shared_memory();

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
        pthread_create(&tids[i], nullptr, player_thread_func, &args[i]);
    }

    // Wait for game end or SIGTERM 
    while (running && !gs->is_game_over)
        usleep(100000);

    running = false;   // signal threads to exit their poll loops

    for (int i = 0; i < gs->num_players; i++)
        pthread_join(tids[i], nullptr);

    munmap(gs, sizeof(GameState));
    cout << "HIP: exiting." << endl;
    return 0;
}