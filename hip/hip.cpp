// ============================================================
//  hip.cpp  —  Human Interfacing Process
//  OS Concepts used:
//    • POSIX Shared Memory  (shm_open, mmap)
//    • pthreads: one thread per player character
//    • SIGUSR1: stun signal handler
//    • Mutex: only active player thread writes action
// ============================================================

#include <iostream>
#include <cstring>
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

// ─── Signal handler — stun ────────────────────────────────────────────────────
void stun_handler(int sig) {
    if (sig == SIGUSR1) {
        kill(getpid(), SIGSTOP);
    }
}

// ─── Player thread ────────────────────────────────────────────────────────────
struct PlayerThreadArg { int player_index; };

void print_menu(int player_index) {
    Entity& p = gs->players[player_index];
    cout << "\n=== Player " << player_index + 1 << " Turn ===" << endl;
    cout << "HP: " << p.hp << "/" << p.max_hp
         << "  Stamina: " << p.stamina << "/" << p.max_stamina << endl;
    cout << "Actions:" << endl;
    cout << "  1. Strike      (reduce enemy HP)" << endl;
    cout << "  2. Exhaust     (reduce enemy stamina)" << endl;
    cout << "  3. Use Weapon  (use inventory weapon)" << endl;
    cout << "  4. Swap In     (retrieve from long-term storage)" << endl;
    cout << "  5. Heal        (restore 10% HP)" << endl;
    cout << "  6. Skip        (stamina -> 50%)" << endl;
    cout << "  7. Quit game" << endl;

    // Show inventory
    cout << "Inventory: ";
    for (int s = 0; s < INVENTORY_SIZE; s++) {
        if (p.inventory[s] != WPN_NONE)
            cout << "[" << s << "]=" << p.inventory[s] << " ";
    }
    cout << endl;

    // Show enemies
    cout << "Enemies:" << endl;
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity& e = gs->enemies[i];
        if (e.is_alive)
            cout << "  " << i << ". HP:" << e.hp << endl;
    }
}

void* player_thread_func(void* arg) {
    PlayerThreadArg* parg = (PlayerThreadArg*)arg;
    int idx = parg->player_index;

    while (running && !gs->is_game_over) {
        // Only act when it's this player's turn
        if (gs->active_turn != idx) {
            usleep(50000);
            continue;
        }

        // Stunned — skip this turn
        if (gs->players[idx].is_stunned) {
            gs->pending_action = ACTION_SKIP;
            gs->action_actor   = idx;
            continue;
        }

        print_menu(idx);

        int choice = 0;
        cout << "Choice: ";
        cin >> choice;

        gs->action_actor = idx;

        switch (choice) {
            case 1: {
                int target;
                cout << "Target enemy index: ";
                cin >> target;
                gs->action_target  = MAX_PLAYERS + target;
                gs->pending_action = ACTION_STRIKE;
                break;
            }
            case 2: {
                int target;
                cout << "Target enemy index: ";
                cin >> target;
                gs->action_target  = MAX_PLAYERS + target;
                gs->pending_action = ACTION_EXHAUST;
                break;
            }
            case 3: {
                int wpn;
                cout << "Weapon ID to use: ";
                cin >> wpn;
                int target;
                cout << "Target enemy index: ";
                cin >> target;
                gs->action_weapon_id = wpn;
                gs->action_target    = MAX_PLAYERS + target;
                gs->pending_action   = ACTION_USE_WEAPON;
                break;
            }
            case 4: {
                int lt_idx;
                cout << "Long-term storage slot to swap in: ";
                cin >> lt_idx;
                gs->action_weapon_id = lt_idx;
                gs->pending_action   = ACTION_SWAP_IN;
                break;
            }
            case 5:
                gs->pending_action = ACTION_HEAL;
                break;
            case 6:
                gs->pending_action = ACTION_SKIP;
                break;
            case 7:
                gs->pending_action = ACTION_QUIT;
                kill(gs->arbiter_pid, SIGTERM);
                running = false;
                break;
            default:
                gs->pending_action = ACTION_SKIP;
                break;
        }
    }

    return nullptr;
}

// ─── Attach to shared memory ─────────────────────────────────────────────────
void attach_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
    if (fd == -1) { perror("HIP: shm_open"); exit(1); }
    gs = (GameState*)mmap(0, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("HIP: mmap"); exit(1); }
    close(fd);
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGUSR1, stun_handler);

    cout << "HIP: Connecting to shared memory..." << endl;
    // Retry until arbiter creates the shared memory
    for (int i = 0; i < 10; i++) {
        int fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd != -1) { close(fd); break; }
        sleep(1);
    }
    attach_shared_memory();

    gs->hip_pid = getpid();
    cout << "HIP: Connected. Players = " << gs->num_players << endl;

    // Register this process PID for all players
    for (int i = 0; i < gs->num_players; i++)
        gs->players[i].pid = getpid();

    // Create one thread per player
    pthread_t threads[MAX_PLAYERS];
    PlayerThreadArg args[MAX_PLAYERS];

    for (int i = 0; i < gs->num_players; i++) {
        args[i].player_index = i;
        pthread_create(&threads[i], nullptr, player_thread_func, &args[i]);
    }

    for (int i = 0; i < gs->num_players; i++)
        pthread_join(threads[i], nullptr);

    munmap(gs, sizeof(GameState));
    cout << "HIP: exiting." << endl;
    return 0;
}