// ============================================================
//  arbiter.cpp  —  Game Arbiter (Engine + SFML Arena UI)
//
//  CHANGES FROM ORIGINAL:
//    1. All player input moved into SFML window (no cin)
//    2. Weapon drop popup system added
//    3. Eclipse Relic introduced dynamically at kill #3
//    4. Ultimate Ability as separate ACTION_ULTIMATE
//    5. Stun fixed (entity-level, no whole-process SIGSTOP)
//    6. Enemy respawn to reach 10 kills
//    7. Deadlock detection with waiting_for tracking
//    8. Background image renders correctly
//    9. Per-player sprite column from players.png
//   10. Weapon icons drawn as coloured labeled boxes
//       (REPLACE weapons.png sprites when you have them)
//
//  OS Concepts:
//    * POSIX Shared Memory  (shm_open, mmap)
//    * PTHREAD_PROCESS_SHARED mutex
//    * Signals: SIGUSR1 (stun), SIGALRM (ultimate), SIGTERM, SIGCHLD
//    * pthreads: render thread + deadlock-monitor thread
// ============================================================

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
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

// ─── Window / layout ─────────────────────────────────────────────────────────
static const int   WIN_W    = 1500;
static const int   WIN_H    = 720;
static const float GROUND_Y = WIN_H * 0.72f;
static const float P_X1     = 80.f;
static const float P_X2     = 440.f;
static const float E_X1     = 760.f;
static const float E_X2     = 1220.f;

// ─── Sprite sheet constants ───────────────────────────────────────────────────
static const int CELL_W      = 128;
static const int CELL_H      = 128;
static const int ANIM_FRAMES = 5;

// ─── Globals ──────────────────────────────────────────────────────────────────
GameState* gs      = nullptr;
bool       running = true;

struct AttackTrigger {
    volatile bool fired;
    volatile int  attacker;
    volatile int  target;
};
static AttackTrigger atk_trig = {false, 0, 0};

// Ultimate flash state (written by execute_action, read by render)
static volatile float ultimate_flash_timer = 0.f;

// ─── Forward declarations ─────────────────────────────────────────────────────
void  init_game();
void  stamina_tick();
int   find_next_actor();
void  execute_action();
void  check_win_loss();
void  respawn_enemies();
void* render_thread_func(void*);
void* deadlock_monitor_func(void*);
void* stun_resume_thread(void*);
void  signal_handler(int);
void  cleanup();

struct StunResumeArgs { int entity_idx; bool is_player; };

// ─── Inventory helpers ────────────────────────────────────────────────────────
int find_contiguous_slots(Entity& e, int size) {
    for (int i = 0; i <= INVENTORY_SIZE - size; i++) {
        bool ok = true;
        for (int j = i; j < i + size; j++)
            if (e.inventory[j] != WPN_NONE) { ok = false; break; }
        if (ok) return i;
    }
    return -1;
}

void make_room(Entity& e, int size) {
    while (find_contiguous_slots(e, size) == -1) {
        for (int i = 0; i < INVENTORY_SIZE; i++) {
            if (e.inventory[i] != WPN_NONE) {
                int w  = e.inventory[i];
                int ws = WPN_SLOTS[w];
                for (int j = i; j < i + ws && j < INVENTORY_SIZE; j++)
                    e.inventory[j] = WPN_NONE;
                if (e.lt_count < LONG_TERM_SIZE)
                    e.long_term[e.lt_count++] = w;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Swapped %s to long-term storage", WPN_NAMES[w]);
                break;
            }
        }
    }
}

bool pick_up_weapon(Entity& e, int id) {
    int size = WPN_SLOTS[id];
    int slot = find_contiguous_slots(e, size);
    if (slot == -1) { make_room(e, size); slot = find_contiguous_slots(e, size); }
    if (slot == -1) return false;
    for (int i = slot; i < slot + size; i++) e.inventory[i] = id;
    return true;
}

// Check if entity holds a specific weapon anywhere in inventory
bool entity_has_weapon(Entity& e, int wpn_id) {
    for (int s = 0; s < INVENTORY_SIZE; s++)
        if (e.inventory[s] == wpn_id) return true;
    return false;
}

// ─── Signals ──────────────────────────────────────────────────────────────────
void signal_handler(int sig) {
    if (sig == SIGTERM) { gs->is_game_over = true; running = false; }
    if (sig == SIGCHLD) { int s; waitpid(-1, &s, WNOHANG); }
    if (sig == SIGALRM) {
        if (gs->asp_pid > 0) {
            kill(gs->asp_pid, SIGCONT);
            gs->ultimate_pause = false;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Ultimate ended — enemies resume!");
        }
    }
}

// Stun resume: after 3 seconds clear stun flag and send SIGCONT to the process
void* stun_resume_thread(void* arg) {
    StunResumeArgs* a = (StunResumeArgs*)arg;
    sleep(3);
    pthread_mutex_lock(&gs->memory_lock);
    if (a->is_player && a->entity_idx < gs->num_players)
        gs->players[a->entity_idx].is_stunned = false;
    else if (!a->is_player && a->entity_idx < gs->num_enemies)
        gs->enemies[a->entity_idx].is_stunned = false;
    pthread_mutex_unlock(&gs->memory_lock);
    // Send SIGCONT so the process isn't frozen (stun is now purely flag-based)
    pid_t target_pid = a->is_player ? gs->hip_pid : gs->asp_pid;
    if (target_pid > 0) kill(target_pid, SIGCONT);
    delete a;
    return nullptr;
}

// Trigger stun: set is_stunned flag, start timer thread
void trigger_stun(int entity_idx, bool is_player) {
    pthread_mutex_lock(&gs->memory_lock);
    if (is_player && entity_idx < gs->num_players && gs->players[entity_idx].is_alive)
        gs->players[entity_idx].is_stunned = true;
    else if (!is_player && entity_idx < gs->num_enemies && gs->enemies[entity_idx].is_alive)
        gs->enemies[entity_idx].is_stunned = true;
    pthread_mutex_unlock(&gs->memory_lock);

    StunResumeArgs* a = new StunResumeArgs;
    a->entity_idx = entity_idx;
    a->is_player  = is_player;
    pthread_t t;
    pthread_create(&t, nullptr, stun_resume_thread, a);
    pthread_detach(t);
}

