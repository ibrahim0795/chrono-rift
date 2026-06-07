#ifndef SHARED_UTILS_H
#define SHARED_UTILS_H

#include "../common.h"

// Attach to an existing shared-memory segment with retry (up to 10 s).
// Returns a mapped GameState* or exits on failure.
// `process_name` is used in error messages (e.g. "ASP", "HIP").
GameState* attach_shared_memory(const char* process_name);

// Install SIGUSR1 (3-second stun sleep) and SIGTERM handlers, and block
// SIGUSR1 process-wide so individual threads can selectively unblock it.
// On SIGTERM the flag pointed to by `running_flag` is set to false.
void install_stun_and_term_handlers(bool* running_flag);

// Unblock SIGUSR1 for the calling thread only.
void unblock_sigusr1_current_thread();

// Initialize an enemy Entity with randomised stats derived from ROLL_SEED
// constants.  Caller must seed rand() before the first call.
void init_enemy(Entity& e);

#endif
