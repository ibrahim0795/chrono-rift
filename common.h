#ifndef COMMON_H
#define COMMON_H

// ═══════════════════════════════════════════════════════════════════════════
//  common.h  —  Chrono Rift shared definitions
//
//  IPC POLICY (STRICT — NO PIPES):
//    All inter-process communication uses POSIX shared memory only.
//    The single SharedMemory segment (SHARED_MEM_NAME) holds every struct
//    in this file.  Synchronization uses the PTHREAD_PROCESS_SHARED mutex
//    gs->memory_lock exclusively — no pipes, no FIFOs, no sockets.
//
//  DATA FLOW SUMMARY:
//
//    Arbiter render thread
//      → writes UIEvent into gs->ui_events  (locked with memory_lock)
//
//    HIP player thread[i]
//      → polls gs->ui_events for events where player_idx == i
//      → pops matching event (locked with memory_lock)
//      → pushes PlayerInputEvent into gs->player_buffers[i]  (locked)
//
//    Arbiter main loop
//      → pops PlayerInputEvent from gs->player_buffers[actor]  (locked)
//      → commits to gs->pending_action / gs->action_actor etc.
//      → calls execute_action()
//
//    ASP enemy thread[i]
//      → writes directly to gs->pending_action / gs->action_actor
//        (locked with memory_lock)
//
//  STUN MECHANIC (signal-based, never flag-polled):
//    Arbiter calls pthread_kill(gs->hip_thread_tids[i], SIGUSR1) or
//                   pthread_kill(gs->asp_thread_tids[i], SIGUSR1).
//    The per-thread stun_handler calls sleep(3) inside the handler,
//    blocking only that thread for 3 seconds.  is_stunned is a
//    VISUAL-ONLY flag for the renderer — no game logic polls it.
// ═══════════════════════════════════════════════════════════════════════════

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <signal.h>

// ─── Game constants ───────────────────────────────────────────────────────────
#define MAX_PLAYERS       4
#define MAX_ENEMIES       9
#define INVENTORY_SIZE    20
#define LONG_TERM_SIZE    50
#define MAX_ACTION_LOG    512
#define SHARED_MEM_NAME   "/chronorift_mem"

// Roll-number derived constants (23I-0795)
#define ROLL_SEED         795     // srand seed
#define LAST_DIGIT        5       // player base damage offset
#define SECOND_LAST       9       // enemy base damage offset
#define LAST_TWO          95      // enemy base HP offset
#define ROLL_FULL         795     // player HP base

// ─── UIEventQueue capacity ────────────────────────────────────────────────────
// Maximum UIEvents the arbiter render thread can queue before HIP drains them.
// 32 is generous — in normal play at most 1 event is pending at a time.
#define UI_EVENT_CAP      32

// ─── Action ring-buffer capacity ─────────────────────────────────────────────
// PlayerInputEvents waiting for the Arbiter main loop to pop.
// The Arbiter drains the buffer before granting the next turn so this
// only ever holds 1 entry in practice, but capacity 8 gives safety margin.
#define ACTION_BUF_CAP    8

// ─── Weapon IDs ──────────────────────────────────────────────────────────────
#define WPN_NONE          -1
#define WPN_SOLAR_CORE     0   // 10 slots, 95 dmg  — artifact
#define WPN_LUNAR_BLADE    1   // 10 slots, 90 dmg  — artifact
#define WPN_IRON_HALBERD   2   //  7 slots, 55 dmg
#define WPN_VENOM_DAGGER   3   //  4 slots, 30 dmg
#define WPN_THUNDERSTAFF   4   //  6 slots, 50 dmg
#define WPN_OBSIDIAN_AXE   5   //  5 slots, 45 dmg
#define WPN_FROSTBOW       6   //  6 slots, 48 dmg
#define WPN_SPLINTER_STICK 7   //  2 slots, 12 dmg
#define WPN_ECLIPSE_RELIC  8   // 10 slots, 80 dmg  — dynamic artifact
#define WPN_COUNT          9

// Weapon tables indexed by WPN_* id
static const int WPN_SLOTS[WPN_COUNT]  = {10, 10,  7,  4,  6,  5,  6, 2, 10};
static const int WPN_DAMAGE[WPN_COUNT] = {95, 90, 55, 30, 50, 45, 48, 12, 80};

