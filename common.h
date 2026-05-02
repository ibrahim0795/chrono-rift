#ifndef COMMON_H
#define COMMON_H

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <signal.h>

// ─── Constants ───────────────────────────────────────────────────────────────
#define MAX_PLAYERS       4
#define MAX_ENEMIES       9
#define INVENTORY_SIZE    20
#define LONG_TERM_SIZE    50
#define MAX_ACTION_LOG    512
#define SHARED_MEM_NAME   "/chronorift_mem"

// Roll number: 23I-0795  →  seed=795, last digit=5, second-last=9, last2=95
#define ROLL_SEED         795
#define LAST_DIGIT        5
#define SECOND_LAST       9
#define LAST_TWO          95
#define ROLL_FULL         230795   // used for player HP base

// ─── Weapon IDs ──────────────────────────────────────────────────────────────
#define WPN_NONE          -1
#define WPN_SOLAR_CORE     0   // 10 slots, 95 dmg  (artifact)
#define WPN_LUNAR_BLADE    1   // 10 slots, 90 dmg  (artifact)
#define WPN_IRON_HALBERD   2   //  7 slots, 55 dmg
#define WPN_VENOM_DAGGER   3   //  4 slots, 30 dmg
#define WPN_THUNDERSTAFF   4   //  6 slots, 50 dmg
#define WPN_OBSIDIAN_AXE   5   //  5 slots, 45 dmg
#define WPN_FROSTBOW       6   //  6 slots, 48 dmg
#define WPN_SPLINTER_STICK 7   //  2 slots, 12 dmg
#define WPN_ECLIPSE_RELIC  8   // dynamic artifact
#define WPN_COUNT          9

// Weapon table (slot size, damage)
static const int WPN_SLOTS[WPN_COUNT]  = {10,10, 7, 4, 6, 5, 6, 2, 10};
static const int WPN_DAMAGE[WPN_COUNT] = {95,90,55,30,50,45,48,12, 80};

// ─── Action codes ────────────────────────────────────────────────────────────
#define ACTION_NONE        0
#define ACTION_STRIKE      1   // reduce enemy HP
#define ACTION_EXHAUST     2   // reduce enemy stamina
#define ACTION_USE_WEAPON  3   // attack with inventory weapon
#define ACTION_SWAP_IN     4   // retrieve weapon from long-term storage
#define ACTION_HEAL        5   // restore 10% HP
#define ACTION_SKIP        6   // skip turn (stamina -> 50%)
#define ACTION_QUIT        7   // SIGTERM to arbiter

// ─── Entity struct ────────────────────────────────────────────────────────────
struct Entity {
    bool  is_alive;
    bool  is_stunned;
    int   hp;
    int   max_hp;
    int   stamina;          // current stamina (fills up to max_stamina)
    int   max_stamina;      // 100 for players, 150 for enemies
    int   damage;
    int   speed;            // stamina added per tick

    // Inventory: primary (20 slots) and long-term storage
    int   inventory[INVENTORY_SIZE];    // WPN_NONE = empty slot
    int   long_term[LONG_TERM_SIZE];    // weapons swapped out
    int   lt_count;                     // how many items in long_term

    // Which player/enemy index this is (for signal targets)
    pid_t pid;
};

// ─── Shared game state ───────────────────────────────────────────────────────
struct GameState {
    // Synchronization — PTHREAD_PROCESS_SHARED mutex
    pthread_mutex_t memory_lock;

    // Processes
    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;

    // Entities
    int    num_players;
    Entity players[MAX_PLAYERS];

    int    num_enemies;
    Entity enemies[MAX_ENEMIES];
    int    enemies_killed;       // game ends at 10

    // Game status
    bool   is_game_over;
    bool   player_won;
    bool   ultimate_pause;       // ASP is paused by Ultimate Ability

    // Scheduling: whose turn it is
    // 0..MAX_PLAYERS-1 = player index
    // MAX_PLAYERS..MAX_PLAYERS+MAX_ENEMIES-1 = enemy index
    int    active_turn;          // set by Arbiter after stamina fills

    // Action pipeline: HIP/ASP write here, Arbiter reads & executes
    int    pending_action;       // ACTION_* code
    int    action_actor;         // entity index (same encoding as active_turn)
    int    action_target;        // target entity index
    int    action_weapon_id;     // for USE_WEAPON / SWAP_IN

    // Artifact resource table
    bool   solar_core_exists;
    int    solar_core_holder;    // -1 = free, else entity index

    bool   lunar_blade_exists;
    int    lunar_blade_holder;

    bool   eclipse_relic_exists; // introduced dynamically
    int    eclipse_relic_holder;

    // Action log (latest message, printed by SFML thread)
    char   action_log[MAX_ACTION_LOG];
};

#endif // COMMON_H