// ─── Shared memory + game init ────────────────────────────────────────────────
void init_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open"); exit(1); }
    ftruncate(fd, sizeof(GameState));
    gs = (GameState*)mmap(0, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    memset(gs, 0, sizeof(GameState));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gs->memory_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

void init_game() {
    srand(ROLL_SEED);
    cout << "=== CHRONO RIFT ===" << endl;
    cout << "Enter number of players (1-4): ";
    cin >> gs->num_players;
    if (gs->num_players < 1 || gs->num_players > MAX_PLAYERS)
        gs->num_players = 1;

    for (int i = 0; i < gs->num_players; i++) {
        Entity& p = gs->players[i];
        p.is_alive   = true;
        p.is_stunned = false;
        p.hp         = ROLL_FULL + (rand() % 901 + 100);
        p.max_hp     = p.hp;
        p.max_stamina = 100;
        p.stamina    = 0;
        p.damage     = LAST_DIGIT + 10;
        p.speed      = 100 / gs->num_players;
        p.lt_count   = 0;
        p.waiting_for = ART_NONE;
        memset(p.inventory, WPN_NONE, sizeof(p.inventory));
        memset(p.long_term,  WPN_NONE, sizeof(p.long_term));
    }

    gs->num_enemies = rand() % 8 + 2;
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity& e = gs->enemies[i];
        e.is_alive   = true;
        e.is_stunned = false;
        e.hp         = LAST_TWO + (rand() % 151 + 50);
        e.max_hp     = e.hp;
        e.max_stamina = 150;
        e.stamina    = 0;
        e.damage     = SECOND_LAST + 10;
        e.speed      = rand() % 21 + 10;
        e.lt_count   = 0;
        e.waiting_for = ART_NONE;
        memset(e.inventory, WPN_NONE, sizeof(e.inventory));
        memset(e.long_term,  WPN_NONE, sizeof(e.long_term));
    }

    gs->is_game_over  = false;
    gs->player_won    = false;
    gs->enemies_killed = 0;
    gs->active_turn   = -1;
    gs->pending_action = ACTION_NONE;
    gs->ultimate_pause = false;

    gs->solar_core_exists  = true;  gs->solar_core_holder  = -1;
    gs->lunar_blade_exists = true;  gs->lunar_blade_holder = -1;
    gs->eclipse_relic_exists   = false;
    gs->eclipse_relic_holder   = -1;
    gs->eclipse_relic_announced = false;

    gs->weapon_drop.pending  = false;
    gs->weapon_drop.weapon_id = WPN_NONE;
    gs->weapon_drop.for_player = -1;
    gs->weapon_drop.decision   = 0;
    // gs->drop_decision          = 0;

    gs->arbiter_pid = getpid();
    snprintf(gs->action_log, MAX_ACTION_LOG,
             "Chrono Rift! Players:%d  Enemies:%d",
             gs->num_players, gs->num_enemies);
    cout << "Players:" << gs->num_players
         << "  Enemies:" << gs->num_enemies << endl;
}

// ─── Respawn enemies ──────────────────────────────────────────────────────────
void respawn_enemies() {
    int count = rand() % 3 + 2; // 2-4 new enemies
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    gs->num_enemies = count;
    for (int i = 0; i < count; i++) {
        Entity& e = gs->enemies[i];
        e.is_alive   = true;
        e.is_stunned = false;
        e.hp         = LAST_TWO + (rand() % 151 + 50);
        e.max_hp     = e.hp;
        e.max_stamina = 150;
        e.stamina    = 0;
        e.damage     = SECOND_LAST + 10;
        e.speed      = rand() % 21 + 10;
        e.lt_count   = 0;
        e.waiting_for = ART_NONE;
        memset(e.inventory, WPN_NONE, sizeof(e.inventory));
        memset(e.long_term,  WPN_NONE, sizeof(e.long_term));
    }
    snprintf(gs->action_log, MAX_ACTION_LOG,
             "New wave! %d enemies approach...", count);
}

// ─── Stamina tick ─────────────────────────────────────────────────────────────
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

// True arrival-time scheduling: entity with highest stamina among those full goes first
int find_next_actor() {
    int  best     = -1;
    int  best_sta = -1;
    for (int i = 0; i < gs->num_players; i++) {
        Entity& p = gs->players[i];
        if (p.is_alive && !p.is_stunned && p.stamina >= p.max_stamina) {
            if (p.stamina > best_sta) { best_sta = p.stamina; best = i; }
        }
    }
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity& e = gs->enemies[i];
        if (e.is_alive && !e.is_stunned && e.stamina >= e.max_stamina) {
            if (e.stamina > best_sta) { best_sta = e.stamina; best = MAX_PLAYERS + i; }
        }
    }
    return best;
}

// ─── Execute action ───────────────────────────────────────────────────────────
void execute_action() {
    if (gs->pending_action == ACTION_NONE) return;
    pthread_mutex_lock(&gs->memory_lock);

    int actor  = gs->action_actor;
    int target = gs->action_target;
    int action = gs->pending_action;
    int wpn    = gs->action_weapon_id;
    bool ap    = (actor  < MAX_PLAYERS);
    bool tp    = (target < MAX_PLAYERS);

    Entity& ae = ap ? gs->players[actor]
                    : gs->enemies[actor - MAX_PLAYERS];
    // Target entity only valid for actions that use it
    Entity* te_ptr = nullptr;
    if (action == ACTION_STRIKE || action == ACTION_EXHAUST ||
        action == ACTION_USE_WEAPON) {
        te_ptr = tp ? &gs->players[target]
                    : &gs->enemies[target - MAX_PLAYERS];
    }

    switch (action) {
        // ── Strike ────────────────────────────────────────────────────────
        case ACTION_STRIKE:
            if (te_ptr) {
                te_ptr->hp -= ae.damage;
                ae.stamina = 0;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d STRIKES entity %d for %d dmg",
                         actor, target, ae.damage);
                atk_trig.attacker = actor;
                atk_trig.target   = target;
                atk_trig.fired    = true;
            }
            break;

        // ── Exhaust (stun) ────────────────────────────────────────────────
        case ACTION_EXHAUST:
            if (te_ptr) {
                te_ptr->stamina -= ae.damage;
                if (te_ptr->stamina < 0) te_ptr->stamina = 0;
                ae.stamina = 0;
                int tidx = tp ? target : (target - MAX_PLAYERS);
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d EXHAUSTS entity %d — STUNNED!",
                         actor, target);
                pthread_mutex_unlock(&gs->memory_lock);
                trigger_stun(tidx, tp);
                pthread_mutex_lock(&gs->memory_lock);
            }
            break;

        // ── Use weapon ────────────────────────────────────────────────────
        case ACTION_USE_WEAPON:
            if (wpn >= 0 && wpn < WPN_COUNT && te_ptr) {
                te_ptr->hp -= WPN_DAMAGE[wpn];
                ae.stamina = 0;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d uses %s → %d dmg!",
                         actor, WPN_NAMES[wpn], WPN_DAMAGE[wpn]);
                atk_trig.attacker = actor;
                atk_trig.target   = target;
                atk_trig.fired    = true;
            }
            break;

        // ── Ultimate ability ──────────────────────────────────────────────
        case ACTION_ULTIMATE:
            ae.stamina = 0;
            gs->ultimate_pause = true;
            ultimate_flash_timer = 1.5f;
            if (gs->asp_pid > 0) kill(gs->asp_pid, SIGSTOP);
            alarm(10);
            // Massive hit on ALL enemies
            for (int i = 0; i < gs->num_enemies; i++) {
                if (gs->enemies[i].is_alive)
                    gs->enemies[i].hp -= (ae.damage * 3);
            }
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "ULTIMATE ACTIVATED — Enemies frozen 10s! All enemies hit!");
            break;

        // ── Heal ──────────────────────────────────────────────────────────
        case ACTION_HEAL:
            ae.hp += ae.max_hp / 10;
            if (ae.hp > ae.max_hp) ae.hp = ae.max_hp;
            ae.stamina = 0;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Entity %d HEALS for %d HP", actor, ae.max_hp / 10);
            break;

        // ── Skip ──────────────────────────────────────────────────────────
        case ACTION_SKIP:
            ae.stamina = ae.max_stamina / 2;
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "Entity %d SKIPS", actor);
            break;

        // ── Swap in ───────────────────────────────────────────────────────
        case ACTION_SWAP_IN:
            if (wpn >= 0 && wpn < ae.lt_count) {
                int wid = ae.long_term[wpn];
                pick_up_weapon(ae, wid);
                for (int k = wpn; k < ae.lt_count - 1; k++)
                    ae.long_term[k] = ae.long_term[k + 1];
                ae.lt_count--;
                ae.stamina = 0;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d swapped in %s", actor, WPN_NAMES[wid]);
            }
            break;
    }

    // ── Check enemy death + weapon drop + eclipse relic ───────────────────
    if (te_ptr && !tp && te_ptr->hp <= 0 && te_ptr->is_alive) {
        te_ptr->is_alive = false;
        gs->enemies_killed++;
        int eidx = target - MAX_PLAYERS;

        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d defeated! Total kills: %d/10", eidx, gs->enemies_killed);

        // Eclipse Relic appears at kill #3
        if (gs->enemies_killed == 3 && !gs->eclipse_relic_exists) {
            gs->eclipse_relic_exists = true;
            gs->eclipse_relic_holder = -1;
            // Will show in popup as the dropped weapon
        }

        // Weapon drop (50% chance, only if enemy has no weapon)
        bool enemy_has_wpn = false;
        for (int s = 0; s < INVENTORY_SIZE; s++)
            if (te_ptr->inventory[s] != WPN_NONE) { enemy_has_wpn = true; break; }

        if (!enemy_has_wpn && (rand() % 100) < 50) {
            // Pick the drop weapon
            int drop_wpn;
            if (gs->enemies_killed == 3 && gs->eclipse_relic_exists
                && !gs->eclipse_relic_announced) {
                drop_wpn = WPN_ECLIPSE_RELIC;
                gs->eclipse_relic_announced = true;
            } else {
                // Random weapon, bias away from artifacts for common drops
                drop_wpn = rand() % WPN_COUNT;
            }
            // Find which player is active (or first alive player)
            int for_player = ap ? actor : 0;
            if (for_player >= gs->num_players) for_player = 0;

            gs->weapon_drop.pending   = true;
            gs->weapon_drop.weapon_id = drop_wpn;
            gs->weapon_drop.for_player = for_player;
            gs->weapon_drop.decision  = 0;
        }
    }

    // ── Check player death ─────────────────────────────────────────────────
    if (te_ptr && tp && te_ptr->hp <= 0 && te_ptr->is_alive) {
        te_ptr->is_alive = false;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Player %d has fallen!", target + 1);
    }

    gs->pending_action = ACTION_NONE;
    gs->active_turn    = -1;
    pthread_mutex_unlock(&gs->memory_lock);
}

