// ============================================================
//  arbiter.cpp  —  Game Arbiter (Engine + SFML UI)
//  OS Concepts used:
//    • POSIX Shared Memory  (shm_open, mmap)
//    • PTHREAD_PROCESS_SHARED mutex
//    • Signals: SIGUSR1 (stun), SIGALRM (ultimate timer),
//               SIGTERM (quit), SIGCHLD (child exit)
//    • pthreads: render thread + deadlock-monitor thread
// ============================================================

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <SFML/Graphics.hpp>
#include "../common.h"

using namespace std;

// ─── Globals ─────────────────────────────────────────────────────────────────
GameState* gs = nullptr;   // pointer into shared memory
bool       running = true;

// ─── Forward declarations ────────────────────────────────────────────────────
void  init_game();
void  stamina_tick();
int   find_next_actor();
void  execute_action();
void  check_win_loss();
void* render_thread_func(void* arg);
void* deadlock_monitor_func(void* arg);
void* stun_resume_thread(void* arg);
void  signal_handler(int sig);
void  cleanup();

struct StunResumeArgs {
    pid_t pid;
    bool  players_group;
};

// ─── Inventory helpers ───────────────────────────────────────────────────────
// Returns starting slot index if weapon fits, -1 otherwise
int find_contiguous_slots(Entity& e, int size) {
    for (int i = 0; i <= INVENTORY_SIZE - size; i++) {
        bool free = true;
        for (int j = i; j < i + size; j++)
            if (e.inventory[j] != WPN_NONE) { free = false; break; }
        if (free) return i;
    }
    return -1;
}

// Swap out weapons until 'size' contiguous slots are available
void make_room(Entity& e, int size) {
    while (find_contiguous_slots(e, size) == -1) {
        // Find and remove the first weapon to long-term storage
        for (int i = 0; i < INVENTORY_SIZE; i++) {
            if (e.inventory[i] != WPN_NONE) {
                int wpn = e.inventory[i];
                int wpn_size = WPN_SLOTS[wpn];
                // Remove all slots of this weapon
                for (int j = i; j < i + wpn_size && j < INVENTORY_SIZE; j++)
                    e.inventory[j] = WPN_NONE;
                // Move to long-term
                if (e.lt_count < LONG_TERM_SIZE)
                    e.long_term[e.lt_count++] = wpn;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Swapped weapon %d to long-term storage", wpn);
                break;
            }
        }
    }
}

bool pick_up_weapon(Entity& e, int wpn_id) {
    int size = WPN_SLOTS[wpn_id];
    int slot = find_contiguous_slots(e, size);
    if (slot == -1) {
        make_room(e, size);
        slot = find_contiguous_slots(e, size);
    }
    if (slot == -1) return false;
    for (int i = slot; i < slot + size; i++)
        e.inventory[i] = wpn_id;
    return true;
}

// ─── Signal handler ──────────────────────────────────────────────────────────
void signal_handler(int sig) {
    if (sig == SIGTERM) {
        gs->is_game_over = true;
        running = false;
    }
    if (sig == SIGCHLD) {
        int status;
        waitpid(-1, &status, WNOHANG);
    }
    if (sig == SIGALRM) {
        // Ultimate ability 10-second window expired — resume ASP
        if (gs->asp_pid > 0) {
            kill(gs->asp_pid, SIGCONT);
            gs->ultimate_pause = false;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Ultimate window expired. ASP resumed.");
        }
    }
}

void trigger_stun(pid_t pid, bool players_group) {
    if (pid <= 0) return;

    pthread_mutex_lock(&gs->memory_lock);
    if (players_group) {
        for (int i = 0; i < gs->num_players; i++)
            if (gs->players[i].is_alive) gs->players[i].is_stunned = true;
    } else {
        for (int i = 0; i < gs->num_enemies; i++)
            if (gs->enemies[i].is_alive) gs->enemies[i].is_stunned = true;
    }
    pthread_mutex_unlock(&gs->memory_lock);

    kill(pid, SIGUSR1);

    StunResumeArgs* args = new StunResumeArgs;
    args->pid = pid;
    args->players_group = players_group;

    pthread_t tid;
    pthread_create(&tid, nullptr, stun_resume_thread, args);
    pthread_detach(tid);
}