// Human-readable weapon names (defined in weapon_names.cpp)
extern const char* WPN_NAMES[WPN_COUNT];

// ─── Action codes ─────────────────────────────────────────────────────────────
#define ACTION_NONE        0
#define ACTION_STRIKE      1   // deal damage to target
#define ACTION_EXHAUST     2   // drain stamina and stun target via SIGUSR1
#define ACTION_USE_WEAPON  3   // attack with an inventory weapon
#define ACTION_SWAP_IN     4   // pull weapon from long-term storage
#define ACTION_HEAL        5   // restore 10 % HP
#define ACTION_SKIP        6   // forfeit turn; stamina drops to 50 %
#define ACTION_QUIT        7   // player requests SIGTERM to arbiter
#define ACTION_ULTIMATE    8   // dual-artifact ultimate (Solar Core + Lunar Blade)

// ─── Artifact IDs (for deadlock-detection wait-for graph) ────────────────────
#define ART_NONE          -1
#define ART_SOLAR_CORE     0
#define ART_LUNAR_BLADE    1
#define ART_ECLIPSE_RELIC  2

// ─── UIEvent ─────────────────────────────────────────────────────────────────
// Written into gs->ui_events by the Arbiter render thread when the active
// player clicks a UI element.  Consumed by the matching HIP player thread.
// NO pipe file descriptor is involved — the struct lives in shared memory.
struct UIEvent {
    int action;       // ACTION_* code chosen in the UI
    int target;       // entity index (MAX_PLAYERS + enemy_i for enemies), -1 if N/A
    int weapon_id;    // WPN_* id, or long-term slot index for SWAP_IN, or -1
    int player_idx;   // which player's thread should consume this event
};

// ─── PlayerInputEvent ─────────────────────────────────────────────────────────
// A fully resolved player decision produced by a HIP player thread after it
// consumes a UIEvent.  Stored in the per-player ring-buffer; the Arbiter
// main loop pops one of these to drive execute_action().
struct PlayerInputEvent {
    int  action;
    int  target;
    int  weapon_id;
    bool valid;    // false = sentinel / uninitialized
};

// ─── Per-player action ring-buffer ───────────────────────────────────────────
// HIP player threads push here; Arbiter main loop pops here.
// Protected by gs->memory_lock.
struct PlayerActionBuffer {
    PlayerInputEvent events[ACTION_BUF_CAP];
    int head;    // Arbiter reads from here
    int tail;    // HIP player thread writes here
    int count;   // number of valid entries currently in the buffer
};

// ─── UIEventQueue ─────────────────────────────────────────────────────────────
// Simple ordered queue stored inside shared memory.
// The Arbiter render thread appends events at events[count++].
// HIP player threads perform an in-place removal when they find their event.
// All accesses must hold gs->memory_lock.
//
// Design note: This replaces the (now-removed) named FIFO/pipe that the
// previous iteration used.  There is NO file descriptor, NO open(), NO
// read() system call involved in event delivery.
struct UIEventQueue {
    UIEvent events[UI_EVENT_CAP];
    int     count;   // number of events currently queued
};

// ─── Entity ───────────────────────────────────────────────────────────────────
struct Entity {
    bool  is_alive;

    // is_stunned — VISUAL HINT FOR RENDERER ONLY.
    // The authoritative stun mechanism works via SIGUSR1 sent by the Arbiter
    // directly to the entity's owning pthread.  The stun_handler calls
    // sleep(3) inside the handler body, blocking that thread for exactly
    // 3 seconds from the point of signal delivery.  No game-logic code
    // may poll is_stunned to decide whether to take a turn.
    bool  is_stunned;

    int   hp;
    int   max_hp;
    int   stamina;
    int   max_stamina;
    int   damage;
    int   speed;          // stamina added per tick

    // Primary inventory: INVENTORY_SIZE contiguous slots.
    // A weapon that occupies N slots fills N consecutive entries with its id.
    int   inventory[INVENTORY_SIZE];

    // Long-term storage: weapons pushed here when the primary inventory is full.
    int   long_term[LONG_TERM_SIZE];
    int   lt_count;

    pid_t pid;            // owning process PID (informational)

