#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <SFML/Graphics.hpp>
#include "../common.h"
#include "../shared/shared_utils.h"
#include "../game_logic.h"

using namespace std;

//Window / layout
static const int   WIN_W    = 1500;
static const int   WIN_H    = 720;
static const float GROUND_Y = WIN_H * 0.65f;
static const float P_X1     = 80.f;
static const float P_X2     = 440.f;
static const float E_X1     = 760.f;
static const float E_X2     = 1220.f;

// Sprite sheet constants 
static const int CELL_W      = 128;
static const int CELL_H      = 128;

// Globals
static GameState* gs      = nullptr;
static bool       running = true;

struct AttackTrigger {
    pthread_mutex_t lock;
    bool fired;
    int  attacker;
    int  target;
};
static AttackTrigger atk_trig = {{}, false, 0, 0};
static volatile float ultimate_flash_timer = 0.f;

// Forward declarations 
static void  init_game();
static void  execute_action();
static void* render_thread_func(void*);
static void* deadlock_monitor_func(void*);
static void  signal_handler(int);
static void  cleanup();
static void  trigger_stun(int entity_idx, bool is_player);

// Thin wrappers around game_logic functions that handle mutex locking
static void send_ui_event_locked(int action, int target, int weapon_id, int player_idx) {
    pthread_mutex_lock(&gs->memory_lock);
    send_ui_event(gs, action, target, weapon_id, player_idx);
    pthread_mutex_unlock(&gs->memory_lock);
}

static bool pop_player_action_locked(int player_idx, PlayerInputEvent* out) {
    pthread_mutex_lock(&gs->memory_lock);
    bool has = pop_player_action(gs, player_idx, out);
    pthread_mutex_unlock(&gs->memory_lock);
    return has;
}

// Attack-trigger helper (render thread reads this to animate lunges)
static void fire_attack_trigger(int attacker, int target) {
    pthread_mutex_lock(&atk_trig.lock);
    atk_trig.attacker = attacker;
    atk_trig.target   = target;
    atk_trig.fired    = true;
    pthread_mutex_unlock(&atk_trig.lock);
}

// Lunge animation frame from normalised progress
static int compute_lunge_frame(float timer, float duration) {
    float tp = timer / duration;
    if      (tp < 0.2f)  return 1;
    else if (tp < 0.4f)  return 2;
    else if (tp < 0.62f) return 3;
    else if (tp < 0.8f)  return 4;
    return 0;
}

// Inventory helpers now live in game_logic.cpp

// Process signals
static void signal_handler(int sig) {
    if (sig == SIGTERM) { gs->is_game_over = true; running = false; }
    if (sig == SIGCHLD) { int s; waitpid(-1, &s, WNOHANG); }
    if (sig == SIGALRM) {
        if (gs->asp_pid > 0) kill(gs->asp_pid, SIGCONT);
        gs->ultimate_pause = false;
        snprintf(gs->action_log, MAX_ACTION_LOG, "Ultimate ended — enemies resume!");
    }
}

static void trigger_stun(int entity_idx, bool is_player) {
    if (is_player && (entity_idx < 0 || entity_idx >= MAX_PLAYERS))  return;
    if (!is_player && (entity_idx < 0 || entity_idx >= MAX_ENEMIES)) return;

    pthread_mutex_lock(&gs->memory_lock);
    if (is_player && entity_idx < gs->num_players && gs->players[entity_idx].is_alive)
        gs->players[entity_idx].is_stunned = true;
    else if (!is_player && entity_idx < gs->num_enemies && gs->enemies[entity_idx].is_alive)
        gs->enemies[entity_idx].is_stunned = true;

    // Grab the Process ID instead of the local Thread ID
    pid_t target_pid = is_player ? gs->hip_pid : gs->asp_pid;
    pthread_mutex_unlock(&gs->memory_lock);

    // Safely send the signal across processes using POSIX kill()
    if (target_pid > 0) {
        if (kill(target_pid, SIGUSR1) == -1)
            perror("Arbiter: kill SIGUSR1 for stun");
    }

    // Detached helper clears the visual flag after 3 s.
    struct ClearArg { int idx; bool ip; };
    ClearArg* ca = new ClearArg{entity_idx, is_player};

    pthread_t ct;
    int rc = pthread_create(&ct, nullptr, [](void* p) -> void* {
        ClearArg* a = (ClearArg*)p;
        sleep(3);
        pthread_mutex_lock(&gs->memory_lock);
        if (a->ip && a->idx < MAX_PLAYERS)
            gs->players[a->idx].is_stunned = false;
        else if (!a->ip && a->idx < MAX_ENEMIES)
            gs->enemies[a->idx].is_stunned = false;
        pthread_mutex_unlock(&gs->memory_lock);
        delete a;
        return nullptr;
    }, ca);
    if (rc != 0) {
        cerr << "Arbiter: failed to create stun-clear thread: " << strerror(rc) << endl;
        delete ca;
        return;
    }
    pthread_detach(ct);
}

// send_ui_event and pop_player_action now live in game_logic.cpp
// (wrapped by send_ui_event_locked / pop_player_action_locked above)

// Shared memory + game init
static void init_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd == -1) { perror("shm_open"); exit(1); }
    if (ftruncate(fd, sizeof(GameState)) == -1) {
        perror("Arbiter: ftruncate"); close(fd); exit(1);
    }
    gs = (GameState*)mmap(nullptr, sizeof(GameState),
                          PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs == MAP_FAILED) { perror("mmap"); exit(1); }
    if (close(fd) == -1) perror("Arbiter: close shm fd");
    memset(gs, 0, sizeof(GameState));

    pthread_mutexattr_t attr;
    int rc;
    if ((rc = pthread_mutexattr_init(&attr)) != 0) {
        cerr << "Arbiter: pthread_mutexattr_init failed: " << strerror(rc) << endl; exit(1);
    }
    if ((rc = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) != 0) {
        cerr << "Arbiter: pthread_mutexattr_setpshared failed: " << strerror(rc) << endl; exit(1);
    }
    if ((rc = pthread_mutex_init(&gs->memory_lock, &attr)) != 0) {
        cerr << "Arbiter: pthread_mutex_init failed: " << strerror(rc) << endl; exit(1);
    }
    pthread_mutexattr_destroy(&attr);
    // Init atk_trig mutex (process-local, render thread + game loop)
    if ((rc = pthread_mutex_init(&atk_trig.lock, nullptr)) != 0) {
        cerr << "Arbiter: atk_trig mutex init failed: " << strerror(rc) << endl; exit(1);
    }
}

