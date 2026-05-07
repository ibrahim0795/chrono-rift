// ============================================================
//  hip.cpp  —  Human Interfacing Process (HIP)
//
//  IPC POLICY: POSIX shared memory ONLY — no pipes, no FIFOs.
//
//  THREADING MODEL:
//    One pthread is created for each player-controlled character.
//    MAX_PLAYERS threads may be alive simultaneously; each thread
//    handles exactly one player slot.
//
//  EVENT FLOW (shared memory, no file descriptors):
//
//    1. The Arbiter render thread writes a UIEvent into
//       gs->ui_events (the UIEventQueue in shared memory) when
//       the active player clicks a UI element.  It does NOT
//       touch gs->pending_action or any other action field.
//
//    2. Each player thread below polls gs->ui_events for any
//       UIEvent where uev.player_idx == this thread's player
//       index.  The poll uses usleep(10 ms) between iterations
//       so it never busy-spins.
//
//    3. When a matching event is found the thread:
//         a. Removes it from ui_events (in-place shift, locked).
//         b. Calls push_player_action() to write a
//            PlayerInputEvent into gs->player_buffers[player_idx]
//            (also locked with memory_lock).
//
//    4. The Arbiter main loop calls pop_player_action() to obtain
//       the decision and commits it to gs->pending_action.
//
//  STUN MECHANIC (SIGUSR1 — fully asynchronous, no flag polling):
//
//    • At startup each player thread registers its pthread_t in
//      gs->hip_thread_tids[player_idx].
//    • SIGUSR1 is blocked process-wide in main(); each player
//      thread individually unblocks it for itself.  This ensures
//      only the targeted thread ever receives the signal.
//    • When the Arbiter stuns player i it calls:
//          pthread_kill(gs->hip_thread_tids[i], SIGUSR1)
//    • The per-thread stun_handler calls sleep(3) inside the
//      signal handler body, suspending ONLY that thread for
//      3 seconds from the exact point of interruption.
//    • After sleep(3) the handler returns.  The interrupted
//      usleep() in the poll loop returns EINTR; the thread simply
//      loops and retries — the Arbiter's turn-timeout will have
//      advanced active_turn, so the thread idles cleanly.
//    • No code anywhere polls is_stunned to decide actions.
//
//  OS Concepts demonstrated:
//    • POSIX Shared Memory  (shm_open / mmap)
//    • pthreads: one dedicated thread per player character
//    • UIEventQueue in shared memory (no pipe, no FD)
//    • SIGUSR1 per-thread delivery via pthread_kill
//    • sleep(3) inside async signal handler for 3-second stun
//    • PTHREAD_PROCESS_SHARED mutex for all shared accesses
// ============================================================

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

// ─── Process-level globals ────────────────────────────────────────────────────
static GameState* gs      = nullptr;
static bool       running = true;

// ─── Per-thread stun signal handler ──────────────────────────────────────────
//
// Called in the context of whichever player thread received SIGUSR1.
// sleep(3) suspends ONLY this thread — not the process, not other threads.
// sleep() is async-signal-safe per POSIX and is therefore legal here.
//
// When sleep(3) returns the handler exits and the OS resumes the thread at
// the interrupted instruction.  The interrupted usleep() in player_thread_func
// returns EINTR; the loop simply retries.
//
static void stun_handler(int /*sig*/) {
    sleep(3);   // halt THIS thread — and only this thread — for 3 seconds
}

static void sigterm_handler(int /*sig*/) {
    running = false;
}

// ─── Attach to existing shared memory segment ────────────────────────────────
// The Arbiter creates the segment; HIP retries until it appears.
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

// ─── push_player_action ───────────────────────────────────────────────────────
// Called from a player thread after it has consumed a UIEvent.
// Pushes a fully resolved PlayerInputEvent into the per-player ring-buffer
// so the Arbiter main loop can pop it via pop_player_action().
// Uses the PTHREAD_PROCESS_SHARED mutex — safe across processes.
static void push_player_action(int player_idx,
                                int action, int target, int weapon_id)
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
    // If the buffer is somehow full (should never happen in normal play
    // because the Arbiter drains it before granting the next turn)
    // the event is silently dropped to avoid blocking under the lock.

    pthread_mutex_unlock(&gs->memory_lock);
}