    // Deadlock detection: which artifact is this entity currently waiting to acquire?
    // ART_NONE = not waiting for anything.
    int   waiting_for;
};

// ─── Weapon drop popup state ──────────────────────────────────────────────────
struct WeaponDropState {
    bool  pending;        // true = popup visible, waiting for player decision
    int   weapon_id;      // which weapon was dropped
    int   for_player;     // which player index must decide (-1 = none)
    int   decision;       // 0 = still pending, 1 = picked up, 2 = left on ground
};

// ─── GameState (the entire shared memory segment) ────────────────────────────
struct GameState {

    // ── Synchronization ───────────────────────────────────────────────────────
    // Initialized with PTHREAD_PROCESS_SHARED so it works across all three
    // processes.  Every access to any field below must be bracketed by
    // pthread_mutex_lock / pthread_mutex_unlock on this mutex.
    pthread_mutex_t memory_lock;

    // ── Process PIDs ──────────────────────────────────────────────────────────
    pid_t arbiter_pid;
    pid_t hip_pid;
    pid_t asp_pid;

    // ── Per-player thread IDs (written by HIP at thread startup) ──────────────
    // Arbiter calls  pthread_kill(hip_thread_tids[i], SIGUSR1)  to stun
    // exactly player i's thread inside the HIP process.  All other player
    // threads continue running.  Set to 0 until the HIP thread registers.
    pthread_t hip_thread_tids[MAX_PLAYERS];

    // ── Per-enemy thread IDs (written by ASP at thread startup) ───────────────
    // Arbiter calls  pthread_kill(asp_thread_tids[i], SIGUSR1)  to stun
    // exactly enemy i's thread inside the ASP process.  Set to 0 until
    // the ASP thread registers.
    pthread_t asp_thread_tids[MAX_ENEMIES];

    // ── Per-player action buffers ──────────────────────────────────────────────
    // HIP player threads push resolved PlayerInputEvents here.
    // Arbiter main loop pops them — it never writes player actions itself.
    PlayerActionBuffer player_buffers[MAX_PLAYERS];

    // ── UIEvent queue (shared memory — NO PIPE) ──────────────────────────────
    // Arbiter render thread writes UIEvents here when the player clicks UI.
    // HIP player threads poll and consume their own events from this queue.
    // Both sides hold memory_lock during every access.
    UIEventQueue ui_events;

    // ── Entities ──────────────────────────────────────────────────────────────
    int    num_players;
    Entity players[MAX_PLAYERS];

    int    num_enemies;
    Entity enemies[MAX_ENEMIES];
    int    enemies_killed;        // win condition: reach 10

    // ── Game status ───────────────────────────────────────────────────────────
    bool   is_game_over;
    bool   player_won;
    bool   ultimate_pause;        // true while ASP is SIGSTOP'd by ultimate ability

    // ── Scheduling ────────────────────────────────────────────────────────────
    // -1 = no entity is currently taking a turn.
    // 0..MAX_PLAYERS-1         = player index.
    // MAX_PLAYERS..MAX_PLAYERS+MAX_ENEMIES-1 = enemy index.
    int    active_turn;

    // ── Action pipeline (ASP enemy threads write here; Arbiter reads) ────────
    // Player actions travel through player_buffers[] instead.
    int    pending_action;       // ACTION_* code, or ACTION_NONE when idle
    int    action_actor;         // entity index using the active_turn encoding
    int    action_target;        // target entity index
    int    action_weapon_id;     // WPN_* id or long-term slot index

    // ── Weapon drop popup ─────────────────────────────────────────────────────
    WeaponDropState weapon_drop;

    // ── Artifact resource table ───────────────────────────────────────────────
    // holder = -1 means the artifact is currently free.
    bool   solar_core_exists;
    int    solar_core_holder;

    bool   lunar_blade_exists;
    int    lunar_blade_holder;

    bool   eclipse_relic_exists;
    int    eclipse_relic_holder;
    bool   eclipse_relic_announced;   // true once the Eclipse Relic drop event fired

    // ── Action log ────────────────────────────────────────────────────────────
    // Latest human-readable event description, shown in the HUD.
    char   action_log[MAX_ACTION_LOG];
};

#endif // COMMON_H