void* stun_resume_thread(void* arg) {
    StunResumeArgs* args = (StunResumeArgs*)arg;
    sleep(3);

    pthread_mutex_lock(&gs->memory_lock);
    if (args->players_group) {
        for (int i = 0; i < gs->num_players; i++)
            gs->players[i].is_stunned = false;
    } else {
        for (int i = 0; i < gs->num_enemies; i++)
            gs->enemies[i].is_stunned = false;
    }
    pthread_mutex_unlock(&gs->memory_lock);

    kill(args->pid, SIGCONT);
    delete args;
    return nullptr;
}

// ─── Init shared memory & game state ─────────────────────────────────────────
void init_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open"); exit(1); }
    ftruncate(fd, sizeof(GameState));
    gs = (GameState*)mmap(0, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);

    memset(gs, 0, sizeof(GameState));

    // Process-shared mutex
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gs->memory_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

void init_game() {
    srand(ROLL_SEED);

    // Ask party size
    cout << "=== CHRONO RIFT ===" << endl;
    cout << "Enter number of players (1-4): ";
    cin >> gs->num_players;
    if (gs->num_players < 1 || gs->num_players > MAX_PLAYERS)
        gs->num_players = 1;

    // Init players
    for (int i = 0; i < gs->num_players; i++) {
        Entity& p = gs->players[i];
        p.is_alive    = true;
        p.is_stunned  = false;
        p.hp          = ROLL_FULL + (rand() % 901 + 100);
        p.max_hp      = p.hp;
        p.max_stamina = 100;
        p.stamina     = 0;
        p.damage      = LAST_DIGIT + 10;          // 5+10 = 15
        p.speed       = 100 / gs->num_players;
        p.lt_count    = 0;
        memset(p.inventory, WPN_NONE, sizeof(p.inventory));
        memset(p.long_term, WPN_NONE, sizeof(p.long_term));
    }

    // Init enemies (2-9)
    gs->num_enemies = rand() % 8 + 2;
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity& e = gs->enemies[i];
        e.is_alive    = true;
        e.is_stunned  = false;
        e.hp          = LAST_TWO + (rand() % 151 + 50);  // 95 + 50-200
        e.max_hp      = e.hp;
        e.max_stamina = 150;
        e.stamina     = 0;
        e.damage      = SECOND_LAST + 10;                // 9+10 = 19
        e.speed       = rand() % 21 + 10;
        e.lt_count    = 0;
        memset(e.inventory, WPN_NONE, sizeof(e.inventory));
        memset(e.long_term, WPN_NONE, sizeof(e.long_term));
    }

    gs->is_game_over   = false;
    gs->player_won     = false;
    gs->enemies_killed = 0;
    gs->active_turn    = -1;
    gs->pending_action = ACTION_NONE;
    gs->ultimate_pause = false;

    // Artifacts are free at start
    gs->solar_core_exists   = true;
    gs->solar_core_holder   = -1;
    gs->lunar_blade_exists  = true;
    gs->lunar_blade_holder  = -1;
    gs->eclipse_relic_exists = false;
    gs->eclipse_relic_holder = -1;

    gs->arbiter_pid = getpid();
    snprintf(gs->action_log, MAX_ACTION_LOG,
             "Chrono Rift started! Players: %d  Enemies: %d",
             gs->num_players, gs->num_enemies);

    cout << "Players: " << gs->num_players
         << "  Enemies: " << gs->num_enemies << endl;
}

// ─── Stamina tick — called every ~100ms ──────────────────────────────────────
void stamina_tick() {
    pthread_mutex_lock(&gs->memory_lock);

    for (int i = 0; i < gs->num_players; i++) {
        Entity& p = gs->players[i];
        if (!p.is_alive || p.is_stunned) continue;
        p.stamina += p.speed;
        if (p.stamina > p.max_stamina) p.stamina = p.max_stamina;
    }

    if (!gs->ultimate_pause) {
        for (int i = 0; i < gs->num_enemies; i++) {
            Entity& e = gs->enemies[i];
            if (!e.is_alive || e.is_stunned) continue;
            e.stamina += e.speed;
            if (e.stamina > e.max_stamina) e.stamina = e.max_stamina;
        }
    }

    pthread_mutex_unlock(&gs->memory_lock);
}

