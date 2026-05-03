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
#define ROLL_FULL         795   // used for player HP base

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
#define WPN_ECLIPSE_RELIC  8   // 10 slots, 80 dmg  (dynamic artifact)
#define WPN_COUNT          9

// Weapon table (slot size, damage)
static const int WPN_SLOTS[WPN_COUNT]  = {10,10, 7, 4, 6, 5, 6, 2, 10};
static const int WPN_DAMAGE[WPN_COUNT] = {95,90,55,30,50,45,48,12, 80};

// Weapon names for UI display
static const char* WPN_NAMES[WPN_COUNT] = {
    "Solar Core",
    "Lunar Blade",
    "Iron Halberd",
    "Venom Dagger",
    "Thunderstaff",
    "Obsidian Axe",
    "Frostbow",
    "Splinter Stick",
    "Eclipse Relic"
};

// ─── Action codes ────────────────────────────────────────────────────────────
#define ACTION_NONE        0
#define ACTION_STRIKE      1   // reduce enemy HP
#define ACTION_EXHAUST     2   // reduce enemy stamina (stun mechanic)
#define ACTION_USE_WEAPON  3   // attack with inventory weapon
#define ACTION_SWAP_IN     4   // retrieve weapon from long-term storage
#define ACTION_HEAL        5   // restore 10% HP
#define ACTION_SKIP        6   // skip turn (stamina -> 50%)
#define ACTION_QUIT        7   // SIGTERM to arbiter
#define ACTION_ULTIMATE    8   // trigger ultimate (needs Solar Core + Lunar Blade)

// ─── Artifact IDs for deadlock tracking ──────────────────────────────────────
#define ART_NONE          -1
#define ART_SOLAR_CORE     0
#define ART_LUNAR_BLADE    1
#define ART_ECLIPSE_RELIC  2

// ─── Entity struct ────────────────────────────────────────────────────────────
struct Entity {
    bool  is_alive;
    bool  is_stunned;
    int   hp;
    int   max_hp;
    int   stamina;
    int   max_stamina;
    int   damage;
    int   speed;

    int   inventory[INVENTORY_SIZE];
    int   long_term[LONG_TERM_SIZE];
    int   lt_count;

    pid_t pid;

    // Deadlock: which artifact is this entity waiting to acquire?
    int   waiting_for;
};

// ─── Weapon drop popup state ─────────────────────────────────────────────────
struct WeaponDropState {
    bool  pending;       // true = popup is visible, waiting for player decision
    int   weapon_id;     // which weapon dropped
    int   for_player;    // which player index decides (-1 = no player)
    // decision: 0 = pending, 1 = pick up, 2 = leave
    int   decision;
};

// ─── Shared game state ───────────────────────────────────────────────────────
struct GameState {
    // Synchronization
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
    int    enemies_killed;

    // Game status
    bool   is_game_over;
    bool   player_won;
    bool   ultimate_pause;

    // Scheduling
    int    active_turn;

    // Action pipeline
    int    pending_action;
    int    action_actor;
    int    action_target;
    int    action_weapon_id;

    // Weapon drop popup
    WeaponDropState weapon_drop;

    // Artifacts
    bool   solar_core_exists;
    int    solar_core_holder;

    bool   lunar_blade_exists;
    int    lunar_blade_holder;

    bool   eclipse_relic_exists;
    int    eclipse_relic_holder;
    bool   eclipse_relic_announced;

    // Action log
    char   action_log[MAX_ACTION_LOG];
};

#endif // COMMON_H