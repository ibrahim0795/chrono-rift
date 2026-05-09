#ifndef COMMON_H
#define COMMON_H
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <signal.h>

// Game constants
#define MAX_PLAYERS       4
#define MAX_ENEMIES       9
#define INVENTORY_SIZE    20
#define LONG_TERM_SIZE    50
#define MAX_ACTION_LOG    512
#define SHARED_MEM_NAME   "/chronorift_mem"

// Roll-number derived constants (23I-0795)
#define ROLL_SEED         795  
#define LAST_DIGIT        5      
#define SECOND_LAST       9       
#define LAST_TWO          95      
#define ROLL_FULL         795     

// UIEventQueue capacity
// Maximum UIEvents the arbiter render thread can queue before HIP drains them.
#define UI_EVENT_CAP      32

// Action ring-buffer capacity 
// PlayerInputEvents waiting for the Arbiter main loop to pop.
// The Arbiter drains the buffer before granting the next turn so this
#define ACTION_BUF_CAP    8

//Weapon IDs
#define WPN_NONE          -1
#define WPN_SOLAR_CORE     0   // 10 slots, 95 dmg
#define WPN_LUNAR_BLADE    1   // 10 slots, 90 dmg 
#define WPN_IRON_HALBERD   2   //  7 slots, 55 dmg
#define WPN_VENOM_DAGGER   3   //  4 slots, 30 dmg
#define WPN_THUNDERSTAFF   4   //  6 slots, 50 dmg
#define WPN_OBSIDIAN_AXE   5   //  5 slots, 45 dmg
#define WPN_FROSTBOW       6   //  6 slots, 48 dmg
#define WPN_SPLINTER_STICK 7   //  2 slots, 12 dmg
#define WPN_ECLIPSE_RELIC  8   // 10 slots, 80 dmg
#define WPN_COUNT          9

// Weapon tables indexed by WPN_* id
static const int WPN_SLOTS[WPN_COUNT]  = {10, 10,  7,  4,  6,  5,  6, 2, 10};
static const int WPN_DAMAGE[WPN_COUNT] = {95, 90, 55, 30, 50, 45, 48, 12, 80};
extern const char* WPN_NAMES[WPN_COUNT];

//Action codes
#define ACTION_NONE        0
// deal damage to target
#define ACTION_STRIKE      1  
// drain stamina and stun target via SIGUSR1
#define ACTION_EXHAUST     2  
// attack with an inventory weapon
#define ACTION_USE_WEAPON  3   
// pull weapon from long-term storage
#define ACTION_SWAP_IN     4   
// restore 10 % HP
#define ACTION_HEAL        5   
// forfeit turn; stamina drops to 50 %
#define ACTION_SKIP        6  
// player requests SIGTERM to arbiter
#define ACTION_QUIT        7   
// dual-artifact ultimate (Solar Core + Lunar Blade)
#define ACTION_ULTIMATE    8 


// Artifact IDs (for deadlock-detection wait-for graph)
#define ART_NONE          -1
#define ART_SOLAR_CORE     0
#define ART_LUNAR_BLADE    1
#define ART_ECLIPSE_RELIC  2

// UIEvent 
struct UIEvent {
    int action;       
    int target;       
    int weapon_id;   
    int player_idx; 
};

// PlayerInputEvent
struct PlayerInputEvent {
    int  action;
    int  target;
    int  weapon_id;
    bool valid;    // false = sentinel(uninitialized)
};

// Per-player action ring-buffer 

struct PlayerActionBuffer {
    PlayerInputEvent events[ACTION_BUF_CAP];
    int head;   
    int tail;    
    int count;   
};

// UIEventQueue
struct UIEventQueue {
    UIEvent events[UI_EVENT_CAP];
    int count;  
};

// Entity
struct Entity {
    bool is_alive;
    bool is_stunned;
    int  hp;
    int max_hp;
    int stamina;
    int max_stamina;
    int damage;
    int speed;          

    int inventory[INVENTORY_SIZE];

    // Long-term storage: weapons pushed here when the primary inventory is full.
    int long_term[LONG_TERM_SIZE];
    int lt_count;

    pid_t pid;          

    int waiting_for;
};

// Weapon drop popup state 
struct WeaponDropState {
    bool pending;        
    int weapon_id;     
    int for_player;     
    int decision;     
};

// GameState
struct GameState {

    //Synchronization
    pthread_mutex_t memory_lock;

    //Process PIDs
    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;

    pthread_t hip_thread_tids[MAX_PLAYERS];
    pthread_t asp_thread_tids[MAX_ENEMIES];

    PlayerActionBuffer player_buffers[MAX_PLAYERS];
    UIEventQueue ui_events;

    //Entities
    int num_players;
    Entity players[MAX_PLAYERS];

    int num_enemies;
    Entity enemies[MAX_ENEMIES];
    int enemies_killed;    

    // Game status
    bool is_game_over;
    bool player_won;
    bool ultimate_pause;        
    int  active_turn;
    int  pending_action;       
    int action_actor;       
    int action_target;       
    int action_weapon_id;     

    // Weapon drop popup 
    WeaponDropState weapon_drop;

    // Artifact resource table
    bool   solar_core_exists;
    int    solar_core_holder;

    bool   lunar_blade_exists;
    int    lunar_blade_holder;

    bool   eclipse_relic_exists;
    int    eclipse_relic_holder;
    bool   eclipse_relic_announced;  

    //Action log
    char   action_log[MAX_ACTION_LOG];
};

#endif