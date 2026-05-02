#include <iostream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include "../common.h"

using namespace std;

int main() {
    cout << "Arbiter is online!" << endl;

    // Create shared memory
    int shm_fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    ftruncate(shm_fd, sizeof(GameState));
    GameState* gs = (GameState*)mmap(0, sizeof(GameState),
                    PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    // Init mutex as process-shared
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gs->memory_lock, &attr);

    // Ask player party size
    cout << "Enter number of players (1-4): ";
    cin >> gs->num_players;

    // Seed with roll number
    srand(795);  // last 3 digits of your roll number

    // Init players
    for (int i = 0; i < gs->num_players; i++) {
        gs->players[i].hp = 23795 + (rand() % 901 + 100);
        gs->players[i].max_stamina = 100;
        gs->players[i].stamina = 0;
        gs->players[i].damage = 5 + 10;  // last digit of roll (5) + 10
        gs->players[i].speed = 100 / gs->num_players;
        gs->players[i].is_alive = true;
        gs->players[i].is_stunned = false;
        memset(gs->players[i].inventory, -1, sizeof(gs->players[i].inventory));
    }

    // Init enemies (random 2-9)
    gs->num_enemies = rand() % 8 + 2;
    for (int i = 0; i < gs->num_enemies; i++) {
        gs->enemies[i].hp = 95 + (rand() % 151 + 50);  // last 2 digits (95) + 50-200
        gs->enemies[i].max_stamina = 150;
        gs->enemies[i].stamina = 0;
        gs->enemies[i].damage = 9 + 10;  // second last digit (9) + 10
        gs->enemies[i].speed = rand() % 21 + 10;
        gs->enemies[i].is_alive = true;
        gs->enemies[i].is_stunned = false;
    }

    gs->is_game_over = false;
    gs->enemies_killed = 0;

    cout << "Players: " << gs->num_players << ", Enemies: " << gs->num_enemies << endl;

    // Keep alive until game over
    while (!gs->is_game_over) {
        sleep(1);
    }

    // Cleanup
    pthread_mutex_destroy(&gs->memory_lock);
    munmap(gs, sizeof(GameState));
    shm_unlink(SHARED_MEM_NAME);
    return 0;
}