static void init_game() {
    srand(ROLL_SEED);
    cout << "=== CHRONO RIFT ===" << endl;
    cout << "Enter number of players (1-" << MAX_PLAYERS << "): ";
    cin >> gs->num_players;
    if (cin.fail()) {
        cerr << "Arbiter: invalid input for player count, defaulting to 1" << endl;
        cin.clear();
        cin.ignore(256, '\n');
        gs->num_players = 1;
    } else if (gs->num_players < 1 || gs->num_players > MAX_PLAYERS) {
        gs->num_players = 1;
    }

    for (int i = 0; i < gs->num_players; i++) {
        Entity& p     = gs->players[i];
        p.is_alive    = true;
        p.is_stunned  = false;
        p.hp          = ROLL_FULL + (rand() % 901 + 100);
        p.max_hp      = p.hp;
        p.max_stamina = 100;
        p.stamina     = 0;
        p.damage      = LAST_DIGIT + 10;
        p.speed       = 100 / gs->num_players;
        p.lt_count    = 0;
        p.waiting_for = ART_NONE;
        memset(p.inventory, WPN_NONE, sizeof(p.inventory));
        memset(p.long_term,  WPN_NONE, sizeof(p.long_term));

        for (int s = 0; s < WPN_SLOTS[WPN_IRON_HALBERD]; s++)
            p.inventory[s] = WPN_IRON_HALBERD;
        for (int s = 0; s < WPN_SLOTS[WPN_VENOM_DAGGER]; s++)
            p.inventory[WPN_SLOTS[WPN_IRON_HALBERD] + s] = WPN_VENOM_DAGGER;
    }

    gs->num_enemies = rand() % 8 + 2;
    for (int i = 0; i < gs->num_enemies; i++)
        init_enemy(gs->enemies[i]);

    gs->is_game_over  = false;
    gs->player_won    = false;
    gs->enemies_killed = 0;
    gs->active_turn   = -1;
    gs->pending_action = ACTION_NONE;
    gs->ultimate_pause = false;

    gs->solar_core_exists      = true;  gs->solar_core_holder  = -1;
    gs->lunar_blade_exists     = true;  gs->lunar_blade_holder = -1;
    gs->eclipse_relic_exists   = false; gs->eclipse_relic_holder = -1;
    gs->eclipse_relic_announced = false;

    gs->weapon_drop.pending    = false;
    gs->weapon_drop.weapon_id  = WPN_NONE;
    gs->weapon_drop.for_player = -1;
    gs->weapon_drop.decision   = 0;
        gs->ui_events.count = 0;

    // Zero thread-tid arrays; HIP/ASP populate these at thread startup.
    memset(gs->hip_thread_tids, 0, sizeof(gs->hip_thread_tids));
    memset(gs->asp_thread_tids, 0, sizeof(gs->asp_thread_tids));

    // Initialise per-player action buffers.
    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs->player_buffers[i].head  = 0;
        gs->player_buffers[i].tail  = 0;
        gs->player_buffers[i].count = 0;
    }

    gs->arbiter_pid = getpid();
    snprintf(gs->action_log, MAX_ACTION_LOG,
             "Chrono Rift! Players:%d  Enemies:%d",
             gs->num_players, gs->num_enemies);
    cout << "Players:" << gs->num_players
         << "  Enemies:" << gs->num_enemies << endl;
}

// respawn_enemies, stamina_tick, find_next_actor now live in game_logic.cpp

// Execute action 
static void execute_action() {
    if (gs->pending_action == ACTION_NONE) return;
    pthread_mutex_lock(&gs->memory_lock);

    int actor  = gs->action_actor;
    int target = gs->action_target;
    int action = gs->pending_action;
    int wpn    = gs->action_weapon_id;

    if (actor < 0 || actor >= MAX_PLAYERS + MAX_ENEMIES) {
        gs->pending_action = ACTION_NONE;
        pthread_mutex_unlock(&gs->memory_lock);
        return;
    }
    bool ap    = (actor  < MAX_PLAYERS);
    auto is_valid_target = [&](int t) {
        if (t < 0) return false;
        if (t < MAX_PLAYERS) return t < gs->num_players;
        int ei = t - MAX_PLAYERS;
        return ei >= 0 && ei < gs->num_enemies;
    };
    bool tp = (target >= 0 && target < MAX_PLAYERS);

    Entity& ae = ap ? gs->players[actor] : gs->enemies[actor - MAX_PLAYERS];
    Entity* te_ptr = nullptr;
    if ((action == ACTION_STRIKE || action == ACTION_EXHAUST || action == ACTION_USE_WEAPON || action == ACTION_HEAL)
        && is_valid_target(target)) {
        te_ptr = tp ? &gs->players[target] : &gs->enemies[target - MAX_PLAYERS];
    }

    if (action == ACTION_SKIP) {
        ae.stamina = ae.max_stamina / 2; // Skip depletes to 50%
    } else {
        ae.stamina = 0; // All other actions deplete to 0 immediately
    }

    switch (action) {
        case ACTION_STRIKE:
            if (te_ptr) {
                te_ptr->hp -= ae.damage; 
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d STRIKES entity %d for %d dmg", actor, target, ae.damage);
                fire_attack_trigger(actor, target);
            } else {
                snprintf(gs->action_log, MAX_ACTION_LOG, "Entity %d missed their strike!", actor);
            }
            break;

        case ACTION_EXHAUST:
            if (te_ptr) {
                te_ptr->stamina -= ae.damage;
                if (te_ptr->stamina < 0) te_ptr->stamina = 0;
                
                int tidx = tp ? target : (target - MAX_PLAYERS);
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d EXHAUSTS entity %d — STUNNED!", actor, target);
                
                // Unlock memory BEFORE triggering stun to prevent deadlocks
                pthread_mutex_unlock(&gs->memory_lock);
                trigger_stun(tidx, tp);
                pthread_mutex_lock(&gs->memory_lock);
            }
            break;

        case ACTION_USE_WEAPON:
            if (wpn >= 0 && wpn < WPN_COUNT && te_ptr) {
                te_ptr->hp -= WPN_DAMAGE[wpn]; 
                snprintf(gs->action_log, MAX_ACTION_LOG,
                         "Entity %d uses %s → %d dmg!", actor, WPN_NAMES[wpn], WPN_DAMAGE[wpn]);
                fire_attack_trigger(actor, target);
            }
            break;

        case ACTION_ULTIMATE:
            gs->ultimate_pause = true; ultimate_flash_timer = 1.5f;
            if (gs->asp_pid > 0 && kill(gs->asp_pid, SIGSTOP) == -1)
                perror("Arbiter: kill SIGSTOP for ultimate");
            alarm(10);
            for (int i = 0; i < gs->num_enemies; i++)
                if (gs->enemies[i].is_alive) gs->enemies[i].hp -= (ae.damage * 3);
            snprintf(gs->action_log, MAX_ACTION_LOG,
                     "ULTIMATE ACTIVATED — Enemies frozen 10s!");
            break;

        case ACTION_HEAL:
            if (te_ptr && tp) { // 'tp' ensures the target is actually a Player, not an Enemy!
                te_ptr->hp += te_ptr->max_hp / 10;
                if (te_ptr->hp > te_ptr->max_hp) te_ptr->hp = te_ptr->max_hp;
                snprintf(gs->action_log, MAX_ACTION_LOG,
                        "Entity %d HEALS Player %d for %d HP", actor, target + 1, te_ptr->max_hp / 10);
            } else {
                snprintf(gs->action_log, MAX_ACTION_LOG, "Entity %d missed their heal!", actor);
            }
            break;

        case ACTION_SKIP:
            snprintf(gs->action_log, MAX_ACTION_LOG, "Entity %d SKIPS", actor);
            break;

        case ACTION_SWAP_IN:
            if (wpn >= 0 && wpn < ae.lt_count) {
                int wid = ae.long_term[wpn];
                if (!pick_up_weapon(gs, ae, wid)) {
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "Entity %d failed to swap in %s — no room!", actor, WPN_NAMES[wid]);
                } else {
                    for (int k = wpn; k < ae.lt_count - 1; k++) ae.long_term[k] = ae.long_term[k+1];
                    ae.lt_count--; 
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "Entity %d swapped in %s", actor, WPN_NAMES[wid]);
                }
            }
            break;
    }

    // Enemy death + weapon drop
    if (te_ptr && !tp && te_ptr->hp <= 0 && te_ptr->is_alive) {
        te_ptr->is_alive = false; gs->enemies_killed++;
        int eidx = target - MAX_PLAYERS;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d defeated! Kills: %d/10", eidx + 1, gs->enemies_killed);

        if (gs->enemies_killed == 3 && !gs->eclipse_relic_exists) {
            gs->eclipse_relic_exists = true; gs->eclipse_relic_holder = -1;
        }
        bool enemy_has_wpn = false;
        for (int s = 0; s < INVENTORY_SIZE; s++)
            if (te_ptr->inventory[s] != WPN_NONE) { enemy_has_wpn = true; break; }

        if (!enemy_has_wpn && (rand() % 100) < 50) {
            int drop_wpn = (gs->enemies_killed == 3 && gs->eclipse_relic_exists
                            && !gs->eclipse_relic_announced)
                           ? (gs->eclipse_relic_announced = true, WPN_ECLIPSE_RELIC)
                           : rand() % WPN_COUNT;
            int for_player = ap ? actor : 0;
            if (for_player >= gs->num_players) for_player = 0;
            gs->weapon_drop.pending    = true;
            gs->weapon_drop.weapon_id  = drop_wpn;
            gs->weapon_drop.for_player = for_player;
            gs->weapon_drop.decision   = 0;
        }
    }

    // Player death
    if (te_ptr && tp && te_ptr->hp <= 0 && te_ptr->is_alive) {
        te_ptr->is_alive = false;
        snprintf(gs->action_log, MAX_ACTION_LOG, "Player %d has fallen!", target + 1);
    }

    gs->pending_action = ACTION_NONE;
    gs->active_turn    = -1;
    pthread_mutex_unlock(&gs->memory_lock);
}