// ─── Check win/loss + respawn ─────────────────────────────────────────────────
void check_win_loss() {
    if (gs->enemies_killed >= 10) {
        gs->is_game_over = true;
        gs->player_won   = true;
        return;
    }
    // Check if all current enemies dead → respawn
    bool any_alive_enemy = false;
    for (int i = 0; i < gs->num_enemies; i++)
        if (gs->enemies[i].is_alive) { any_alive_enemy = true; break; }
    if (!any_alive_enemy && gs->enemies_killed < 10)
        respawn_enemies();

    // Check all players dead
    bool any_alive_player = false;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].is_alive) { any_alive_player = true; break; }
    if (!any_alive_player) {
        gs->is_game_over = true;
        gs->player_won   = false;
    }
}

// ─── Deadlock monitor ────────────────────────────────────────────────────────
void* deadlock_monitor_func(void*) {
    while (!gs->is_game_over) {
        sleep(2);
        pthread_mutex_lock(&gs->memory_lock);

        // Build a simple hold/wait graph for the two artifacts
        // solar_core_holder holds ART_SOLAR_CORE
        // lunar_blade_holder holds ART_LUNAR_BLADE
        int sc_holder = gs->solar_core_holder;
        int lb_holder = gs->lunar_blade_holder;

        if (sc_holder != -1 && lb_holder != -1 && sc_holder != lb_holder) {
            // Get the waiting_for of each holder
            auto get_waiting = [&](int idx) -> int {
                if (idx < MAX_PLAYERS) return gs->players[idx].waiting_for;
                int ei = idx - MAX_PLAYERS;
                if (ei < gs->num_enemies) return gs->enemies[ei].waiting_for;
                return ART_NONE;
            };

            int sc_wait = get_waiting(sc_holder);  // what does solar holder want?
            int lb_wait = get_waiting(lb_holder);  // what does lunar holder want?

            // Circular: sc_holder wants Lunar, lb_holder wants Solar
            if (sc_wait == ART_LUNAR_BLADE && lb_wait == ART_SOLAR_CORE) {
                // Break deadlock: force higher-index entity to release
                int release_idx = (sc_holder > lb_holder) ? sc_holder : lb_holder;
                if (release_idx == sc_holder) {
                    gs->solar_core_holder = -1;
                    if (release_idx < MAX_PLAYERS)
                        gs->players[release_idx].waiting_for = ART_NONE;
                    else
                        gs->enemies[release_idx - MAX_PLAYERS].waiting_for = ART_NONE;
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "[Arbiter] DEADLOCK! Freed Solar Core from entity %d",
                             release_idx);
                } else {
                    gs->lunar_blade_holder = -1;
                    if (release_idx < MAX_PLAYERS)
                        gs->players[release_idx].waiting_for = ART_NONE;
                    else
                        gs->enemies[release_idx - MAX_PLAYERS].waiting_for = ART_NONE;
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "[Arbiter] DEADLOCK! Freed Lunar Blade from entity %d",
                             release_idx);
                }
            }
        }
        pthread_mutex_unlock(&gs->memory_lock);
    }
    return nullptr;
}

