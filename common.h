#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>

#define MAX_PLAYERS 4
#define MAX_ENEMIES 9
#define INVENTORY_SIZE 20
#define SHARED_MEM_NAME "/chronorift_mem"

struct Entity {
    int hp;
    int stamina;
    int max_stamina;
    int damage;
    int speed;
    bool is_alive;
    bool is_stunned;
    int inventory[INVENTORY_SIZE];  // weapon IDs, -1 = empty
};

struct GameState {
    pthread_mutex_t memory_lock;

    // Players
    int num_players;
    Entity players[MAX_PLAYERS];

    // Enemies
    int num_enemies;
    Entity enemies[MAX_ENEMIES];

    // Game status
    bool is_game_over;
    bool player_won;
    int enemies_killed;

    // Whose turn
    int active_entity;   // index into combined schedule

    // Artifact table
    bool solar_core_held;   int solar_core_owner;
    bool lunar_blade_held;  int lunar_blade_owner;
    bool eclipse_relic_held; int eclipse_relic_owner;
    bool eclipse_relic_exists;

    // Action log (latest action as a string)
    char action_log[256];
};

#endif