// Returns index of first entity that reached full stamina (-1 if none)
int find_next_actor() {
    // Check players first, then enemies
    for (int i = 0; i < gs->num_players; i++) {
        Entity& p = gs->players[i];
        if (p.is_alive && !p.is_stunned && p.stamina >= p.max_stamina)
            return i;
    }
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity& e = gs->enemies[i];
        if (e.is_alive && !e.is_stunned && e.stamina >= e.max_stamina)
            return MAX_PLAYERS + i;
    }
    return -1;
}

// ─── Execute the action written by HIP or ASP ────────────────────────────────
void execute_action() {
    if (gs->pending_action == ACTION_NONE) return;

    pthread_mutex_lock(&gs->memory_lock);

    int actor  = gs->action_actor;
    int target = gs->action_target;
    int action = gs->pending_action;
    int wpn    = gs->action_weapon_id;

    bool actor_is_player = (actor < MAX_PLAYERS);
    Entity& actor_entity = actor_is_player
                           ? gs->players[actor]
                           : gs->enemies[actor - MAX_PLAYERS];

    bool target_is_player = (target < MAX_PLAYERS);
    Entity& target_entity = target_is_player
                            ? gs->players[target]
                            : gs->enemies[target - MAX_PLAYERS];

    switch (action) {
        case ACTION_STRIKE:
            target_entity.hp -= actor_entity.damage;
            actor_entity.stamina = 0;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Entity %d STRIKES entity %d for %d dmg",
                     actor, target, actor_entity.damage);
            break;

        case ACTION_EXHAUST:
            target_entity.stamina -= actor_entity.damage;
            if (target_entity.stamina < 0) target_entity.stamina = 0;
            actor_entity.stamina = 0;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Entity %d EXHAUSTS entity %d stamina",
                     actor, target);
            trigger_stun(target_is_player ? gs->hip_pid : gs->asp_pid,
                         target_is_player);
            break;

        case ACTION_USE_WEAPON:
            if (wpn >= 0 && wpn < WPN_COUNT) {
                target_entity.hp -= WPN_DAMAGE[wpn];
                actor_entity.stamina = 0;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d uses weapon %d on entity %d for %d dmg",
                         actor, wpn, target, WPN_DAMAGE[wpn]);

                // Check ultimate ability eligibility
                bool has_solar = false, has_lunar = false;
                for (int s = 0; s < INVENTORY_SIZE; s++) {
                    if (actor_entity.inventory[s] == WPN_SOLAR_CORE)  has_solar = true;
                    if (actor_entity.inventory[s] == WPN_LUNAR_BLADE) has_lunar = true;
                }
                if (has_solar && has_lunar && actor_is_player) {
                    // Ultimate ability: pause ASP for 10 seconds
                    gs->ultimate_pause = true;
                    if (gs->asp_pid > 0) kill(gs->asp_pid, SIGSTOP);
                    alarm(10);
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "ULTIMATE ABILITY! ASP paused for 10 seconds!");
                }
            }
            break;

        case ACTION_HEAL:
            actor_entity.hp += actor_entity.max_hp / 10;
            if (actor_entity.hp > actor_entity.max_hp)
                actor_entity.hp = actor_entity.max_hp;
            actor_entity.stamina = 0;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Entity %d HEALS", actor);
            break;

        case ACTION_SKIP:
            actor_entity.stamina = actor_entity.max_stamina / 2;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Entity %d SKIPS", actor);
            break;

        case ACTION_SWAP_IN:
            if (wpn >= 0 && wpn < actor_entity.lt_count) {
                int wpn_id = actor_entity.long_term[wpn];
                pick_up_weapon(actor_entity, wpn_id);
                // Remove from long-term
                for (int k = wpn; k < actor_entity.lt_count - 1; k++)
                    actor_entity.long_term[k] = actor_entity.long_term[k+1];
                actor_entity.lt_count--;
                actor_entity.stamina = 0;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d swapped in weapon %d", actor, wpn_id);
            }
            break;
    }

    // Check if target died
    if (target_entity.hp <= 0) {
        target_entity.is_alive = false;
        if (!target_is_player) {
            gs->enemies_killed++;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Enemy %d defeated! Total kills: %d",
                     target - MAX_PLAYERS, gs->enemies_killed);
        }
    }

    // Clear pending action
    gs->pending_action = ACTION_NONE;
    gs->active_turn    = -1;

    pthread_mutex_unlock(&gs->memory_lock);
}

