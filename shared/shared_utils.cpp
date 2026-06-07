#include "shared_utils.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>

// ---- internal state for signal handlers ----

static bool* g_running_flag = nullptr;

static void stun_handler(int) {
    sleep(3);
}

static void sigterm_handler(int) {
    if (g_running_flag) *g_running_flag = false;
}

// ---- public API ----

GameState* attach_shared_memory(const char* process_name) {
    int fd = -1;
    for (int i = 0; i < 10 && fd == -1; i++) {
        fd = shm_open(SHARED_MEM_NAME, O_RDWR, 0666);
        if (fd == -1) sleep(1);
    }
    if (fd == -1) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: shm_open", process_name);
        perror(msg);
        exit(1);
    }

    GameState* gs = (GameState*)mmap(nullptr, sizeof(GameState),
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: mmap", process_name);
        perror(msg);
        exit(1);
    }
    if (close(fd) == -1) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s: close shm fd", process_name);
        perror(msg);
    }
    return gs;
}

void install_stun_and_term_handlers(bool* running_flag) {
    g_running_flag = running_flag;

    // Block SIGUSR1 process-wide; threads unblock individually.
    sigset_t block_usr1;
    sigemptyset(&block_usr1);
    sigaddset(&block_usr1, SIGUSR1);
    if (pthread_sigmask(SIG_BLOCK, &block_usr1, nullptr) != 0) {
        perror("pthread_sigmask SIG_BLOCK");
        exit(1);
    }

    // SIGUSR1 → 3-second stun sleep.
    struct sigaction sa{};
    sa.sa_handler = stun_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, nullptr) == -1) {
        perror("sigaction SIGUSR1");
        exit(1);
    }

    // SIGTERM → set running flag to false.
    if (signal(SIGTERM, sigterm_handler) == SIG_ERR) {
        perror("signal SIGTERM");
        exit(1);
    }
}

void unblock_sigusr1_current_thread() {
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGUSR1);
    if (pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr) != 0)
        perror("pthread_sigmask SIG_UNBLOCK");
}

void init_enemy(Entity& e) {
    e.is_alive    = true;
    e.is_stunned  = false;
    e.hp          = LAST_TWO + (rand() % 151 + 50);
    e.max_hp      = e.hp;
    e.max_stamina = 150;
    e.stamina     = 0;
    e.damage      = SECOND_LAST + 10;
    e.speed       = rand() % 21 + 10;
    e.lt_count    = 0;
    e.waiting_for = ART_NONE;
    memset(e.inventory, WPN_NONE, sizeof(e.inventory));
    memset(e.long_term,  WPN_NONE, sizeof(e.long_term));
}