// ─── Player thread ────────────────────────────────────────────────────────────
struct PlayerThreadArg { int player_idx; };

static void* player_thread_func(void* arg) {
    const int idx = ((PlayerThreadArg*)arg)->player_idx;

    // ── Step 1: register this pthread_t so Arbiter can target it ────────────
    gs->hip_thread_tids[idx] = pthread_self();

    // ── Step 2: unblock SIGUSR1 in this thread only ──────────────────────────
    // main() blocks SIGUSR1 process-wide before spawning threads so that
    // all threads inherit the blocked mask.  We selectively unblock it here,
    // ensuring the stun signal can ONLY be delivered to the correct thread.
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

    cout << "HIP: Player " << idx + 1
         << " thread ready  (tid=" << pthread_self() << ")" << endl;

    while (running && !gs->is_game_over) {

        // Skip dead players immediately.
        if (!gs->players[idx].is_alive) {
            usleep(50000);
            continue;
        }

        // ── Poll the UIEventQueue for an event addressed to this player ──────
        //
        // We do NOT block on a pipe read() call.  Instead we lock shared
        // memory, scan for our event, remove it if found, and unlock.
        // usleep(10 ms) between polls keeps CPU usage negligible.
        //
        // If SIGUSR1 (stun) arrives during usleep(), usleep() returns EINTR
        // after stun_handler's sleep(3) completes.  We simply retry the loop.
        //
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
            // Nothing found — yield CPU and retry.
            // usleep returns EINTR if a signal (stun) arrives; that is fine,
            // the loop simply retries from the top.
            usleep(10000);   // 10 ms poll interval
        }
    }

    cout << "HIP: Player " << idx + 1 << " thread exiting." << endl;
    return nullptr;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {

    // ── Block SIGUSR1 process-wide BEFORE spawning any thread ────────────────
    // Each player thread individually unblocks it for itself (see above).
    // This guarantees the stun signal is delivered only to the exact thread
    // the Arbiter targeted — never to main() or any other player thread.
    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr);

    // Install stun handler.
    // SA_RESTART is intentionally omitted so that usleep() returns EINTR
    // after the handler completes, allowing the poll loop to retry cleanly.
    struct sigaction sa{};
    sa.sa_handler = stun_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags   = 0;   // no SA_RESTART
    sigaction(SIGUSR1, &sa, nullptr);

    signal(SIGTERM, sigterm_handler);

    // ── Attach to shared memory created by Arbiter ───────────────────────────
    cout << "HIP: Connecting to shared memory..." << endl;
    attach_shared_memory();

    gs->hip_pid = getpid();
    for (int i = 0; i < gs->num_players; i++)
        gs->players[i].pid = getpid();

    // Zero thread-tid array; each thread fills its own slot at startup.
    memset(gs->hip_thread_tids, 0, sizeof(gs->hip_thread_tids));

    // Reset per-player action buffers (Arbiter also does this in init_game(),
    // but we do it again here for safety in case HIP starts after Arbiter).
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->player_buffers[i].head  = 0;
        gs->player_buffers[i].tail  = 0;
        gs->player_buffers[i].count = 0;
    }

    cout << "HIP: Connected.  Players = " << gs->num_players << endl;
    cout << "HIP: Spawning " << gs->num_players << " player thread(s)." << endl;

    // ── Spawn one pthread per player ─────────────────────────────────────────
    pthread_t       tids[MAX_PLAYERS];
    PlayerThreadArg args[MAX_PLAYERS];

    for (int i = 0; i < gs->num_players; i++) {
        args[i].player_idx = i;
        pthread_create(&tids[i], nullptr, player_thread_func, &args[i]);
    }

    // ── Wait for game end or SIGTERM ─────────────────────────────────────────
    while (running && !gs->is_game_over)
        usleep(100000);

    running = false;   // signal threads to exit their poll loops

    for (int i = 0; i < gs->num_players; i++)
        pthread_join(tids[i], nullptr);

    munmap(gs, sizeof(GameState));
    cout << "HIP: exiting." << endl;
    return 0;
}