// ─── Win/loss check ──────────────────────────────────────────────────────────
void check_win_loss() {
    if (gs->enemies_killed >= 10) {
        gs->is_game_over = true;
        gs->player_won   = true;
        return;
    }
    bool any_alive = false;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].is_alive) { any_alive = true; break; }
    if (!any_alive) {
        gs->is_game_over = true;
        gs->player_won   = false;
    }
}

// ─── SFML Render thread ───────────────────────────────────────────────────────
void* render_thread_func(void* arg) {
    // Check if display is available before creating window
    const char* display = getenv("DISPLAY");
    if (!display || display[0] == '\0') {
        cout << "[Render] No DISPLAY set — running without GUI." << endl;
        while (!gs->is_game_over) {
            // Print state to console instead
            cout << "\r[Game] Enemies killed: " << gs->enemies_killed
                 << "  Active turn: " << gs->active_turn
                 << "  Log: " << gs->action_log
                 << "          " << flush;
            usleep(500000);
        }
        return nullptr;
    }

    sf::RenderWindow window(sf::VideoMode(900, 600), "Chrono Rift");
    sf::Font font;
    // Try loading a system font
    if (!font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")) {
        // fallback — no font, still render colored bars
    }

    while (window.isOpen() && !gs->is_game_over) {
        sf::Event event;
        while (window.pollEvent(event))
            if (event.type == sf::Event::Closed) window.close();

        window.clear(sf::Color(20, 20, 40));

        // ── Draw player bars ──────────────────────────────────────────────
        for (int i = 0; i < gs->num_players; i++) {
            Entity& p = gs->players[i];
            float y = 50.f + i * 80.f;

            // HP bar (green)
            float hp_ratio = (float)p.hp / p.max_hp;
            sf::RectangleShape hp_bar(sf::Vector2f(200.f * hp_ratio, 20.f));
            hp_bar.setFillColor(sf::Color::Green);
            hp_bar.setPosition(100.f, y);
            window.draw(hp_bar);

            // Stamina bar (yellow)
            float st_ratio = (float)p.stamina / p.max_stamina;
            sf::RectangleShape st_bar(sf::Vector2f(200.f * st_ratio, 10.f));
            st_bar.setFillColor(sf::Color::Yellow);
            st_bar.setPosition(100.f, y + 25.f);
            window.draw(st_bar);

            // Label
            sf::Text label("P" + to_string(i+1) +
                           " HP:" + to_string(p.hp) +
                           " SP:" + to_string(p.stamina),
                           font, 14);
            label.setPosition(10.f, y);
            label.setFillColor(sf::Color::White);
            window.draw(label);
        }

        // ── Draw enemy bars ───────────────────────────────────────────────
        for (int i = 0; i < gs->num_enemies; i++) {
            Entity& e = gs->enemies[i];
            if (!e.is_alive) continue;
            float y = 50.f + i * 60.f;

            float hp_ratio = (float)e.hp / e.max_hp;
            sf::RectangleShape hp_bar(sf::Vector2f(150.f * hp_ratio, 15.f));
            hp_bar.setFillColor(sf::Color::Red);
            hp_bar.setPosition(650.f, y);
            window.draw(hp_bar);

            float st_ratio = (float)e.stamina / e.max_stamina;
            sf::RectangleShape st_bar(sf::Vector2f(150.f * st_ratio, 8.f));
            st_bar.setFillColor(sf::Color(255, 165, 0));
            st_bar.setPosition(650.f, y + 18.f);
            window.draw(st_bar);

            sf::Text label("E" + to_string(i+1) +
                           " HP:" + to_string(e.hp),
                           font, 12);
            label.setPosition(810.f, y);
            label.setFillColor(sf::Color::White);
            window.draw(label);
        }

        // ── Action log ────────────────────────────────────────────────────
        sf::Text log_text(gs->action_log, font, 13);
        log_text.setPosition(10.f, 520.f);
        log_text.setFillColor(sf::Color::Cyan);
        window.draw(log_text);

        // ── Active turn indicator ─────────────────────────────────────────
        if (gs->active_turn >= 0) {
            string turn_str = (gs->active_turn < MAX_PLAYERS)
                ? "Player " + to_string(gs->active_turn + 1) + "'s turn"
                : "Enemy "  + to_string(gs->active_turn - MAX_PLAYERS + 1) + "'s turn";
            sf::Text turn_text(turn_str, font, 16);
            turn_text.setPosition(350.f, 560.f);
            turn_text.setFillColor(sf::Color::Magenta);
            window.draw(turn_text);
        }

        window.display();
        usleep(33000); // ~30 FPS
    }

    // Game over screen
    window.clear(sf::Color::Black);
    sf::Text end_text(gs->player_won ? "YOU WIN!" : "GAME OVER", font, 48);
    end_text.setPosition(300.f, 250.f);
    end_text.setFillColor(gs->player_won ? sf::Color::Green : sf::Color::Red);
    window.draw(end_text);
    window.display();
    sleep(3);
    window.close();
    return nullptr;
}

