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

// Globals 
static GameState* gs      = nullptr;
static bool       running = true;


static void stun_handler(int sig) {
    (void)sig;
    sleep(3); 
}

static void sigterm_handler(int sig) {
    (void)sig;
    running = false;
}

// SIGSTOP / SIGCONT for the ultimate ability are delivered to the
// entire ASP process and handled automatically by the OS.

// Enemy AI decision
// strike the first alive player with 80% probability, else skip.
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

// Enemy thread
struct EnemyThreadArg { int enemy_index; };

static void* enemy_thread_func(void* arg) {
    EnemyThreadArg* earg = (EnemyThreadArg*)arg;
    const int idx = earg->enemy_index;

    //Register this thread's tid for targeted stun signals
    gs->asp_thread_tids[idx] = pthread_self();

    // Unblock SIGUSR1 in this thread only 
    // main() blocks SIGUSR1 process-wide.  Each enemy thread unblocks it
    // for itself so only the correct thread receives the stun signal.
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr) != 0)
        perror("ASP: pthread_sigmask SIG_UNBLOCK");

    while (running && !gs->is_game_over) {
        if (idx >= gs->num_enemies) { usleep(100000); continue; }

        Entity& e = gs->enemies[idx];
        if (!e.is_alive)  { usleep(100000); continue; }

        // Only act when it is this enemy's turn.
        int my_turn = MAX_PLAYERS + idx;
        if (gs->active_turn != my_turn) { usleep(50000); continue; }

        // Decide and post action 
        pthread_mutex_lock(&gs->memory_lock);
        enemy_decide(idx);
        pthread_mutex_unlock(&gs->memory_lock);

        // Wait for the Arbiter to consume and clear the action.
        while (gs->pending_action != ACTION_NONE && !gs->is_game_over)
            usleep(20000);
    }

    return nullptr;
}

// Attach to shared memory 
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
    if (close(fd) == -1) perror("ASP: close shm fd");
}

// main 
int main() {
    //Block SIGUSR1 process-wide; threads unblock it individually 
    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr) != 0) {
        perror("ASP: pthread_sigmask SIG_BLOCK"); exit(1);
    }

    struct sigaction sa{};
    sa.sa_handler = stun_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;
    if (sigaction(SIGUSR1, &sa, nullptr) == -1) {
        perror("ASP: sigaction SIGUSR1"); exit(1);
    }

    if (signal(SIGTERM, sigterm_handler) == SIG_ERR) {
        perror("ASP: signal SIGTERM"); exit(1);
    }

    cout << "ASP: Connecting to shared memory..." << endl;
    attach_shared_memory();

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