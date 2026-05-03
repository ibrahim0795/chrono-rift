// ============================================================
//  hip.cpp  —  Human Interfacing Process
//
//  UPDATED: All player input has been moved to the SFML window
//  in arbiter.cpp. This process now only:
//    1. Attaches to shared memory
//    2. Registers its PID so arbiter can send signals
//    3. Waits while the game runs
//    4. Handles SIGUSR1 (stun signal) — no longer self-stops,
//       stun is handled via is_stunned flag in shared memory
//
//  OS Concepts used:
//    • POSIX Shared Memory  (shm_open, mmap)
//    • SIGUSR1: stun signal handler (flag-based, no SIGSTOP)
// ============================================================

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include "../common.h"

using namespace std;

GameState* gs      = nullptr;
bool       running = true;

// Stun handler: stun is now managed by the is_stunned flag in shared memory.
// The arbiter sets the flag and clears it after 3 seconds via a timer thread.
// HIP just needs to acknowledge the signal without stopping the whole process.
void stun_handler(int sig) {
    (void)sig;
    // Stun is flag-based — arbiter handles setting/clearing is_stunned.
    // Nothing to do here; the flag causes find_next_actor() to skip this entity.
}

void sigterm_handler(int sig) {
    (void)sig;
    running = false;
}

void attach_shared_memory() {
    // Retry a few times in case arbiter hasn't created shm yet
    int fd = -1;
    for (int i = 0; i < 10; i++) {
        fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd != -1) break;
        sleep(1);
    }
    if (fd == -1) { perror("HIP: shm_open"); exit(1); }
    gs = (GameState*)mmap(0, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("HIP: mmap"); exit(1); }
    close(fd);
}

int main() {
    signal(SIGUSR1,  stun_handler);
    signal(SIGTERM,  sigterm_handler);

    cout << "HIP: Connecting to shared memory..." << endl;
    attach_shared_memory();

    gs->hip_pid = getpid();

    // Register this PID for all player entities
    for (int i = 0; i < gs->num_players; i++)
        gs->players[i].pid = getpid();

    cout << "HIP: Connected. Players = " << gs->num_players << endl;
    cout << "HIP: All input handled via SFML window." << endl;

    // Wait until game ends
    while (running && !gs->is_game_over) {
        usleep(100000);
    }

    munmap(gs, sizeof(GameState));
    cout << "HIP: exiting." << endl;
    return 0;
}