// check_win_loss now lives in game_logic.cpp

// Deadlock monitor
static void* deadlock_monitor_func(void*) {
    while (!gs->is_game_over) {
        sleep(2);
        pthread_mutex_lock(&gs->memory_lock);
        int sc = gs->solar_core_holder, lb = gs->lunar_blade_holder;
        if (sc != -1 && lb != -1 && sc != lb) {
            auto get_wf = [&](int idx) {
                return idx < MAX_PLAYERS ? gs->players[idx].waiting_for
                       : (idx - MAX_PLAYERS < gs->num_enemies
                          ? gs->enemies[idx - MAX_PLAYERS].waiting_for
                          : ART_NONE);
            };
            if (get_wf(sc) == ART_LUNAR_BLADE && get_wf(lb) == ART_SOLAR_CORE) {
                int rel = (sc > lb) ? sc : lb;
                if (rel == sc) {
                    gs->solar_core_holder = -1;
                    (rel < MAX_PLAYERS ? gs->players[rel].waiting_for
                                       : gs->enemies[rel - MAX_PLAYERS].waiting_for) = ART_NONE;
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "[Arbiter] DEADLOCK! Freed Solar Core from entity %d", rel);
                } else {
                    gs->lunar_blade_holder = -1;
                    (rel < MAX_PLAYERS ? gs->players[rel].waiting_for
                                       : gs->enemies[rel - MAX_PLAYERS].waiting_for) = ART_NONE;
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "[Arbiter] DEADLOCK! Freed Lunar Blade from entity %d", rel);
                }
            }
        }
        pthread_mutex_unlock(&gs->memory_lock);
    }
    return nullptr;
}

static void cleanup() {
    // Notify HIP and ASP to exit cleanly before destroying shared memory.
    if (gs->hip_pid > 0 && kill(gs->hip_pid, SIGTERM) == -1)
        perror("Arbiter: kill HIP");
    if (gs->asp_pid > 0 && kill(gs->asp_pid, SIGTERM) == -1)
        perror("Arbiter: kill ASP");
    // Give them 1 second to unmap and exit before we destroy the segment.
    usleep(1000000);
    if (pthread_mutex_destroy(&gs->memory_lock) != 0)
        perror("Arbiter: pthread_mutex_destroy memory_lock");
    if (pthread_mutex_destroy(&atk_trig.lock) != 0)
        perror("Arbiter: pthread_mutex_destroy atk_trig");
    if (munmap(gs, sizeof(GameState)) == -1)
        perror("Arbiter: munmap");
    if (shm_unlink(SHARED_MEM_NAME) == -1)
        perror("Arbiter: shm_unlink");
    cout << "Arbiter: cleaned up." << endl;
}

static sf::Vector2f player_foot(int i, int n) {
    float x = (n <= 1) ? (P_X1+P_X2)/2.f : P_X1+(P_X2-P_X1)*((float)i/(n-1));
    return {x, GROUND_Y - (float)i * 14.f};
}
static sf::Vector2f enemy_foot(int i, int n) {
    float x = (n <= 1) ? (E_X1+E_X2)/2.f : 600+(E_X2-E_X1)*(1.5f*i/n);
    return {x, GROUND_Y - (float)i * 4.f};
}

static void draw_bars(sf::RenderWindow& win, sf::Font& font,
                      float cx, float foot_y, int hp, int mhp, int sp, int msp,
                      bool dead, const char* name, bool compact = false)
{
    if (dead) return;
    float bw = compact ? 44.f : 80.f, bh = 8.f;
    float bx = cx - bw/2.f, by = foot_y - CELL_H - 24.f;
    sf::RectangleShape bg({bw+4.f, bh*2.f+8.f});
    bg.setFillColor({0,0,0,180}); bg.setPosition(bx-2.f, by-2.f); win.draw(bg);
    float hr = (float)hp/mhp; if (hr<0) hr=0; if (hr>1) hr=1;
    sf::Color hc = hr>0.5f?sf::Color{50,220,80}:hr>0.25f?sf::Color{230,200,0}:sf::Color{220,50,50};
    sf::RectangleShape hbar({bw*hr, bh}); hbar.setFillColor(hc); hbar.setPosition(bx, by); win.draw(hbar);
    float sr = (float)sp/msp; if (sr<0) sr=0; if (sr>1) sr=1;
    sf::RectangleShape sbar({bw*sr, bh-2.f}); sbar.setFillColor({255,200,0});
    sbar.setPosition(bx, by+bh+2.f); win.draw(sbar);
    sf::Text t(name, font, compact?10u:11u); t.setFillColor({220,220,255});
    sf::FloatRect tb=t.getLocalBounds(); t.setPosition(cx-tb.width/2.f, by-16.f); win.draw(t);
    char vals[32]; snprintf(vals,sizeof(vals),"%d/%d  %d/%d",hp,mhp,sp,msp);
    sf::Text v(vals, font, compact?8u:9u); v.setFillColor({200,200,200});
    sf::FloatRect vb=v.getLocalBounds(); v.setPosition(cx-vb.width/2.f, by+bh+12.f); win.draw(v);
}