void cleanup() {
    pthread_mutex_destroy(&gs->memory_lock);
    munmap(gs, sizeof(GameState));
    shm_unlink(SHARED_MEM_NAME);
    cout << "Arbiter: cleaned up." << endl;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  RENDER THREAD — Expedition-33 style 2D arena + full GUI input
// ═══════════════════════════════════════════════════════════════════════════════

sf::Vector2f player_foot(int i, int n) {
    float x = (n <= 1) ? (P_X1 + P_X2) / 2.f
                       : P_X1 + (P_X2 - P_X1) * ((float)i / (n - 1));
    return {x, GROUND_Y - (float)i * 14.f};
}

sf::Vector2f enemy_foot(int i, int n) {
    float x = (n <= 1) ? (E_X1 + E_X2) / 2.f
                       : 600 + (E_X2 - E_X1) * (1.5f * i / n);
    return {x, GROUND_Y - (float)i * 4.f};
}

void draw_bars(sf::RenderWindow& win, sf::Font& font,
               float cx, float foot_y,
               int hp, int mhp, int sp, int msp,
               bool dead, const char* name, bool compact = false)
{
    if (dead) return;
    float bw = compact ? 44.f : 80.f, bh = 8.f;
    float bx = cx - bw / 2.f, by = foot_y - CELL_H - 24.f;

    sf::RectangleShape bg({bw + 4.f, bh * 2.f + 8.f});
    bg.setFillColor({0, 0, 0, 180});
    bg.setPosition(bx - 2.f, by - 2.f);
    win.draw(bg);

    float hr = (float)hp / mhp; if (hr < 0) hr = 0; if (hr > 1) hr = 1;
    sf::Color hc = hr > 0.5f ? sf::Color{50,220,80}
                 : hr > 0.25f ? sf::Color{230,200,0}
                 : sf::Color{220,50,50};
    sf::RectangleShape hbar({bw * hr, bh});
    hbar.setFillColor(hc); hbar.setPosition(bx, by); win.draw(hbar);

    float sr = (float)sp / msp; if (sr < 0) sr = 0; if (sr > 1) sr = 1;
    sf::RectangleShape sbar({bw * sr, bh - 2.f});
    sbar.setFillColor({255, 200, 0});
    sbar.setPosition(bx, by + bh + 2.f); win.draw(sbar);

    sf::Text t(name, font, compact ? 10u : 11u);
    t.setFillColor({220, 220, 255});
    sf::FloatRect tb = t.getLocalBounds();
    t.setPosition(cx - tb.width / 2.f, by - 16.f); win.draw(t);

    char vals[32];
    snprintf(vals, sizeof(vals), "%d/%d  %d/%d", hp, mhp, sp, msp);
    sf::Text v(vals, font, compact ? 8u : 9u);
    v.setFillColor({200, 200, 200});
    sf::FloatRect vb = v.getLocalBounds();
    v.setPosition(cx - vb.width / 2.f, by + bh + 12.f); win.draw(v);
}

void draw_slash(sf::RenderWindow& win, float sx, float sy, float p) {
    struct L { float ax, ay, len; };
    L lines[] = {{-0.5f,-0.3f,80},{-0.2f,-0.5f,100},{0.1f,-0.4f,110},{0.4f,-0.2f,85},{0.6f,0.1f,60}};
    for (auto& l : lines) {
        float len = l.len * p;
        sf::Uint8 alpha = (sf::Uint8)(255 * (1.f - p));
        sf::Vertex vx[] = {
            sf::Vertex({sx, sy}, {255,255,100,alpha}),
            sf::Vertex({sx + cosf(l.ax)*len, sy + sinf(l.ay)*len}, {255,255,255,0})
        };
        win.draw(vx, 2, sf::Lines);
    }
    float r = 30.f * p;
    sf::CircleShape c(r, 12);
    c.setFillColor({255,220,100,(sf::Uint8)(120*(1.f-p))});
    c.setOrigin(r, r); c.setPosition(sx, sy); win.draw(c);
}

// ── Weapon icon drawing ───────────────────────────────────────────────────────
// PLACEHOLDER: draws a coloured labelled box for each weapon.
// Replace with: sf::Sprite from weapons.png when you have the sprite sheet.
// weapons.png should be a 9-cell horizontal sheet, each cell 64x64.
// Cell order: Solar Core(0), Lunar Blade(1), Iron Halberd(2), Venom Dagger(3),
//             Thunderstaff(4), Obsidian Axe(5), Frostbow(6), Splinter Stick(7), Eclipse Relic(8)
//
// TO SWAP IN SPRITE: load sf::Texture wpn_tex from "Sprites/weapons.png"
// then: spr.setTextureRect({wpn_id * 64, 0, 64, 64}); spr.setPosition(x,y); win.draw(spr);

static const sf::Color WPN_COLORS[WPN_COUNT] = {
    {255,200, 50},   // Solar Core      — gold
    {180,180,255},   // Lunar Blade     — silver-blue
    {140, 80, 40},   // Iron Halberd    — brown
    {80, 200, 80},   // Venom Dagger    — green
    {100,100,255},   // Thunderstaff    — electric blue
    {60,  60, 60},   // Obsidian Axe   — dark grey
    {80, 200,220},   // Frostbow        — ice blue
    {180,120, 60},   // Splinter Stick  — light brown
    {200, 50,200},   // Eclipse Relic   — purple
};

void draw_weapon_icon(sf::RenderWindow& win, sf::Font& font,
                      int wpn_id, float x, float y,
                      float size = 40.f, bool highlighted = false,
                      bool use_sprite = false, sf::Texture* wpn_tex = nullptr)
{
    if (wpn_id < 0 || wpn_id >= WPN_COUNT) {
        // Empty slot — grey box
        sf::RectangleShape box({size, size});
        box.setFillColor({40,40,40,200});
        box.setOutlineThickness(1.f);
        box.setOutlineColor({80,80,80});
        box.setPosition(x, y); win.draw(box);
        return;
    }

    // TODO: When weapons.png is ready, replace this block with sprite drawing:
    // if (use_sprite && wpn_tex) {
    //     sf::Sprite spr(*wpn_tex);
    //     spr.setTextureRect({wpn_id * 64, 0, 64, 64});
    //     float scale = size / 64.f;
    //     spr.setScale(scale, scale);
    //     spr.setPosition(x, y);
    //     if (highlighted) spr.setColor({255,255,100,255});
    //     win.draw(spr);
    //     return;
    // }

    // ── Placeholder coloured box ──────────────────────────────────────────
    sf::Color col = WPN_COLORS[wpn_id];
    sf::RectangleShape box({size, size});
    box.setFillColor({(sf::Uint8)(col.r/2), (sf::Uint8)(col.g/2), (sf::Uint8)(col.b/2), 220});
    box.setOutlineThickness(highlighted ? 3.f : 1.5f);
    box.setOutlineColor(highlighted ? sf::Color{255,255,0} : col);
    box.setPosition(x, y);
    win.draw(box);

    // Weapon initial letter in box
    char abbr[3] = {WPN_NAMES[wpn_id][0], WPN_NAMES[wpn_id][1], '\0'};
    sf::Text lbl(abbr, font, (unsigned)(size * 0.4f));
    lbl.setFillColor(col);
    sf::FloatRect lb = lbl.getLocalBounds();
    lbl.setPosition(x + size/2.f - lb.width/2.f, y + size/2.f - lb.height/2.f - 2.f);
    win.draw(lbl);
}

// ── Draw action panel button ──────────────────────────────────────────────────
bool draw_button(sf::RenderWindow& win, sf::Font& font,
                 const char* label, float x, float y, float w, float h,
                 sf::Vector2i mouse, bool active = true,
                 sf::Color base_col = {50,50,80,220})
{
    sf::FloatRect rect(x, y, w, h);
    bool hovered = active && rect.contains((float)mouse.x, (float)mouse.y);
    sf::RectangleShape btn({w, h});
    btn.setPosition(x, y);
    btn.setFillColor(hovered ? sf::Color{80,80,140,240} : base_col);
    btn.setOutlineThickness(hovered ? 2.5f : 1.f);
    btn.setOutlineColor(active ? sf::Color{120,120,220} : sf::Color{60,60,60});
    win.draw(btn);

    sf::Text t(label, font, 13u);
    t.setFillColor(active ? sf::Color{220,220,255} : sf::Color{80,80,80});
    sf::FloatRect tb = t.getLocalBounds();
    t.setPosition(x + w/2.f - tb.width/2.f, y + h/2.f - tb.height/2.f - 2.f);
    win.draw(t);
    return hovered;
}

// ─── Render thread ────────────────────────────────────────────────────────────
void* render_thread_func(void*) {
    const char* disp = getenv("DISPLAY");
    if (!disp || !disp[0]) {
        while (!gs->is_game_over) {
            cout << "\r[Game] Kills:" << gs->enemies_killed
                 << " Turn:" << gs->active_turn
                 << " " << gs->action_log << "    " << flush;
            usleep(500000);
        }
        return nullptr;
    }

    sf::RenderWindow win(sf::VideoMode(WIN_W, WIN_H), "Chrono Rift — Arena",
                         sf::Style::Titlebar | sf::Style::Close);
    win.setFramerateLimit(60);

    sf::Font font;
    if (!font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"))
        font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    // ── Load textures ─────────────────────────────────────────────────────
    sf::Texture texP, texS, texR, texB, bg_tex;
    bool hasP  = texP.loadFromFile("Sprites/players.png");
    bool hasS  = texS.loadFromFile("Sprites/enemy_silver_flip.png");
    bool hasR  = texR.loadFromFile("Sprites/enemy_red_flip.png");
    bool hasB  = texB.loadFromFile("Sprites/enemy_blue_flip.png");
    bool hasBg = bg_tex.loadFromFile("Sprites/backgrounds_2.jpeg");
    texP.setSmooth(false); texS.setSmooth(false);
    texR.setSmooth(false); texB.setSmooth(false);

    // Weapon sprite sheet — PLACEHOLDER until weapons.png is provided
    // sf::Texture wpn_tex;
    // bool hasWpn = wpn_tex.loadFromFile("Sprites/weapons.png");

    // ── Per-entity state ──────────────────────────────────────────────────
    float flash[MAX_PLAYERS + MAX_ENEMIES] = {};
    float attack_fx_timer = 0.f;
    int   attack_fx_target = -1;
    static const float ATTACK_FX_DUR = 1.f;

    int   lng_actor  = -1, lng_target = -1;
    float lng_timer  = 0.f;
    bool  lng_hit    = false;
    static const float LNG_DUR = 0.75f;

    // ── GUI state ─────────────────────────────────────────────────────────
    // ui_mode: 0=normal, 1=picking target(strike), 2=picking target(exhaust),
    //          3=picking weapon(use), 4=picking lt_slot(swap in)
    //          5=picking weapon+target(use weapon — step 2)
    int  ui_mode        = 0;
    int  ui_weapon_sel  = -1;   // selected weapon id for USE_WEAPON
    bool ui_can_ultimate = false;

    sf::Clock clock;
    float prev_t = 0.f;

    // Weapon drop popup: local copy of decision pending
    bool popup_showing   = false;
    int  popup_weapon_id = WPN_NONE;

    while (win.isOpen() && !gs->is_game_over) {
        sf::Event ev;
        sf::Vector2i mouse = sf::Mouse::getPosition(win);
        bool left_clicked = false;

        while (win.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) win.close();
            if (ev.type == sf::Event::MouseButtonReleased &&
                ev.mouseButton.button == sf::Mouse::Left)
                left_clicked = true;
        }

        float now = clock.getElapsedTime().asSeconds();
        float dt  = now - prev_t; prev_t = now;

        // Sync popup state
        if (gs->weapon_drop.pending && !popup_showing) {
            popup_showing   = true;
            popup_weapon_id = gs->weapon_drop.weapon_id;
            ui_mode = 0; // pause other UI during popup
        }
        if (!gs->weapon_drop.pending) popup_showing = false;

        // ── Attack trigger / lunge ────────────────────────────────────────
        if (atk_trig.fired && lng_actor == -1) {
            lng_actor  = atk_trig.attacker;
            lng_target = atk_trig.target;
            lng_timer  = 0.f; lng_hit = false;
            atk_trig.fired = false;
        }
        float lng_ofs = 0.f, slash_prog = 0.f;
        if (lng_actor != -1) {
            lng_timer += dt;
            float p = lng_timer / LNG_DUR;
            if      (p < 0.4f)  { lng_ofs = (p / 0.4f) * 140.f; }
            else if (p < 0.62f) {
                lng_ofs = 140.f; slash_prog = (p - 0.4f) / 0.22f;
                if (!lng_hit) {
                    int ti = lng_target;
                    if (ti >= 0 && ti < MAX_PLAYERS + MAX_ENEMIES) flash[ti] = 1.f;
                    attack_fx_target = ti; attack_fx_timer = ATTACK_FX_DUR;
                    lng_hit = true;
                }
            }
            else if (p < 1.f)   { lng_ofs = (1.f-(p-0.62f)/0.38f)*140.f; }
            else                  { lng_actor = -1; lng_target = -1; }
        }

        for (int i = 0; i < MAX_PLAYERS + MAX_ENEMIES; i++) {
            flash[i] -= dt * 3.f; if (flash[i] < 0) flash[i] = 0;
        }
        if (attack_fx_timer > 0.f) {
            attack_fx_timer -= dt; if (attack_fx_timer < 0.f) attack_fx_timer = 0.f;
        }
        if (ultimate_flash_timer > 0.f)
            ultimate_flash_timer -= dt;

        // ── Clear + background ────────────────────────────────────────────
        win.clear({14, 11, 26});
        if (hasBg) {
            sf::Sprite bg(bg_tex);
            float sx = (float)WIN_W / bg_tex.getSize().x;
            float sy = (float)WIN_H / bg_tex.getSize().y;
            bg.setScale(sx, sy);
            win.draw(bg);
        }

        int np = gs->num_players, ne = gs->num_enemies;

        // ── Sprite helpers ────────────────────────────────────────────────
        auto apply_color = [&](sf::Sprite& spr, bool dead, float fl) {
            if (dead)       { spr.setColor({80,80,80,140}); return; }
            if (fl > 0.f)   { spr.setColor({255,(sf::Uint8)(255*(1.f-fl)),
                                            (sf::Uint8)(255*(1.f-fl)),255}); return; }
            spr.setColor(sf::Color::White);
        };
        auto draw_flipped = [&](sf::Texture& tex, int frame, float cx, float fy,
                                 bool flip_x, bool dead, float fl) {
            sf::Sprite spr(tex);
            spr.setTextureRect({frame * CELL_W, 0, CELL_W, CELL_H});
            spr.setOrigin(CELL_W / 2.f, (float)CELL_H);
            spr.setScale(flip_x ? -1.f : 1.f, 1.f);
            spr.setPosition(cx, fy);
            apply_color(spr, dead, fl);
            win.draw(spr);
        };

        // ── Draw players ──────────────────────────────────────────────────
        for (int i = 0; i < np; i++) {
            Entity& p = gs->players[i];
            sf::Vector2f fp = player_foot(i, np);
            float cx = fp.x, fy = fp.y;
            bool is_act = (gs->active_turn == i);
            float ox = (lng_actor == i) ? lng_ofs : 0.f;

            sf::CircleShape sh(28.f, 18); sh.setScale(1.f, 0.2f);
            sh.setFillColor({0,0,0, p.is_alive ? (sf::Uint8)60 : (sf::Uint8)20});
            sh.setOrigin(28.f, 28.f); sh.setPosition(cx+ox, fy); win.draw(sh);

            if (is_act && p.is_alive) {
                float pulse = 0.55f + 0.45f*sinf(now*5.f);
                sf::CircleShape g(48.f, 32); g.setFillColor({0,0,0,0});
                g.setOutlineThickness(4.f);
                g.setOutlineColor({255,(sf::Uint8)(220*pulse),0,(sf::Uint8)(210*pulse)});
                g.setOrigin(48.f, 48.f); g.setPosition(cx+ox, fy-48.f); win.draw(g);
            }

            if (hasP) {
                // Each player uses their own column in players.png
                int hero_col = i % 5;
                draw_flipped(texP, hero_col, cx+ox, fy, true, !p.is_alive, flash[i]);
            } else {
                sf::RectangleShape r({40.f, 70.f});
                r.setFillColor(p.is_alive ? sf::Color{100,160,255} : sf::Color{60,60,60});
                r.setOrigin(20.f, 70.f); r.setPosition(cx+ox, fy); win.draw(r);
            }

            // Stun stars
            if (p.is_stunned && p.is_alive) {
                for (int s = 0; s < 3; s++) {
                    float ang = (float)s/3.f*6.2832f + now*3.f;
                    sf::CircleShape star(5.f, 5); star.setFillColor({255,220,0});
                    star.setOrigin(5.f, 5.f);
                    star.setPosition(cx+ox+cosf(ang)*22.f, fy-CELL_H+sinf(ang)*8.f);
                    win.draw(star);
                }
                sf::Text st("STUNNED", font, 11u);
                st.setFillColor({255,220,0});
                sf::FloatRect stb = st.getLocalBounds();
                st.setPosition(cx - stb.width/2.f, fy - CELL_H - 40.f); win.draw(st);
            }

            char lbl[16]; snprintf(lbl, sizeof(lbl), "P%d", i+1);
            draw_bars(win, font, cx+ox, fy, p.hp, p.max_hp,
                      p.stamina, p.max_stamina, !p.is_alive, lbl, false);
        }

        // ── Draw enemies ──────────────────────────────────────────────────
        for (int i = 0; i < ne; i++) {
            Entity& e = gs->enemies[i];
            sf::Vector2f fp = enemy_foot(i, ne);
            float cx = fp.x, fy = fp.y;
            int eid = MAX_PLAYERS + i;
            bool is_act = (gs->active_turn == eid);
            float ox = (lng_actor == eid) ? -lng_ofs : 0.f;

            sf::CircleShape sh(28.f, 18); sh.setScale(1.f, 0.2f);
            sh.setFillColor({0,0,0, e.is_alive ? (sf::Uint8)60 : (sf::Uint8)20});
            sh.setOrigin(28.f, 28.f); sh.setPosition(cx+ox, fy); win.draw(sh);

            if (is_act && e.is_alive) {
                float pulse = 0.55f + 0.45f*sinf(now*5.f);
                sf::CircleShape g(48.f, 32); g.setFillColor({0,0,0,0});
                g.setOutlineThickness(4.f);
                g.setOutlineColor({255,(sf::Uint8)(80*pulse),80,(sf::Uint8)(210*pulse)});
                g.setOrigin(48.f, 48.f); g.setPosition(cx+ox, fy-48.f); win.draw(g);
            }

            sf::Texture* et = nullptr;
            if      (i%3==0 && hasS) et = &texS;
            else if (i%3==1 && hasR) et = &texR;
            else if (hasB)           et = &texB;
            else if (hasS)           et = &texS;

            // For enemy lunge, cycle through frames
            int frame = (lng_actor == eid) ? (int)(slash_prog * (ANIM_FRAMES-1)) : 0;

            if (et) {
                draw_flipped(*et, frame, cx+ox, fy, true, !e.is_alive, flash[eid]);
            } else {
                sf::RectangleShape r({40.f, 60.f});
                r.setFillColor(e.is_alive ? sf::Color{200,60,60} : sf::Color{60,60,60});
                r.setOrigin(20.f, 60.f); r.setPosition(cx+ox, fy); win.draw(r);
            }

            // Stun stars + label
            if (e.is_stunned && e.is_alive) {
                for (int s = 0; s < 3; s++) {
                    float ang = (float)s/3.f*6.2832f + now*3.f;
                    sf::CircleShape star(5.f, 5); star.setFillColor({255,220,0});
                    star.setOrigin(5.f, 5.f);
                    star.setPosition(cx+ox+cosf(ang)*22.f, fy-CELL_H+sinf(ang)*8.f);
                    win.draw(star);
                }
                sf::Text st("STUNNED", font, 11u);
                st.setFillColor({255,220,0});
                sf::FloatRect stb = st.getLocalBounds();
                st.setPosition(cx - stb.width/2.f, fy - CELL_H - 40.f); win.draw(st);
            }

            // Target highlight when player is choosing enemy
            if ((ui_mode == 1 || ui_mode == 2 || ui_mode == 5) && e.is_alive) {
                sf::FloatRect hitbox(cx - 50.f, fy - CELL_H, 100.f, (float)CELL_H);
                bool hov = hitbox.contains((float)mouse.x, (float)mouse.y);
                sf::RectangleShape hl({100.f, (float)CELL_H});
                hl.setPosition(cx - 50.f, fy - CELL_H);
                hl.setFillColor({255,255,0, (sf::Uint8)(hov ? 60 : 20)});
                hl.setOutlineThickness(2.f);
                hl.setOutlineColor({255,255,0, (sf::Uint8)(hov ? 200 : 80)});
                win.draw(hl);

                if (hov && left_clicked && !popup_showing) {
                    int tgt = MAX_PLAYERS + i;
                    if (ui_mode == 1) {
                        gs->action_target  = tgt;
                        gs->action_actor   = gs->active_turn;
                        gs->pending_action = ACTION_STRIKE;
                    } else if (ui_mode == 2) {
                        gs->action_target  = tgt;
                        gs->action_actor   = gs->active_turn;
                        gs->pending_action = ACTION_EXHAUST;
                    } else if (ui_mode == 5) {
                        gs->action_target    = tgt;
                        gs->action_actor     = gs->active_turn;
                        gs->action_weapon_id = ui_weapon_sel;
                        gs->pending_action   = ACTION_USE_WEAPON;
                    }
                    ui_mode = 0;
                }
            }

            char lbl[16]; snprintf(lbl, sizeof(lbl), "E%d", i+1);
            draw_bars(win, font, cx+ox, fy, e.hp, e.max_hp,
                      e.stamina, e.max_stamina, !e.is_alive, lbl, true);
        }

        // ── Slash effect ──────────────────────────────────────────────────
        if (lng_actor != -1 && slash_prog > 0.f) {
            bool tp = (lng_target < MAX_PLAYERS);
            float sx, sy;
            if (tp && lng_target < np) {
                sf::Vector2f fp = player_foot(lng_target, np);
                sx = fp.x; sy = fp.y - CELL_H * 0.5f;
            } else {
                int ti = lng_target - MAX_PLAYERS;
                if (ti < ne) { sf::Vector2f fp = enemy_foot(ti, ne); sx = fp.x; sy = fp.y - CELL_H * 0.5f; }
                else { sx = WIN_W / 2.f; sy = GROUND_Y - 60.f; }
            }
            draw_slash(win, sx, sy, slash_prog);
        }

        // ── TOP HUD ───────────────────────────────────────────────────────
        sf::RectangleShape hud_top({(float)WIN_W, 42.f});
        hud_top.setFillColor({0,0,0,200}); win.draw(hud_top);

        char ks[64]; snprintf(ks, sizeof(ks), "Kills: %d / 10", gs->enemies_killed);
        sf::Text kt(ks, font, 20); kt.setFillColor({255,210,0}); kt.setPosition(20.f, 10.f); win.draw(kt);

        if (gs->active_turn >= 0) {
            bool ip  = (gs->active_turn < MAX_PLAYERS);
            int  idx = ip ? gs->active_turn + 1 : gs->active_turn - MAX_PLAYERS + 1;
            char ts[64]; snprintf(ts, sizeof(ts), "%s %d's Turn",
                                  ip ? "Player" : "Enemy", idx);
            sf::Text tt(ts, font, 22);
            float pulse = 0.5f + 0.5f*sinf(now*4.f);
            tt.setFillColor(ip ? sf::Color{100,200,255}
                               : sf::Color{255,(sf::Uint8)(80+40*pulse),80,255});
            sf::FloatRect tb = tt.getLocalBounds();
            tt.setPosition(WIN_W/2.f - tb.width/2.f, 10.f); win.draw(tt);
        }

        if (gs->ultimate_pause) {
            sf::Text ult("ULTIMATE: Enemies Frozen!", font, 17);
            ult.setFillColor({0,220,255});
            sf::FloatRect ub = ult.getLocalBounds();
            ult.setPosition(WIN_W/2.f - ub.width/2.f, WIN_H - 52.f); win.draw(ult);
        }

        // ── BOTTOM LOG BAR ────────────────────────────────────────────────
        sf::RectangleShape bot({(float)WIN_W, 34.f});
        bot.setFillColor({0,0,0,215}); bot.setPosition(0.f, (float)WIN_H-34.f); win.draw(bot);
        sf::Text logtext(gs->action_log, font, 14);
        logtext.setFillColor({0,230,195}); logtext.setPosition(16.f, (float)WIN_H-27.f); win.draw(logtext);

        // ── Ultimate flash overlay ────────────────────────────────────────
        if (ultimate_flash_timer > 0.f) {
            float alpha = ultimate_flash_timer / 1.5f;
            sf::RectangleShape flash_overlay({(float)WIN_W, (float)WIN_H});
            flash_overlay.setFillColor({0, 200, 255, (sf::Uint8)(180 * alpha)});
            win.draw(flash_overlay);
            // Glow text
            for (int g = 0; g < 4; g++) {
                sf::Text glow("ULTIMATE!", font, 90u);
                glow.setFillColor({0, 100, 150, (sf::Uint8)(40 * alpha)});
                sf::FloatRect gb = glow.getLocalBounds();
                glow.setOrigin(gb.width/2.f, gb.height/2.f);
                glow.setPosition(WIN_W/2.f + (g%2==0?-g:g)*3.f,
                                 WIN_H/2.f + (g/2==0?-g:g)*3.f - 30.f);
                win.draw(glow);
            }
            sf::Text ult_txt("ULTIMATE!", font, 90u);
            ult_txt.setFillColor({255, 220, 50, (sf::Uint8)(255 * alpha)});
            sf::FloatRect ub = ult_txt.getLocalBounds();
            ult_txt.setOrigin(ub.width/2.f, ub.height/2.f);
            ult_txt.setPosition(WIN_W/2.f, WIN_H/2.f - 30.f);
            win.draw(ult_txt);
        }

        // ── WEAPON DROP POPUP ─────────────────────────────────────────────
        if (popup_showing && gs->weapon_drop.pending) {
            int  wpn_id     = gs->weapon_drop.weapon_id;
            int  for_player = gs->weapon_drop.for_player;

            // Dark overlay
            sf::RectangleShape overlay({(float)WIN_W, (float)WIN_H});
            overlay.setFillColor({0,0,0,160}); win.draw(overlay);

            // Popup box
            float pw = 420.f, ph = 280.f;
            float px = WIN_W/2.f - pw/2.f, py = WIN_H/2.f - ph/2.f;
            sf::RectangleShape popup_bg({pw, ph});
            popup_bg.setFillColor({20,15,35,250});
            popup_bg.setOutlineThickness(3.f);
            popup_bg.setOutlineColor(wpn_id >= 0 && wpn_id < WPN_COUNT
                                     ? WPN_COLORS[wpn_id]
                                     : sf::Color{200,200,200});
            popup_bg.setPosition(px, py); win.draw(popup_bg);

            // Title
            sf::Text title("WEAPON DROPPED!", font, 22u);
            title.setFillColor({255,220,50});
            sf::FloatRect tb = title.getLocalBounds();
            title.setPosition(px + pw/2.f - tb.width/2.f, py + 16.f); win.draw(title);

            // Weapon icon (placeholder box)
            if (wpn_id >= 0 && wpn_id < WPN_COUNT) {
                draw_weapon_icon(win, font, wpn_id, px + pw/2.f - 32.f, py + 56.f, 64.f);

                // Weapon name
                sf::Text wname(WPN_NAMES[wpn_id], font, 18u);
                wname.setFillColor(WPN_COLORS[wpn_id]);
                sf::FloatRect wb = wname.getLocalBounds();
                wname.setPosition(px + pw/2.f - wb.width/2.f, py + 130.f); win.draw(wname);

                // Stats
                char stats[64];
                snprintf(stats, sizeof(stats), "Slots: %d   Damage: %d",
                         WPN_SLOTS[wpn_id], WPN_DAMAGE[wpn_id]);
                sf::Text st(stats, font, 14u);
                st.setFillColor({180,180,180});
                sf::FloatRect stb = st.getLocalBounds();
                st.setPosition(px + pw/2.f - stb.width/2.f, py + 158.f); win.draw(st);
            }

            // For player label
            char plbl[64];
            snprintf(plbl, sizeof(plbl), "Player %d — Pick up?", for_player + 1);
            sf::Text plblt(plbl, font, 14u);
            plblt.setFillColor({200,200,255});
            sf::FloatRect pf = plblt.getLocalBounds();
            plblt.setPosition(px + pw/2.f - pf.width/2.f, py + 184.f); win.draw(plblt);

            // Buttons
            bool pick_hov = draw_button(win, font, "Pick Up",
                                        px + 40.f, py + ph - 55.f, 140.f, 38.f, mouse,
                                        true, {30, 100, 30, 220});
            bool leave_hov = draw_button(win, font, "Leave",
                                         px + pw - 180.f, py + ph - 55.f, 140.f, 38.f, mouse,
                                         true, {100, 30, 30, 220});

            if (left_clicked) {
                if (pick_hov) {
                    // Give to player
                    pthread_mutex_lock(&gs->memory_lock);
                    if (for_player >= 0 && for_player < gs->num_players)
                        pick_up_weapon(gs->players[for_player], wpn_id);
                    gs->weapon_drop.pending  = false;
                    gs->weapon_drop.decision = 1;
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "Player %d picked up %s!", for_player+1,
                             wpn_id >= 0 && wpn_id < WPN_COUNT ? WPN_NAMES[wpn_id] : "?");
                    pthread_mutex_unlock(&gs->memory_lock);
                    popup_showing = false;
                } else if (leave_hov) {
                    // Give to random alive enemy
                    pthread_mutex_lock(&gs->memory_lock);
                    for (int i = 0; i < gs->num_enemies; i++) {
                        if (gs->enemies[i].is_alive) {
                            pick_up_weapon(gs->enemies[i], wpn_id);
                            snprintf(gs->action_log, MAX_ACTION_LOG,
                                     "Enemy %d picked up %s!", i+1,
                                     wpn_id >= 0 && wpn_id < WPN_COUNT ? WPN_NAMES[wpn_id] : "?");
                            break;
                        }
                    }
                    gs->weapon_drop.pending  = false;
                    gs->weapon_drop.decision = 2;
                    pthread_mutex_unlock(&gs->memory_lock);
                    popup_showing = false;
                }
            }
        }

        // ── PLAYER ACTION PANEL ───────────────────────────────────────────
        int active = gs->active_turn;
        bool is_player_turn = (active >= 0 && active < MAX_PLAYERS
                               && !popup_showing);
        if (is_player_turn) {
            Entity& ap = gs->players[active];
            // Check ultimate eligibility
            ui_can_ultimate = entity_has_weapon(ap, WPN_SOLAR_CORE)
                           && entity_has_weapon(ap, WPN_LUNAR_BLADE);

            // Panel background
            float panel_x = 10.f, panel_y = WIN_H - 200.f;
            float panel_w = 490.f, panel_h = 160.f;
            sf::RectangleShape panel({panel_w, panel_h});
            panel.setFillColor({10,8,20,220});
            panel.setOutlineThickness(2.f);
            panel.setOutlineColor({80,80,160});
            panel.setPosition(panel_x, panel_y); win.draw(panel);

            char ptitle[32];
            snprintf(ptitle, sizeof(ptitle), "Player %d — Choose Action:", active+1);
            sf::Text pt(ptitle, font, 13u);
            pt.setFillColor({180,180,255});
            pt.setPosition(panel_x + 10.f, panel_y + 8.f); win.draw(pt);

            float bw = 108.f, bh = 34.f;
            float bx = panel_x + 10.f, by = panel_y + 32.f;
            float gap = 4.f;

            // Row 1: Strike, Exhaust, Heal, Skip
            if (draw_button(win, font, "Strike",      bx,                 by, bw, bh, mouse) && left_clicked && ui_mode==0)
                ui_mode = 1;
            if (draw_button(win, font, "Exhaust",     bx + (bw+gap),      by, bw, bh, mouse) && left_clicked && ui_mode==0)
                ui_mode = 2;
            if (draw_button(win, font, "Heal",        bx + 2*(bw+gap),    by, bw, bh, mouse) && left_clicked && ui_mode==0) {
                gs->action_actor   = active;
                gs->pending_action = ACTION_HEAL;
                ui_mode = 0;
            }
            if (draw_button(win, font, "Skip",        bx + 3*(bw+gap),    by, bw, bh, mouse) && left_clicked && ui_mode==0) {
                gs->action_actor   = active;
                gs->pending_action = ACTION_SKIP;
                ui_mode = 0;
            }

            // Row 2: Use Weapon, Swap In, Ultimate, Quit
            float by2 = by + bh + gap;
            if (draw_button(win, font, "Use Weapon",  bx,                 by2, bw, bh, mouse) && left_clicked && ui_mode==0)
                ui_mode = 3;
            if (draw_button(win, font, "Swap In",     bx + (bw+gap),      by2, bw, bh, mouse) && left_clicked && ui_mode==0)
                ui_mode = 4;
            if (draw_button(win, font, "ULTIMATE",    bx + 2*(bw+gap),    by2, bw, bh, mouse,
                            ui_can_ultimate, {60,30,100,220}) && left_clicked && ui_mode==0 && ui_can_ultimate) {
                gs->action_actor   = active;
                gs->pending_action = ACTION_ULTIMATE;
                ui_mode = 0;
            }
            if (draw_button(win, font, "Quit",        bx + 3*(bw+gap),    by2, bw, bh, mouse,
                            true, {80,20,20,200}) && left_clicked && ui_mode==0) {
                gs->pending_action = ACTION_QUIT;
                gs->action_actor   = active;
                kill(gs->arbiter_pid, SIGTERM);
                running = false;
            }

            // Mode hints
            const char* hints[] = {"", "Click an enemy to Strike",
                                   "Click an enemy to Exhaust",
                                   "Click a weapon below to Use",
                                   "Click a long-term slot to Swap In",
                                   "Click an enemy to attack with weapon"};
            if (ui_mode > 0 && ui_mode < 6) {
                sf::Text hint(hints[ui_mode], font, 13u);
                hint.setFillColor({255,200,80});
                hint.setPosition(panel_x + 10.f, panel_y + panel_h - 20.f);
                win.draw(hint);
            }

            // ── Inventory panel ───────────────────────────────────────────
            float inv_x = panel_x + panel_w + 10.f;
            float inv_y = WIN_H - 200.f;
            float slot_s = 36.f, slot_gap = 3.f;
            float inv_cols = 10.f;
            float inv_w = inv_cols * (slot_s + slot_gap) + 10.f;
            float inv_h = 2 * (slot_s + slot_gap) + 30.f;

            sf::RectangleShape inv_panel({inv_w, inv_h});
            inv_panel.setFillColor({10,8,20,220});
            inv_panel.setOutlineThickness(2.f);
            inv_panel.setOutlineColor({80,80,160});
            inv_panel.setPosition(inv_x, inv_y); win.draw(inv_panel);

            sf::Text inv_title("Inventory:", font, 12u);
            inv_title.setFillColor({180,180,255});
            inv_title.setPosition(inv_x + 5.f, inv_y + 5.f); win.draw(inv_title);

            // Show 20 slots in 2 rows of 10
            for (int s = 0; s < INVENTORY_SIZE; s++) {
                int col = s % 10, row = s / 10;
                float sx = inv_x + 5.f + col * (slot_s + slot_gap);
                float sy = inv_y + 22.f + row * (slot_s + slot_gap);
                int   wid = ap.inventory[s];
                bool  highlight = false;

                // Highlight weapon slots in USE_WEAPON mode
                if (ui_mode == 3 && wid != WPN_NONE) {
                    sf::FloatRect hitbox(sx, sy, slot_s, slot_s);
                    if (hitbox.contains((float)mouse.x, (float)mouse.y)) {
                        highlight = true;
                        if (left_clicked) {
                            ui_weapon_sel = wid;
                            ui_mode = 5; // now pick target
                        }
                    }
                }
                draw_weapon_icon(win, font, wid, sx, sy, slot_s, highlight);
            }

            // Long-term storage (shown in SWAP_IN mode)
            if (ui_mode == 4) {
                float lt_x = inv_x, lt_y = inv_y + inv_h + 5.f;
                float lt_w = inv_w, lt_h = slot_s + 28.f;
                sf::RectangleShape lt_panel({lt_w, lt_h});
                lt_panel.setFillColor({15,10,25,220});
                lt_panel.setOutlineThickness(2.f);
                lt_panel.setOutlineColor({100,60,160});
                lt_panel.setPosition(lt_x, lt_y); win.draw(lt_panel);

                sf::Text lt_title("Long-Term Storage:", font, 11u);
                lt_title.setFillColor({180,120,255});
                lt_title.setPosition(lt_x + 5.f, lt_y + 4.f); win.draw(lt_title);

                for (int k = 0; k < ap.lt_count && k < 10; k++) {
                    float slx = lt_x + 5.f + k * (slot_s + slot_gap);
                    float sly = lt_y + 20.f;
                    int   wid = ap.long_term[k];
                    bool  hl  = false;
                    sf::FloatRect hitbox(slx, sly, slot_s, slot_s);
                    if (hitbox.contains((float)mouse.x, (float)mouse.y)) {
                        hl = true;
                        if (left_clicked) {
                            gs->action_actor     = active;
                            gs->action_weapon_id = k;
                            gs->pending_action   = ACTION_SWAP_IN;
                            ui_mode = 0;
                        }
                    }
                    draw_weapon_icon(win, font, wid, slx, sly, slot_s, hl);
                }
            }
        }

        win.display();
    }

    // ── Game over screen ──────────────────────────────────────────────────────
    if (win.isOpen()) {
        bool won = gs->player_won;
        for (int frame = 0; frame < 240 && win.isOpen(); frame++) {
            sf::Event ev;
            while (win.pollEvent(ev))
                if (ev.type == sf::Event::Closed) win.close();

            float ft = (float)frame / 60.f;
            win.clear({8, 5, 15});
            sf::RectangleShape ov({(float)WIN_W, (float)WIN_H});
            ov.setFillColor({0,0,0,(sf::Uint8)std::min(200,(int)(ft*80))});
            win.draw(ov);

            float sc = ft < 0.5f ? ft / 0.5f : 1.f;
            sf::Text mt(won ? "VICTORY!" : "DEFEAT", font, (unsigned)(80*sc));
            mt.setFillColor(won ? sf::Color{60,255,120} : sf::Color{255,60,60});
            sf::FloatRect mb = mt.getLocalBounds();
            mt.setOrigin(mb.width/2.f, mb.height/2.f);
            mt.setPosition(WIN_W/2.f, WIN_H/2.f - 50.f); win.draw(mt);

            if (ft > 0.65f) {
                char sub[64];
                snprintf(sub, sizeof(sub), "Enemies slain: %d", gs->enemies_killed);
                sf::Text st(sub, font, 26);
                st.setFillColor({200,200,200,(sf::Uint8)std::min(255,(int)((ft-0.65f)*3.f*255))});
                sf::FloatRect sb = st.getLocalBounds();
                st.setOrigin(sb.width/2.f, 0.f);
                st.setPosition(WIN_W/2.f, WIN_H/2.f + 45.f); win.draw(st);
            }
            win.display();
        }
        win.close();
    }
    return nullptr;
}

// ─── main ─────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGTERM, signal_handler);
    signal(SIGCHLD, signal_handler);
    signal(SIGALRM, signal_handler);

    init_shared_memory();
    init_game();

    pthread_t rtid, dtid;
    pthread_create(&rtid, nullptr, render_thread_func,    nullptr);
    pthread_create(&dtid, nullptr, deadlock_monitor_func, nullptr);

    cout << "Arbiter: waiting for HIP and ASP..." << endl;
    sleep(2);

    while (!gs->is_game_over) {
        // Block while weapon drop popup is showing
        while (gs->weapon_drop.pending && !gs->is_game_over)
            usleep(50000);

        stamina_tick();
        usleep(100000);

        int actor = find_next_actor();
        if (actor == -1) continue;
        gs->active_turn = actor;

        int timeout = 0;
        while (gs->pending_action == ACTION_NONE && !gs->is_game_over) {
            usleep(50000);
            timeout++;
            // NPC timeout: 3 seconds (60 * 50ms)
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

    cout << (gs->player_won ? "Players WIN!" : "GAME OVER") << endl;
    pthread_join(rtid,  nullptr);
    pthread_join(dtid, nullptr);
    cleanup();
    return 0;
}