// ─── Deadlock monitor thread ─────────────────────────────────────────────────
void* deadlock_monitor_func(void* arg) {
    while (!gs->is_game_over) {
        sleep(2);
        pthread_mutex_lock(&gs->memory_lock);

        // Simple circular wait check:
        // If solar held by A and lunar held by B,
        // and A is waiting for lunar and B is waiting for solar → deadlock
        // TODO: Implement full wait-for graph per project spec
        // For now: if both are held and both holders are waiting, force-release one
        if (gs->solar_core_holder != -1 && gs->lunar_blade_holder != -1) {
            int a = gs->solar_core_holder;
            int b = gs->lunar_blade_holder;
            if (a != b) {
                // Deadlock detected — force release solar core
                gs->solar_core_holder = -1;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "[Arbiter] DEADLOCK detected! Released Solar Core from entity %d", a);
            }
        }

        pthread_mutex_unlock(&gs->memory_lock);
    }
    return nullptr;
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────
void cleanup() {
    pthread_mutex_destroy(&gs->memory_lock);
    munmap(gs, sizeof(GameState));
    shm_unlink(SHARED_MEM_NAME);
    cout << "Arbiter: shared memory cleaned up." << endl;
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    // Register signals
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, signal_handler);
    signal(SIGALRM, signal_handler);

    init_shared_memory();
    init_game();

    // Start render thread
    pthread_t render_tid;
    pthread_create(&render_tid, nullptr, render_thread_func, nullptr);

    // Start deadlock monitor thread
    pthread_t deadlock_tid;
    pthread_create(&deadlock_tid, nullptr, deadlock_monitor_func, nullptr);

    cout << "Arbiter running. Waiting for HIP and ASP to connect..." << endl;
    // Give HIP and ASP time to attach to shared memory
    sleep(2);

    // ── Main game loop ────────────────────────────────────────────────────
    while (!gs->is_game_over) {
        stamina_tick();
        usleep(100000); // 100ms per tick

        int actor = find_next_actor();
        if (actor == -1) continue;

        gs->active_turn = actor;

        // Wait for action to be submitted (HIP or ASP will write it)
        int timeout = 0;
        while (gs->pending_action == ACTION_NONE && !gs->is_game_over) {
            usleep(50000);
            timeout++;
            // NPC timeout: 3 seconds (30 * 100ms = 3000ms... 60 * 50ms)
            if (actor >= MAX_PLAYERS && timeout > 60) {
                gs->pending_action = ACTION_SKIP;
                gs->action_actor   = actor;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Enemy %d timed out — auto SKIP", actor - MAX_PLAYERS);
                break;
            }
        }

        execute_action();
        check_win_loss();
    }

    // Game ended
    if (gs->player_won)
        cout << "Players WIN! Enemies killed: " << gs->enemies_killed << endl;
    else
        cout << "GAME OVER. All players defeated." << endl;

    pthread_join(render_tid, nullptr);
    pthread_join(deadlock_tid, nullptr);
    cleanup();
    return 0;
}