static void draw_slash(sf::RenderWindow& win, float sx, float sy, float p) {
    struct L { float ax,ay,len; };
    L lines[] = {{-0.5f,-0.3f,80},{-0.2f,-0.5f,100},{0.1f,-0.4f,110},{0.4f,-0.2f,85},{0.6f,0.1f,60}};
    for (auto& l : lines) {
        float len=l.len*p; sf::Uint8 alpha=(sf::Uint8)(255*(1.f-p));
        sf::Vertex vx[]={sf::Vertex({sx,sy},{255,255,100,alpha}),
                         sf::Vertex({sx+cosf(l.ax)*len,sy+sinf(l.ay)*len},{255,255,255,0})};
        win.draw(vx,2,sf::Lines);
    }
    float r=30.f*p; sf::CircleShape c(r,12);
    c.setFillColor({255,220,100,(sf::Uint8)(120*(1.f-p))});
    c.setOrigin(r,r); c.setPosition(sx,sy); win.draw(c);
}

static const sf::Color WPN_COLORS[WPN_COUNT] = {
    {255,200,50},{180,180,255},{140,80,40},{80,200,80},
    {100,100,255},{60,60,60},{80,200,220},{180,120,60},{200,50,200},
};

static void draw_weapon_icon(sf::RenderWindow& win, sf::Font& font,
                             int wpn_id, float x, float y,
                             float size=40.f, bool highlighted=false)
{
    if (wpn_id < 0 || wpn_id >= WPN_COUNT) {
        sf::RectangleShape box({size,size}); box.setFillColor({40,40,40,200});
        box.setOutlineThickness(1.f); box.setOutlineColor({80,80,80});
        box.setPosition(x,y); win.draw(box); return;
    }
    sf::Color col=WPN_COLORS[wpn_id];
    sf::RectangleShape box({size,size});
    box.setFillColor({(sf::Uint8)(col.r/2),(sf::Uint8)(col.g/2),(sf::Uint8)(col.b/2),220});
    box.setOutlineThickness(highlighted?3.f:1.5f);
    box.setOutlineColor(highlighted?sf::Color{255,255,0}:col);
    box.setPosition(x,y); win.draw(box);
    char abbr[3]={WPN_NAMES[wpn_id][0],WPN_NAMES[wpn_id][1],'\0'};
    sf::Text lbl(abbr,font,(unsigned)(size*0.4f)); lbl.setFillColor(col);
    sf::FloatRect lb=lbl.getLocalBounds();
    lbl.setPosition(x+size/2.f-lb.width/2.f,y+size/2.f-lb.height/2.f-2.f); win.draw(lbl);
}

static bool draw_button(sf::RenderWindow& win, sf::Font& font,
                        const char* label, float x, float y, float w, float h,
                        sf::Vector2i mouse, bool active=true,
                        sf::Color base={50,50,80,220})
{
    sf::FloatRect rect(x,y,w,h);
    bool hovered = active && rect.contains((float)mouse.x,(float)mouse.y);
    sf::RectangleShape btn({w,h}); btn.setPosition(x,y);
    btn.setFillColor(hovered?sf::Color{80,80,140,240}:base);
    btn.setOutlineThickness(hovered?2.5f:1.f);
    btn.setOutlineColor(active?sf::Color{120,120,220}:sf::Color{60,60,60});
    win.draw(btn);
    sf::Text t(label,font,13u);
    t.setFillColor(active?sf::Color{220,220,255}:sf::Color{80,80,80});
    sf::FloatRect tb=t.getLocalBounds();
    t.setPosition(x+w/2.f-tb.width/2.f,y+h/2.f-tb.height/2.f-2.f); win.draw(t);
    return hovered;
}

static void* render_thread_func(void*) {
    const char* disp = getenv("DISPLAY");
    if (!disp || !disp[0]) {
        while (!gs->is_game_over) {
            cout << "\r[Game] Kills:" << gs->enemies_killed
                 << " Turn:" << gs->active_turn << " " << gs->action_log
                 << "    " << flush;
            usleep(500000);
        }
        return nullptr;
    }

    sf::RenderWindow win(sf::VideoMode(WIN_W, WIN_H), "Chrono Rift — Arena",
                         sf::Style::Titlebar | sf::Style::Close);
    win.setFramerateLimit(60);

    sf::Font font;
    if (!font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"))
        if (!font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"))
            cerr << "Arbiter: failed to load any font — text will not render" << endl;

    sf::Texture texP, texS, texR, texB, bg_tex;
    bool hasP  = texP.loadFromFile("Sprites/players.png");
    bool hasS  = texS.loadFromFile("Sprites/enemy_silver_flip.png");
    bool hasR  = texR.loadFromFile("Sprites/enemy_red_flip.png");
    bool hasB  = texB.loadFromFile("Sprites/enemy_blue_flip.png");
    bool hasBg = bg_tex.loadFromFile("Sprites/backgrounds_2.jpeg");
    texP.setSmooth(false); texS.setSmooth(false);
    texR.setSmooth(false); texB.setSmooth(false);

    float flash[MAX_PLAYERS + MAX_ENEMIES] = {};
    float attack_fx_timer = 0.f;
    int   attack_fx_target = -1; (void)attack_fx_target;
    static const float ATTACK_FX_DUR = 1.f;
    int   lng_actor=-1, lng_target=-1;
    float lng_timer=0.f; bool lng_hit=false;
    static const float LNG_DUR = 0.75f;

    // GUI state
    int  ui_mode       = 0;   // 0=normal,1=strike-tgt,2=exhaust-tgt,
                               // 3=weapon-sel,4=lt-sel,5=weapon+tgt
    int  ui_weapon_sel = -1;
    bool ui_can_ultimate = false;

    sf::Clock clock; float prev_t = 0.f;
    bool popup_showing=false; int popup_weapon_id=WPN_NONE; (void)popup_weapon_id;

    while (win.isOpen() && !gs->is_game_over) {
        sf::Event ev;
        sf::Vector2i mouse = sf::Mouse::getPosition(win);
        bool left_clicked  = false;

        while (win.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) win.close();
            if (ev.type == sf::Event::MouseButtonReleased &&
                ev.mouseButton.button == sf::Mouse::Left)
                left_clicked = true;
        }

        float now = clock.getElapsedTime().asSeconds();
        float dt  = now - prev_t; prev_t = now;

        if (gs->weapon_drop.pending && !popup_showing)
            { popup_showing=true; popup_weapon_id=gs->weapon_drop.weapon_id; ui_mode=0; }
        if (!gs->weapon_drop.pending) popup_showing=false;

        // Attack lunge animation, lock atk_trig to prevent race with game loop
        pthread_mutex_lock(&atk_trig.lock);
        if (atk_trig.fired && lng_actor==-1) {
            lng_actor=atk_trig.attacker; lng_target=atk_trig.target;
            lng_timer=0.f; lng_hit=false; atk_trig.fired=false;
        }
        pthread_mutex_unlock(&atk_trig.lock);
        float lng_ofs=0.f, slash_prog=0.f;
        if (lng_actor!=-1) {
            lng_timer+=dt; float p=lng_timer/LNG_DUR;
            if      (p<0.4f)  { lng_ofs=(p/0.4f)*140.f; }
            else if (p<0.62f) { lng_ofs=140.f; slash_prog=(p-0.4f)/0.22f;
                if (!lng_hit) { if(lng_target>=0&&lng_target<MAX_PLAYERS+MAX_ENEMIES) flash[lng_target]=1.f;
                    attack_fx_target=lng_target; attack_fx_timer=ATTACK_FX_DUR; lng_hit=true; } }
            else if (p<1.f)   { lng_ofs=(1.f-(p-0.62f)/0.38f)*140.f; }
            else               { lng_actor=-1; lng_target=-1; }
        }
        for (int i=0;i<MAX_PLAYERS+MAX_ENEMIES;i++) { flash[i]-=dt*3.f; if(flash[i]<0)flash[i]=0; }
        if (attack_fx_timer>0.f) { attack_fx_timer-=dt; if(attack_fx_timer<0.f)attack_fx_timer=0.f; }
        if (ultimate_flash_timer>0.f) ultimate_flash_timer-=dt;

        // Background
        win.clear({14,11,26});
        if (hasBg) {
            sf::Sprite bg(bg_tex);
            bg.setScale((float)WIN_W/bg_tex.getSize().x,(float)WIN_H/bg_tex.getSize().y);
            win.draw(bg);
        }

        int np=gs->num_players, ne=gs->num_enemies;

        auto apply_color=[&](sf::Sprite& spr,bool dead,float fl){
            if(dead){spr.setColor({80,80,80,140});return;}
            if(fl>0.f){spr.setColor({255,(sf::Uint8)(255*(1.f-fl)),(sf::Uint8)(255*(1.f-fl)),255});return;}
            spr.setColor(sf::Color::White);
        };
        auto draw_flipped=[&](sf::Texture& tex,int frame,float cx,float fy,bool fx,bool dead,float fl){
            sf::Sprite spr(tex); spr.setTextureRect({frame*CELL_W,0,CELL_W,CELL_H});
            spr.setOrigin(CELL_W/2.f,(float)CELL_H); spr.setScale(fx?-1.f:1.f,1.f);
            spr.setPosition(cx,fy); apply_color(spr,dead,fl); win.draw(spr);
        };

        // Draw players
        for (int i=0;i<np;i++) {
            Entity& p = gs->players[i]; 
            sf::Vector2f fp=player_foot(i,np);
            float cx=fp.x,fy=fp.y,ox=(lng_actor==i)?lng_ofs:0.f;
            bool is_act=(gs->active_turn==i);
            
            sf::CircleShape sh(28.f,18); sh.setScale(1.f,0.2f);
            sh.setFillColor({0,0,0,p.is_alive?(sf::Uint8)60:(sf::Uint8)20});
            sh.setOrigin(28.f,28.f); sh.setPosition(cx+ox,fy); win.draw(sh);
            
            if (is_act&&p.is_alive) {
                float pulse=0.55f+0.45f*sinf(now*5.f);
                sf::CircleShape g(48.f,32); g.setFillColor({0,0,0,0});
                g.setOutlineThickness(4.f);
                g.setOutlineColor({255,(sf::Uint8)(220*pulse),0,(sf::Uint8)(210*pulse)});
                g.setOrigin(48.f,48.f); g.setPosition(cx+ox,fy-48.f); win.draw(g);
            }

            // ANIMATE SPRITE FRAMES DURING LUNGE
            int p_frame = (lng_actor == i) ? compute_lunge_frame(lng_timer, LNG_DUR) : 0;


                        // NEW TARGETING LOGIC FOR HEALING
            sf::FloatRect p_hitbox(cx+ox-28.f, fy-(float)CELL_H, 56.f, (float)CELL_H);
            if (p.is_alive && ui_mode == 6) {
                sf::RectangleShape hb({p_hitbox.width, p_hitbox.height});
                hb.setPosition(p_hitbox.left, p_hitbox.top);
                hb.setFillColor({0,0,0,0});
                hb.setOutlineThickness(2.f);
                hb.setOutlineColor({255,220,0,200});
                win.draw(hb);
            }

            if (p.is_alive && ui_mode == 6 && left_clicked) {
                if (p_hitbox.contains((float)mouse.x,(float)mouse.y)) {
                    send_ui_event_locked(ACTION_HEAL, i, -1, gs->active_turn);
                    ui_mode = 0;
                }
            }


            if (hasP) draw_flipped(texP, p_frame, cx+ox, fy, true, !p.is_alive, flash[i]);
            else {
                sf::RectangleShape r({40.f,70.f});
                r.setFillColor(p.is_alive?sf::Color{100,160,255}:sf::Color{60,60,60});
                r.setOrigin(20.f,70.f); r.setPosition(cx+ox,fy); win.draw(r);
            }
            
            if (p.is_stunned&&p.is_alive) {
                for (int s=0;s<3;s++) {
                    float ang=(float)s/3.f*6.2832f+now*3.f;
                    sf::CircleShape star(5.f,5); star.setFillColor({255,220,0});
                    star.setOrigin(5.f,5.f);
                    star.setPosition(cx+ox+cosf(ang)*22.f,fy-CELL_H+sinf(ang)*8.f); win.draw(star);
                }
                sf::Text st("STUNNED",font,11u); st.setFillColor({255,220,0});
                sf::FloatRect stb=st.getLocalBounds();
                st.setPosition(cx-stb.width/2.f,fy-CELL_H-40.f); win.draw(st);
            }
            char lbl[16]; snprintf(lbl,sizeof(lbl),"P%d",i+1);
            draw_bars(win,font,cx+ox,fy,p.hp,p.max_hp,p.stamina,p.max_stamina,!p.is_alive,lbl,false);
        }

        // Draw enemies
        for (int i=0;i<ne;i++) {
            Entity& e = gs->enemies[i]; 
            sf::Vector2f fp=enemy_foot(i,ne);
            float cx=fp.x,fy=fp.y; int eid=MAX_PLAYERS+i;
            bool is_act=(gs->active_turn==eid); float ox=(lng_actor==eid)?-lng_ofs:0.f;
            
            sf::CircleShape sh(28.f,18); sh.setScale(1.f,0.2f);
            sh.setFillColor({0,0,0,e.is_alive?(sf::Uint8)60:(sf::Uint8)20});
            sh.setOrigin(28.f,28.f); sh.setPosition(cx+ox,fy); win.draw(sh);
            
            if (is_act&&e.is_alive) {
                float pulse=0.55f+0.45f*sinf(now*5.f);
                sf::CircleShape g(48.f,32); g.setFillColor({0,0,0,0});
                g.setOutlineThickness(4.f);
                g.setOutlineColor({255,(sf::Uint8)(80*pulse),80,(sf::Uint8)(210*pulse)});
                g.setOrigin(48.f,48.f); g.setPosition(cx+ox,fy-48.f); win.draw(g);
            }
            
            sf::Texture* et=nullptr;
            if      (i%3==0&&hasS) et=&texS;
            else if (i%3==1&&hasR) et=&texR;
            else if (hasB)         et=&texB;
            
            // ANIMATE SPRITE FRAMES DURING LUNGE
            int e_frame = (lng_actor == eid) ? compute_lunge_frame(lng_timer, LNG_DUR) : 0;

            if (et) draw_flipped(*et, e_frame, cx+ox, fy, true, !e.is_alive, flash[eid]);
            else {
                sf::RectangleShape r({40.f,60.f});
                r.setFillColor(e.is_alive?sf::Color{220,80,80}:sf::Color{60,60,60});
                r.setOrigin(20.f,60.f); r.setPosition(cx+ox,fy); win.draw(r);
            }
            
            if (e.is_stunned&&e.is_alive) {
                for (int s=0;s<3;s++) {
                    float ang=(float)s/3.f*6.2832f+now*3.f;
                    sf::CircleShape star(5.f,5); star.setFillColor({255,220,0});
                    star.setOrigin(5.f,5.f);
                    star.setPosition(cx+ox+cosf(ang)*22.f,fy-CELL_H+sinf(ang)*8.f); win.draw(star);
                }
            }
            
            sf::FloatRect hitbox(cx+ox-28.f, fy-(float)CELL_H, 56.f, (float)CELL_H);
            if (e.is_alive && (ui_mode==1||ui_mode==2||ui_mode==5)) {
                sf::RectangleShape hb({hitbox.width, hitbox.height});
                hb.setPosition(hitbox.left, hitbox.top);
                hb.setFillColor({0,0,0,0});
                hb.setOutlineThickness(2.f);
                hb.setOutlineColor({255,220,0,200});
                win.draw(hb);
            }

            if (e.is_alive && (ui_mode==1||ui_mode==2||ui_mode==5) && left_clicked) {
                if (hitbox.contains((float)mouse.x,(float)mouse.y)) {
                    int active = gs->active_turn;
                    if (ui_mode==1)      send_ui_event_locked(ACTION_STRIKE,    eid, -1,          active);
                    else if (ui_mode==2) send_ui_event_locked(ACTION_EXHAUST,   eid, -1,          active);
                    else                 send_ui_event_locked(ACTION_USE_WEAPON, eid, ui_weapon_sel, active);
                    ui_weapon_sel=-1; ui_mode=0;
                }
            }
            char lbl[16]; snprintf(lbl,sizeof(lbl),"E%d",i+1);
            draw_bars(win,font,cx+ox,fy,e.hp,e.max_hp,e.stamina,e.max_stamina,!e.is_alive,lbl,true);
        }

        // Slash FX
        if (lng_actor!=-1&&slash_prog>0.f) {
            bool tp=(lng_target<MAX_PLAYERS); float sx,sy;
            if (tp&&lng_target<np) { sf::Vector2f fp=player_foot(lng_target,np); sx=fp.x; sy=fp.y-CELL_H*0.5f; }
            else { int ti=lng_target-MAX_PLAYERS; if(ti<ne){sf::Vector2f fp=enemy_foot(ti,ne);sx=fp.x;sy=fp.y-CELL_H*0.5f;}
                   else{sx=WIN_W/2.f;sy=GROUND_Y-60.f;} }
            draw_slash(win,sx,sy,slash_prog);
        }

        // TOP HUD
        sf::RectangleShape hud_top({(float)WIN_W,42.f}); hud_top.setFillColor({0,0,0,200}); win.draw(hud_top);
        char ks[64]; snprintf(ks,sizeof(ks),"Kills: %d / 10",gs->enemies_killed);
        sf::Text kt(ks,font,20); kt.setFillColor({255,210,0}); kt.setPosition(20.f,10.f); win.draw(kt);
        if (gs->active_turn>=0) {
            bool ip=(gs->active_turn<MAX_PLAYERS);
            int  idx=ip?gs->active_turn+1:gs->active_turn-MAX_PLAYERS+1;
            char ts[64]; snprintf(ts,sizeof(ts),"%s %d's Turn",ip?"Player":"Enemy",idx);
            sf::Text tt(ts,font,22); float pulse=0.5f+0.5f*sinf(now*4.f);
            tt.setFillColor(ip?sf::Color{100,200,255}:sf::Color{255,(sf::Uint8)(80+40*pulse),80,255});
            sf::FloatRect tb=tt.getLocalBounds(); tt.setPosition(WIN_W/2.f-tb.width/2.f,10.f); win.draw(tt);
        }
        if (gs->ultimate_pause) {
            sf::Text ult("ULTIMATE: Enemies Frozen!",font,17); ult.setFillColor({0,220,255});
            sf::FloatRect ub=ult.getLocalBounds(); ult.setPosition(WIN_W/2.f-ub.width/2.f,WIN_H-52.f); win.draw(ult);
        }

        // BOTTOM LOG
        sf::RectangleShape bot({(float)WIN_W,34.f}); bot.setFillColor({0,0,0,215});
        bot.setPosition(0.f,(float)WIN_H-34.f); win.draw(bot);
        sf::Text logtext(gs->action_log,font,14); logtext.setFillColor({0,230,195});
        logtext.setPosition(16.f,(float)WIN_H-27.f); win.draw(logtext);

        // Ultimate flash overlay
        if (ultimate_flash_timer>0.f) {
            float alpha=ultimate_flash_timer/1.5f;
            sf::RectangleShape fov({(float)WIN_W,(float)WIN_H});
            fov.setFillColor({0,200,255,(sf::Uint8)(180*alpha)}); win.draw(fov);
            for (int g=0;g<4;g++) {
                sf::Text glow("ULTIMATE!",font,90u);
                glow.setFillColor({0,100,150,(sf::Uint8)(40*alpha)});
                sf::FloatRect gb=glow.getLocalBounds(); glow.setOrigin(gb.width/2.f,gb.height/2.f);
                glow.setPosition(WIN_W/2.f+(g%2==0?-g:g)*3.f,WIN_H/2.f+(g/2==0?-g:g)*3.f-30.f); win.draw(glow);
            }
            sf::Text ut("ULTIMATE!",font,90u); ut.setFillColor({255,220,50,(sf::Uint8)(255*alpha)});
            sf::FloatRect ub=ut.getLocalBounds(); ut.setOrigin(ub.width/2.f,ub.height/2.f);
            ut.setPosition(WIN_W/2.f,WIN_H/2.f-30.f); win.draw(ut);
        }

        // WEAPON DROP POPUP
        if (popup_showing&&gs->weapon_drop.pending) {
            int wpn_id=gs->weapon_drop.weapon_id, for_player=gs->weapon_drop.for_player;
            sf::RectangleShape overlay({(float)WIN_W,(float)WIN_H});
            overlay.setFillColor({0,0,0,160}); win.draw(overlay);
            float pw=420.f,ph=280.f,px=WIN_W/2.f-pw/2.f,py=WIN_H/2.f-ph/2.f;
            sf::RectangleShape pbg({pw,ph}); pbg.setFillColor({20,15,35,250});
            pbg.setOutlineThickness(3.f);
            pbg.setOutlineColor(wpn_id>=0&&wpn_id<WPN_COUNT?WPN_COLORS[wpn_id]:sf::Color{200,200,200});
            pbg.setPosition(px,py); win.draw(pbg);
            sf::Text title("WEAPON DROPPED!",font,22u); title.setFillColor({255,220,50});
            sf::FloatRect tb=title.getLocalBounds();
            title.setPosition(px+pw/2.f-tb.width/2.f,py+16.f); win.draw(title);
            if (wpn_id>=0&&wpn_id<WPN_COUNT) {
                draw_weapon_icon(win,font,wpn_id,px+pw/2.f-32.f,py+56.f,64.f);
                sf::Text wn(WPN_NAMES[wpn_id],font,18u); wn.setFillColor(WPN_COLORS[wpn_id]);
                sf::FloatRect wb=wn.getLocalBounds(); wn.setPosition(px+pw/2.f-wb.width/2.f,py+130.f); win.draw(wn);
                char stats[64]; snprintf(stats,sizeof(stats),"Slots: %d   Damage: %d",WPN_SLOTS[wpn_id],WPN_DAMAGE[wpn_id]);
                sf::Text st(stats,font,14u); st.setFillColor({180,180,180});
                sf::FloatRect stb=st.getLocalBounds(); st.setPosition(px+pw/2.f-stb.width/2.f,py+158.f); win.draw(st);
            }
            char plbl[64]; snprintf(plbl,sizeof(plbl),"Player %d — Pick up?",for_player+1);
            sf::Text plblt(plbl,font,14u); plblt.setFillColor({200,200,255});
            sf::FloatRect pf=plblt.getLocalBounds(); plblt.setPosition(px+pw/2.f-pf.width/2.f,py+184.f); win.draw(plblt);
            bool pick_hov  = draw_button(win,font,"Pick Up",px+40.f,       py+ph-55.f,140.f,38.f,mouse,true,{30,100,30,220});
            bool leave_hov = draw_button(win,font,"Leave",  px+pw-180.f,   py+ph-55.f,140.f,38.f,mouse,true,{100,30,30,220});
            if (left_clicked) {
                if (pick_hov) {
                    pthread_mutex_lock(&gs->memory_lock);
                    if (for_player>=0&&for_player<gs->num_players) {
                        if (!pick_up_weapon(gs, gs->players[for_player],wpn_id))
                            cerr << "Arbiter: Player " << for_player+1
                                 << " could not pick up weapon (no room)" << endl;
                    }
                    gs->weapon_drop.pending=false; gs->weapon_drop.decision=1;
                    snprintf(gs->action_log,MAX_ACTION_LOG,"Player %d picked up %s!",
                             for_player+1,wpn_id>=0&&wpn_id<WPN_COUNT?WPN_NAMES[wpn_id]:"?");
                    pthread_mutex_unlock(&gs->memory_lock); popup_showing=false;
                } else if (leave_hov) {
                    pthread_mutex_lock(&gs->memory_lock);
                    // Per spec: if the player does not pick up the weapon,
                    // an enemy is GUARANTEED to pick it up.
                    for (int i=0;i<gs->num_enemies;i++) {
                        if (gs->enemies[i].is_alive) {
                            if (!pick_up_weapon(gs, gs->enemies[i],wpn_id))
                                cerr << "Arbiter: Enemy " << i+1
                                     << " could not pick up weapon (no room)" << endl;
                            snprintf(gs->action_log,MAX_ACTION_LOG,
                                     "Enemy %d picked up %s!",
                                     i+1,
                                     (wpn_id>=0&&wpn_id<WPN_COUNT)?WPN_NAMES[wpn_id]:"?");
                            break;
                        }
                    }
                    gs->weapon_drop.pending=false; gs->weapon_drop.decision=2;
                    pthread_mutex_unlock(&gs->memory_lock); popup_showing=false;
                }
            }
        }

        // PLAYER ACTION PANEL

        int active=gs->active_turn;
        bool is_player_turn=(active>=0&&active<MAX_PLAYERS&&!popup_showing);
        if (is_player_turn) {
            Entity& ap=gs->players[active];
            ui_can_ultimate=entity_has_weapon(ap,WPN_SOLAR_CORE)
                           &&entity_has_weapon(ap,WPN_LUNAR_BLADE);

            float panel_x=10.f,panel_y=WIN_H-200.f,panel_w=490.f,panel_h=160.f;
            sf::RectangleShape panel({panel_w,panel_h}); panel.setFillColor({10,8,20,220});
            panel.setOutlineThickness(2.f); panel.setOutlineColor({80,80,160});
            panel.setPosition(panel_x,panel_y); win.draw(panel);

            char ptitle[64]; snprintf(ptitle,sizeof(ptitle),"Player %d - Choose Action:",active+1);
            sf::Text pt(ptitle,font,13u); pt.setFillColor({180,180,255});
            pt.setPosition(panel_x+10.f,panel_y+8.f); win.draw(pt);

            float bw=108.f,bh=34.f,bx=panel_x+10.f,by=panel_y+32.f,gap=4.f;

            // Row 1
            if (draw_button(win,font,"Strike",  bx,           by,bw,bh,mouse)&&left_clicked&&ui_mode==0) ui_mode=1;
            if (draw_button(win,font,"Exhaust", bx+(bw+gap),  by,bw,bh,mouse)&&left_clicked&&ui_mode==0) ui_mode=2;
            if (draw_button(win,font,"Heal", bx+2*(bw+gap),by,bw,bh,mouse)&&left_clicked&&ui_mode==0)
                    ui_mode = 6;
            if (draw_button(win,font,"Skip",    bx+3*(bw+gap),by,bw,bh,mouse)&&left_clicked&&ui_mode==0)
                { send_ui_event_locked(ACTION_SKIP,  -1, -1, active); ui_mode=0; }

            // Row 2
            float by2=by+bh+gap;
            if (draw_button(win,font,"Use Weapon",bx,           by2,bw,bh,mouse)&&left_clicked&&ui_mode==0) ui_mode=3;
            if (draw_button(win,font,"Swap In",   bx+(bw+gap),  by2,bw,bh,mouse)&&left_clicked&&ui_mode==0) ui_mode=4;
            if (draw_button(win,font,"ULTIMATE",  bx+2*(bw+gap),by2,bw,bh,mouse,ui_can_ultimate,{60,30,100,220})
                &&left_clicked&&ui_mode==0&&ui_can_ultimate)
                { send_ui_event_locked(ACTION_ULTIMATE,-1,-1,active); ui_mode=0; }
            if (draw_button(win,font,"Quit",      bx+3*(bw+gap),by2,bw,bh,mouse,true,{80,20,20,200})
                &&left_clicked&&ui_mode==0)
                { send_ui_event_locked(ACTION_QUIT,-1,-1,active); gs->is_game_over=true; running=false; }

            // Mode hint
            const char* hints[]={"","Click an enemy to Strike","Click an enemy to Exhaust",
                                  "Click a weapon below to Use","Click a long-term slot to Swap In",
                                  "Click an enemy to attack with weapon"};
            if (ui_mode>0&&ui_mode<6) {
                sf::Text hint(hints[ui_mode],font,13u); hint.setFillColor({255,200,80});
                hint.setPosition(panel_x+10.f,panel_y+panel_h-20.f); win.draw(hint);
            }

            // Inventory panel
            float inv_x=panel_x+panel_w+10.f,inv_y=WIN_H-200.f;
            float slot_s=36.f,slot_gap=3.f,inv_cols=10.f;
            float inv_w=inv_cols*(slot_s+slot_gap)+10.f,inv_h=2*(slot_s+slot_gap)+30.f;
            sf::RectangleShape inv_panel({inv_w,inv_h}); inv_panel.setFillColor({10,8,20,220});
            inv_panel.setOutlineThickness(2.f); inv_panel.setOutlineColor({80,80,160});
            inv_panel.setPosition(inv_x,inv_y); win.draw(inv_panel);
            sf::Text inv_title("Inventory:",font,12u); inv_title.setFillColor({180,180,255});
            inv_title.setPosition(inv_x+5.f,inv_y+5.f); win.draw(inv_title);

            for (int s=0;s<INVENTORY_SIZE;s++) {
                int col=s%10,row=s/10;
                float sx=inv_x+5.f+col*(slot_s+slot_gap), sy=inv_y+22.f+row*(slot_s+slot_gap);
                int wid=ap.inventory[s]; bool highlight=false;
                if (ui_mode==3&&wid!=WPN_NONE) {
                    sf::FloatRect hitbox(sx,sy,slot_s,slot_s);
                    if (hitbox.contains((float)mouse.x,(float)mouse.y)) {
                        highlight=true;
                        if (left_clicked) { ui_weapon_sel=wid; ui_mode=5; }
                    }
                }
                draw_weapon_icon(win,font,wid,sx,sy,slot_s,highlight);
            }

            // Long-term storage (swap-in mode)
            if (ui_mode==4) {
                float lt_x=inv_x,lt_y=inv_y+inv_h+5.f,lt_w=inv_w,lt_h=slot_s+28.f;
                sf::RectangleShape lt_panel({lt_w,lt_h}); lt_panel.setFillColor({15,10,25,220});
                lt_panel.setOutlineThickness(2.f); lt_panel.setOutlineColor({100,60,160});
                lt_panel.setPosition(lt_x,lt_y); win.draw(lt_panel);
                sf::Text lt_title("Long-Term Storage:",font,11u); lt_title.setFillColor({180,120,255});
                lt_title.setPosition(lt_x+5.f,lt_y+4.f); win.draw(lt_title);
                for (int k=0;k<ap.lt_count&&k<10;k++) {
                    float slx=lt_x+5.f+k*(slot_s+slot_gap),sly=lt_y+20.f;
                    int wid=ap.long_term[k]; bool hl=false;
                    sf::FloatRect hitbox(slx,sly,slot_s,slot_s);
                    if (hitbox.contains((float)mouse.x,(float)mouse.y)) {
                        hl=true;
                        // k is the long-term slot index, weapon_id carries it
                        if (left_clicked) { send_ui_event_locked(ACTION_SWAP_IN,-1,k,active); ui_mode=0; }
                    }
                    draw_weapon_icon(win,font,wid,slx,sly,slot_s,hl);
                }
            }
        }

        win.display();
    }

    // Game-over screen
    if (win.isOpen()) {
        bool won=gs->player_won;
        for (int frame=0;frame<240&&win.isOpen();frame++) {
            sf::Event gev; while(win.pollEvent(gev)) if(gev.type==sf::Event::Closed)win.close();
            float ft=(float)frame/60.f; win.clear({8,5,15});
            sf::RectangleShape ov({(float)WIN_W,(float)WIN_H});
            ov.setFillColor({0,0,0,(sf::Uint8)std::min(200,(int)(ft*80))}); win.draw(ov);
            float sc=ft<0.5f?ft/0.5f:1.f;
            sf::Text mt(won?"VICTORY!":"DEFEAT",font,(unsigned)(80*sc));
            mt.setFillColor(won?sf::Color{60,255,120}:sf::Color{255,60,60});
            sf::FloatRect mb=mt.getLocalBounds(); mt.setOrigin(mb.width/2.f,mb.height/2.f);
            mt.setPosition(WIN_W/2.f,WIN_H/2.f-50.f); win.draw(mt);
            if (ft>0.65f) {
                char sub[64]; snprintf(sub,sizeof(sub),"Enemies slain: %d",gs->enemies_killed);
                sf::Text st(sub,font,26);
                st.setFillColor({200,200,200,(sf::Uint8)std::min(255,(int)((ft-0.65f)*3.f*255))});
                sf::FloatRect sb=st.getLocalBounds(); st.setOrigin(sb.width/2.f,0.f);
                st.setPosition(WIN_W/2.f,WIN_H/2.f+45.f); win.draw(st);
            }
            win.display();
        }
        win.close();
    }
    return nullptr;
}

//main
int main() {
    struct sigaction sa_term{}, sa_chld{}, sa_alrm{};
    sa_term.sa_handler = signal_handler;
    sigemptyset(&sa_term.sa_mask);
    if (sigaction(SIGTERM, &sa_term, nullptr) == -1) { perror("Arbiter: sigaction SIGTERM"); exit(1); }

    sa_chld.sa_handler = signal_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, nullptr) == -1) { perror("Arbiter: sigaction SIGCHLD"); exit(1); }

    sa_alrm.sa_handler = signal_handler;
    sigemptyset(&sa_alrm.sa_mask);
    if (sigaction(SIGALRM, &sa_alrm, nullptr) == -1) { perror("Arbiter: sigaction SIGALRM"); exit(1); }

    init_shared_memory();
    init_game();

    pthread_t rtid, dtid;
    int rc;
    if ((rc = pthread_create(&rtid, nullptr, render_thread_func, nullptr)) != 0) {
        cerr << "Arbiter: failed to create render thread: " << strerror(rc) << endl; exit(1);
    }
    if ((rc = pthread_create(&dtid, nullptr, deadlock_monitor_func, nullptr)) != 0) {
        cerr << "Arbiter: failed to create deadlock monitor thread: " << strerror(rc) << endl; exit(1);
    }

    cout << "Arbiter: waiting for HIP and ASP..." << endl;
    sleep(2);

    while (!gs->is_game_over) {
        // Block while weapon drop popup is active.
        while (gs->weapon_drop.pending && !gs->is_game_over)
            usleep(50000);

        pthread_mutex_lock(&gs->memory_lock);
        stamina_tick(gs);
        pthread_mutex_unlock(&gs->memory_lock);
        usleep(1000000);   // 1 second per tick, spec says "each second speed is added"

        int actor = find_next_actor(gs);
        if (actor == -1) continue;
        gs->active_turn = actor;

        if (actor < MAX_PLAYERS) {
        
            PlayerInputEvent pie{}; pie.valid = false;
            int timeout = 0;

            while (!pie.valid && !gs->is_game_over) {
                if (pop_player_action_locked(actor, &pie)) break;
                usleep(50000);
                if (++timeout > 200) {
                    pie = {ACTION_SKIP, -1, -1, true};
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "Player %d timed out — auto SKIP", actor + 1);
                    break;
                }
            }

            if (!pie.valid || !running) break;
            if (pie.action == ACTION_QUIT) { gs->is_game_over = true; break; }

            // Commit to the shared action fields so execute_action() works
            // uniformly for both players and enemies.
            pthread_mutex_lock(&gs->memory_lock);
            gs->action_actor     = actor;
            gs->action_target    = pie.target;
            gs->action_weapon_id = pie.weapon_id;
            gs->pending_action   = pie.action;
            pthread_mutex_unlock(&gs->memory_lock);

        } else {
            // Enemy turn: wait for ASP thread to post action
            int timeout = 0;
            while (gs->pending_action == ACTION_NONE && !gs->is_game_over) {
                usleep(50000);
                if (++timeout > 60) {
                    gs->pending_action = ACTION_SKIP;
                    gs->action_actor   = actor;
                    snprintf(gs->action_log, MAX_ACTION_LOG,
                             "Enemy %d timed out — auto SKIP", actor - MAX_PLAYERS + 1);
                    break;
                }
            }
        }

        execute_action();
        check_win_loss(gs);
    }

    cout << (gs->player_won ? "Players WIN!" : "GAME OVER") << endl;
    pthread_join(rtid,  nullptr);
    pthread_join(dtid,  nullptr);
    cleanup();
    return 0;
}