// ============================================================
//  arbiter.cpp  —  Game Arbiter (Engine + SFML Arena UI)
//  Expedition-33-style 2D arena: players left, enemies right.
//  Real pixel-art sprites, idle animation, lunge-attack effect,
//  hit flash, slash particles, Expedition-33 camera aesthetic.
//
//  OS Concepts used:
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

// ─── Window / layout constants ───────────────────────────────────────────────
static const int   WIN_W     = 1500;
static const int   WIN_H     = 720;
static const float GROUND_Y  = WIN_H * 0.72f;   // Y of character feet
static const float P_X1      = 80.f;
static const float P_X2      = 440.f;
static const float E_X1      = 760.f;
static const float E_X2      = 1220.f;

// ─── Sprite sheet constants ──────────────────────────────────────────────────
// players.png      : 640x128  – 5 heroes, each 128×128, one idle pose per hero
// enemy_*_flip.png : 640x128  – 5 animation frames, each 128×128
static const int CELL_W      = 128;
static const int CELL_H      = 128;
static const int ANIM_FRAMES = 5;

// ─── Globals ─────────────────────────────────────────────────────────────────
GameState* gs      = nullptr;
bool       running = true;

// Attack-animation trigger (written by execute_action, read by render thread)
struct AttackTrigger {
    volatile bool fired;
    volatile int  attacker;
    volatile int  target;
};
static AttackTrigger atk_trig = {false, 0, 0};

// ─── Forward declarations ────────────────────────────────────────────────────
void  init_game();
void  stamina_tick();
int   find_next_actor();
void  execute_action();
void  check_win_loss();
void* render_thread_func(void*);
void* deadlock_monitor_func(void*);
void* stun_resume_thread(void*);
void  signal_handler(int);
void  cleanup();

struct StunResumeArgs { pid_t pid; bool players_group; };

// ─── Inventory helpers ───────────────────────────────────────────────────────
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
                int w = e.inventory[i], ws = WPN_SLOTS[w];
                for (int j = i; j < i+ws && j < INVENTORY_SIZE; j++) e.inventory[j] = WPN_NONE;
                if (e.lt_count < LONG_TERM_SIZE) e.long_term[e.lt_count++] = w;
                snprintf(gs->action_log, MAX_ACTION_LOG, "Swapped weapon %d to long-term", w);
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

// ─── Signals ─────────────────────────────────────────────────────────────────
void signal_handler(int sig) {
    if (sig == SIGTERM) { gs->is_game_over = true; running = false; }
    if (sig == SIGCHLD) { int s; waitpid(-1, &s, WNOHANG); }
    if (sig == SIGALRM) {
        if (gs->asp_pid > 0) { kill(gs->asp_pid, SIGCONT); gs->ultimate_pause = false; }
    }
}
void* stun_resume_thread(void* arg) {
    StunResumeArgs* a = (StunResumeArgs*)arg;
    sleep(3);
    pthread_mutex_lock(&gs->memory_lock);
    if (a->players_group) for (int i=0;i<gs->num_players;i++) gs->players[i].is_stunned=false;
    else                   for (int i=0;i<gs->num_enemies;i++) gs->enemies[i].is_stunned=false;
    pthread_mutex_unlock(&gs->memory_lock);
    kill(a->pid, SIGCONT);
    delete a; return nullptr;
}
void trigger_stun(pid_t pid, bool pg, int target_idx=-1) {
    if (pid <= 0) return;
    pthread_mutex_lock(&gs->memory_lock);
    // Only stun the specific target entity, not the entire group
    if (target_idx >= 0) {
        if (pg && target_idx < gs->num_players) { 
            if (gs->players[target_idx].is_alive) gs->players[target_idx].is_stunned=true; 
        }
        else if (!pg && target_idx < gs->num_enemies) { 
            if (gs->enemies[target_idx].is_alive) gs->enemies[target_idx].is_stunned=true; 
        }
    }
    pthread_mutex_unlock(&gs->memory_lock);
    kill(pid, SIGUSR1);
    StunResumeArgs* a = new StunResumeArgs; a->pid=pid; a->players_group=pg;
    pthread_t t; pthread_create(&t,nullptr,stun_resume_thread,a); pthread_detach(t);
}

// ─── Shared memory + game init ────────────────────────────────────────────────
void init_shared_memory() {
    int fd = shm_open(SHARED_MEM_NAME, O_CREAT|O_RDWR, 0666);
    if (fd==-1) { perror("shm_open"); exit(1); }
    ftruncate(fd, sizeof(GameState));
    gs = (GameState*)mmap(0, sizeof(GameState), PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (gs==MAP_FAILED) { perror("mmap"); exit(1); }
    close(fd);
    memset(gs, 0, sizeof(GameState));
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gs->memory_lock, &a);
    pthread_mutexattr_destroy(&a);
}
void init_game() {
    srand(ROLL_SEED);
    cout << "=== CHRONO RIFT ===" << endl;
    cout << "Enter number of players (1-4): ";
    cin >> gs->num_players;
    if (gs->num_players<1||gs->num_players>MAX_PLAYERS) gs->num_players=1;
    for (int i=0;i<gs->num_players;i++) {
        Entity& p=gs->players[i];
        p.is_alive=true; p.is_stunned=false;
        p.hp=ROLL_FULL+(rand()%901+100); p.max_hp=p.hp;
        p.max_stamina=100; p.stamina=0;
        p.damage=LAST_DIGIT+10; p.speed=100/gs->num_players;
        p.lt_count=0;
        memset(p.inventory,WPN_NONE,sizeof(p.inventory));
        memset(p.long_term, WPN_NONE,sizeof(p.long_term));
    }
    gs->num_enemies=rand()%8+2;
    for (int i=0;i<gs->num_enemies;i++) {
        Entity& e=gs->enemies[i];
        e.is_alive=true; e.is_stunned=false;
        e.hp=LAST_TWO+(rand()%151+50); e.max_hp=e.hp;
        e.max_stamina=150; e.stamina=0;
        e.damage=SECOND_LAST+10; e.speed=rand()%21+10;
        e.lt_count=0;
        memset(e.inventory,WPN_NONE,sizeof(e.inventory));
        memset(e.long_term, WPN_NONE,sizeof(e.long_term));
    }
    gs->is_game_over=false; gs->player_won=false;
    gs->enemies_killed=0; gs->active_turn=-1;
    gs->pending_action=ACTION_NONE; gs->ultimate_pause=false;
    gs->solar_core_exists=true;  gs->solar_core_holder=-1;
    gs->lunar_blade_exists=true; gs->lunar_blade_holder=-1;
    gs->eclipse_relic_exists=false; gs->eclipse_relic_holder=-1;
    gs->arbiter_pid=getpid();
    snprintf(gs->action_log,MAX_ACTION_LOG,"Chrono Rift! Players:%d  Enemies:%d",
             gs->num_players,gs->num_enemies);
    cout<<"Players:"<<gs->num_players<<"  Enemies:"<<gs->num_enemies<<endl;
}

// ─── Stamina + scheduling ─────────────────────────────────────────────────────
void stamina_tick() {
    pthread_mutex_lock(&gs->memory_lock);
    for (int i=0;i<gs->num_players;i++) {
        Entity& p=gs->players[i];
        if (!p.is_alive||p.is_stunned) continue;
        p.stamina+=p.speed; if(p.stamina>p.max_stamina) p.stamina=p.max_stamina;
    }
    if (!gs->ultimate_pause) {
        for (int i=0;i<gs->num_enemies;i++) {
            Entity& e=gs->enemies[i];
            if (!e.is_alive||e.is_stunned) continue;
            e.stamina+=e.speed; if(e.stamina>e.max_stamina) e.stamina=e.max_stamina;
        }
    }
    pthread_mutex_unlock(&gs->memory_lock);
}
int find_next_actor() {
    for (int i=0;i<gs->num_players;i++) {
        Entity& p=gs->players[i];
        if (p.is_alive&&!p.is_stunned&&p.stamina>=p.max_stamina) return i;
    }
    for (int i=0;i<gs->num_enemies;i++) {
        Entity& e=gs->enemies[i];
        if (e.is_alive&&!e.is_stunned&&e.stamina>=e.max_stamina) return MAX_PLAYERS+i;
    }
    return -1;
}

// ─── Action execution ─────────────────────────────────────────────────────────
void execute_action() {
    if (gs->pending_action==ACTION_NONE) return;
    pthread_mutex_lock(&gs->memory_lock);
    int actor=gs->action_actor, target=gs->action_target;
    int action=gs->pending_action, wpn=gs->action_weapon_id;
    bool ap=(actor<MAX_PLAYERS), tp=(target<MAX_PLAYERS);
    Entity& ae=ap?gs->players[actor]:gs->enemies[actor-MAX_PLAYERS];
    Entity& te=tp?gs->players[target]:gs->enemies[target-MAX_PLAYERS];

    switch(action) {
        case ACTION_STRIKE:
            te.hp-=ae.damage; ae.stamina=0;
            snprintf(gs->action_log,MAX_ACTION_LOG,"Entity %d STRIKES entity %d for %d dmg",actor,target,ae.damage);
            atk_trig.attacker=actor; atk_trig.target=target; atk_trig.fired=true;
            break;
        case ACTION_EXHAUST: {
            te.stamina-=ae.damage; if(te.stamina<0)te.stamina=0; ae.stamina=0;
            snprintf(gs->action_log,MAX_ACTION_LOG,"Entity %d EXHAUSTS entity %d",actor,target);
            int target_idx = tp ? target : (target - MAX_PLAYERS);
            trigger_stun(tp?gs->hip_pid:gs->asp_pid, tp, target_idx);
            break;
        }
        case ACTION_USE_WEAPON:
            if(wpn>=0&&wpn<WPN_COUNT){
                te.hp-=WPN_DAMAGE[wpn]; ae.stamina=0;
                snprintf(gs->action_log,MAX_ACTION_LOG,"Entity %d weapon %d → %d dmg",actor,wpn,WPN_DAMAGE[wpn]);
                atk_trig.attacker=actor; atk_trig.target=target; atk_trig.fired=true;
                bool hs=false,hl=false;
                for(int s=0;s<INVENTORY_SIZE;s++){if(ae.inventory[s]==WPN_SOLAR_CORE)hs=true;if(ae.inventory[s]==WPN_LUNAR_BLADE)hl=true;}
                if(hs&&hl&&ap){gs->ultimate_pause=true;if(gs->asp_pid>0)kill(gs->asp_pid,SIGSTOP);alarm(10);
                    snprintf(gs->action_log,MAX_ACTION_LOG,"ULTIMATE! ASP paused 10s!");}
            }
            break;
        case ACTION_HEAL:
            ae.hp+=ae.max_hp/10; if(ae.hp>ae.max_hp)ae.hp=ae.max_hp; ae.stamina=0;
            snprintf(gs->action_log,MAX_ACTION_LOG,"Entity %d HEALS",actor); break;
        case ACTION_SKIP:
            ae.stamina=ae.max_stamina/2;
            snprintf(gs->action_log,MAX_ACTION_LOG,"Entity %d SKIPS",actor); break;
        case ACTION_SWAP_IN:
            if(wpn>=0&&wpn<ae.lt_count){
                int wid=ae.long_term[wpn]; pick_up_weapon(ae,wid);
                for(int k=wpn;k<ae.lt_count-1;k++)ae.long_term[k]=ae.long_term[k+1];
                ae.lt_count--; ae.stamina=0;
                snprintf(gs->action_log,MAX_ACTION_LOG,"Entity %d swapped in weapon %d",actor,wid);
            }
            break;
    }
    if(te.hp<=0){te.is_alive=false;
        if(!tp){gs->enemies_killed++;
            snprintf(gs->action_log,MAX_ACTION_LOG,"Enemy %d defeated! Kills:%d",target-MAX_PLAYERS,gs->enemies_killed);}}
    gs->pending_action=ACTION_NONE; gs->active_turn=-1;
    pthread_mutex_unlock(&gs->memory_lock);
}
void check_win_loss() {
    if(gs->enemies_killed>=10){gs->is_game_over=true;gs->player_won=true;return;}
    bool any=false;
    for(int i=0;i<gs->num_players;i++) if(gs->players[i].is_alive){any=true;break;}
    if(!any){gs->is_game_over=true;gs->player_won=false;}
}

// ─── Deadlock monitor ─────────────────────────────────────────────────────────
void* deadlock_monitor_func(void*) {
    while(!gs->is_game_over){
        sleep(2);
        pthread_mutex_lock(&gs->memory_lock);
        if(gs->solar_core_holder!=-1&&gs->lunar_blade_holder!=-1){
            int a=gs->solar_core_holder,b=gs->lunar_blade_holder;
            if(a!=b){gs->solar_core_holder=-1;
                snprintf(gs->action_log,MAX_ACTION_LOG,"[Arbiter] DEADLOCK! Released Solar Core from %d",a);}
        }
        pthread_mutex_unlock(&gs->memory_lock);
    }
    return nullptr;
}
void cleanup(){
    pthread_mutex_destroy(&gs->memory_lock);
    munmap(gs,sizeof(GameState)); shm_unlink(SHARED_MEM_NAME);
    cout<<"Arbiter: cleaned up."<<endl;
}

// ═══════════════════════════════════════════════════════════════════════════
//  RENDER THREAD  —  Expedition-33 style 2D arena
// ═══════════════════════════════════════════════════════════════════════════

// ─── Helpers ─────────────────────────────────────────────────────────────────

sf::Vector2f player_foot(int i, int n) {
    float x = (n<=1) ? (P_X1+P_X2)/2.f : P_X1+(P_X2-P_X1)*((float)i/(n-1));
    float y  = GROUND_Y - (float)i*14.f;   // slight depth offset
    return {x, y};
}
sf::Vector2f enemy_foot(int i, int n) {
    float x = (n<=1) ? (E_X1+E_X2)/2.f : 600+(E_X2-E_X1)*((float)1.5f*i/(n));
    float y  = GROUND_Y - (float)i*4.f;   // slight depth offset
    return {x, y};
}

// Draws HP + Stamina bars above a character
void draw_bars(sf::RenderWindow& win, sf::Font& font,
               float cx, float foot_y, int hp, int mhp, int sp, int msp,
               bool dead, const char* name, bool compact=false)
{
    if(dead) return;
    float bw=compact ? 44.f : 80.f;
    float bh=8.f;
    float bx=cx-bw/2.f;
    float by=foot_y-CELL_H-24.f;
    sf::RectangleShape bg({bw+4.f, bh*2.f+8.f});
    bg.setFillColor({0,0,0,180}); bg.setPosition(bx-2.f,by-2.f); win.draw(bg);

    float hr=(float)hp/mhp; if(hr<0)hr=0; if(hr>1)hr=1;
    sf::Color hc = hr>0.5f?sf::Color{50,220,80}:hr>0.25f?sf::Color{230,200,0}:sf::Color{220,50,50};
    sf::RectangleShape hbar({bw*hr,bh}); hbar.setFillColor(hc); hbar.setPosition(bx,by); win.draw(hbar);

    float sr=(float)sp/msp; if(sr<0)sr=0; if(sr>1)sr=1;
    sf::RectangleShape sbar({bw*sr,bh-2.f}); sbar.setFillColor({255,200,0}); sbar.setPosition(bx,by+bh+2.f); win.draw(sbar);

    sf::Text t(name,font,compact ? 10u : 11u); t.setFillColor({220,220,255});
    sf::FloatRect tb=t.getLocalBounds(); t.setPosition(cx-tb.width/2.f,by-16.f); win.draw(t);

    char vals[32];
    if (compact)
        snprintf(vals, sizeof(vals), "%d/%d  %d/%d", hp, mhp, sp, msp);
    else
        snprintf(vals, sizeof(vals), "%d/%d  %d/%d", hp, mhp, sp, msp);
    sf::Text v(vals, font, compact ? 8u : 9u);
    v.setFillColor({200,200,200});
    sf::FloatRect vb = v.getLocalBounds();
    v.setPosition(cx-vb.width/2.f, by+bh+12.f); win.draw(v);
}

// Draws slash particles at (sx,sy) with progress 0..1
void draw_slash(sf::RenderWindow& win, float sx, float sy, float p) {
    struct L { float ax; float ay; float len; };
    L lines[] = {{-0.5f,-0.3f,80},{-0.2f,-0.5f,100},{0.1f,-0.4f,110},{0.4f,-0.2f,85},{0.6f,0.1f,60}};
    for(auto& l:lines){
        float len=l.len*p;
        sf::Uint8 alpha=(sf::Uint8)(255*(1.f-p));
        sf::Vertex vx[]={
            sf::Vertex({sx,sy},{255,255,100,alpha}),
            sf::Vertex({sx+cosf(l.ax)*len, sy+sinf(l.ay)*len},{255,255,255,0})
        };
        win.draw(vx,2,sf::Lines);
    }
    // Impact circle
    float r=30.f*p;
    sf::CircleShape c(r,12);
    c.setFillColor({255,220,100,(sf::Uint8)(120*(1.f-p))});
    c.setOrigin(r,r); c.setPosition(sx,sy); win.draw(c);
}

// Draws the arena environment
// void draw_arena_bg(sf::RenderWindow& win, float t) {
//     // Sky
//     sf::RectangleShape sky({(float)WIN_W,(float)WIN_H});
//     sky.setFillColor({14,11,26}); win.draw(sky);

//     // Stars
//     for(int s=0;s<100;s++){
//         float sx=(float)((s*173+41)%WIN_W);
//         float sy=(float)((s*113+7) %(int)(WIN_H*0.65f));
//         float tw=0.5f+0.5f*sinf(t*1.8f+s*0.9f);
//         sf::CircleShape star(1.3f);
//         star.setFillColor({255,255,255,(sf::Uint8)(60+80*tw)});
//         star.setPosition(sx,sy); win.draw(star);
//     }

//     // Mountains (parallax layers)
//     struct Mt { float x,y,w,h; sf::Color c; };
//     Mt mts[]={
//         {  0, GROUND_Y-260,340,260,{38,28,50}},
//         {280, GROUND_Y-210,310,210,{45,35,58}},
//         {560, GROUND_Y-290,360,290,{33,24,44}},
//         {870, GROUND_Y-230,320,230,{42,32,54}},
//         {1100,GROUND_Y-270,340,270,{36,27,48}},
//     };
//     for(auto& m:mts){
//         sf::ConvexShape tri(3);
//         tri.setPoint(0,{m.x,          m.y+m.h});
//         tri.setPoint(1,{m.x+m.w/2.f, m.y    });
//         tri.setPoint(2,{m.x+m.w,     m.y+m.h});
//         tri.setFillColor(m.c); win.draw(tri);
//     }

//     // // Ground platform
//     // sf::RectangleShape gnd({(float)WIN_W,(float)WIN_H*0.30f});
//     // gnd.setPosition(0.f,GROUND_Y); gnd.setFillColor({28,20,38}); win.draw(gnd);

//     // // Ground edge highlight
//     // sf::RectangleShape gl({(float)WIN_W,3.f});
//     // gl.setPosition(0.f,GROUND_Y-1.f); gl.setFillColor({110,75,160}); win.draw(gl);

//     // // Tile pattern
//     // for(int r=0;r<6;r++) for(int c=0;c<17;c++){
//     //     sf::RectangleShape tile({79.f,79.f});
//     //     tile.setPosition((float)(c*80),(float)(GROUND_Y+r*80));
//     //     tile.setFillColor((r+c)%2==0?sf::Color{36,26,46}:sf::Color{30,22,40});
//     //     win.draw(tile);
//     // }

//     // // Glowing arena center line
//     // sf::RectangleShape cline({2.f,(float)WIN_H*0.30f});
//     // cline.setPosition((float)WIN_W/2.f-1.f, GROUND_Y);
//     // cline.setFillColor({130,80,200,50}); win.draw(cline);

//     // Torch columns (left & right edges of arena)
//     for(int side=0;side<2;side++){
//         float tx=(side==0)?50.f:(float)WIN_W-50.f;
//         float ty=GROUND_Y;
//         sf::RectangleShape pole({10.f,100.f}); pole.setFillColor({65,45,28});
//         pole.setPosition(tx-5.f,ty-100.f); win.draw(pole);
//         float flk=0.85f+0.15f*sinf(t*9.f+side*3.f);
//         sf::CircleShape fl(16.f*flk,8); fl.setFillColor({255,(sf::Uint8)(90+40*sinf(t*7.f)),0,230});
//         fl.setOrigin(16.f,16.f); fl.setPosition(tx,ty-112.f); win.draw(fl);
//         sf::CircleShape fi(8.f,8); fi.setFillColor({255,225,100,200});
//         fi.setOrigin(8.f,8.f); fi.setPosition(tx,ty-112.f); win.draw(fi);
//     }
// }

// ─── Render thread ────────────────────────────────────────────────────────────
void* render_thread_func(void*) {
    const char* disp = getenv("DISPLAY");
    if (!disp || !disp[0]) {
        while (!gs->is_game_over) {
            cout<<"\r[Game] Kills:"<<gs->enemies_killed<<" Turn:"<<gs->active_turn<<" "<<gs->action_log<<"    "<<flush;
            usleep(500000);
        }
        return nullptr;
    }

    sf::RenderWindow win(sf::VideoMode(WIN_W,WIN_H),"Chrono Rift — Arena",
                         sf::Style::Titlebar|sf::Style::Close);
    win.setFramerateLimit(60);

    sf::Font font;
    if (!font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"))
        font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");

    // ── Load textures (from Sprites/ folder next to binary) ─────────────
    sf::Texture texP, texS, texR, texB, background_texture, attack_texture;
    bool hasP = texP.loadFromFile("Sprites/players.png");
    bool hasS = texS.loadFromFile("Sprites/enemy_silver_flip.png");
    bool hasR = texR.loadFromFile("Sprites/enemy_red_flip.png");
    bool hasB = texB.loadFromFile("Sprites/enemy_blue_flip.png");
    bool hasBg = background_texture.loadFromFile("Sprites/backgrounds_2.jpeg");
    // bool hasAttack = attack_texture.loadFromFile("Sprites/Attack.png");
    texP.setSmooth(false); texS.setSmooth(false); texR.setSmooth(false); texB.setSmooth(false);
    background_texture.setSmooth(false);
    // attack_texture.setSmooth(false);

    // Per-entity idle animation state
    float idle_t[MAX_PLAYERS+MAX_ENEMIES]   = {};
    int   idle_f[MAX_PLAYERS+MAX_ENEMIES]   = {};
    float idle_spd = 0.14f;   // seconds per frame

    // Per-entity hit-flash 0..1
    float flash[MAX_PLAYERS+MAX_ENEMIES] = {};

    // Simple hit overlay: show Attack.jpeg for 1 second on each hit
    float attack_fx_timer = 0.f;
    int   attack_fx_target = -1;
    static const float ATTACK_FX_DUR = 1.f;

    // Lunge animation state
    int   lng_actor  = -1, lng_target = -1;
    float lng_timer  = 0.f;
    bool  lng_hit    = false;
    static const float LNG_DUR = 0.75f;

    sf::Clock clock;
    float prev_t = 0.f;

    while (win.isOpen() && !gs->is_game_over) {
        sf::Event ev;
        while (win.pollEvent(ev))
            if (ev.type==sf::Event::Closed) win.close();

        float now = clock.getElapsedTime().asSeconds();
        float dt  = now - prev_t; prev_t = now;

        // ── Pick up new attack trigger ─────────────────────────────────────
        if (atk_trig.fired && lng_actor==-1) {
            lng_actor  = atk_trig.attacker;
            lng_target = atk_trig.target;
            lng_timer  = 0.f;
            lng_hit    = false;
            atk_trig.fired = false;
        }

        // ── Advance lunge ─────────────────────────────────────────────────
        float lng_ofs    = 0.f;   // pixel offset toward target
        float slash_prog = 0.f;
        if (lng_actor != -1) {
            lng_timer += dt;
            float p = lng_timer / LNG_DUR;
            if (p < 0.4f) {
                lng_ofs = (p/0.4f)*140.f;
            } else if (p < 0.62f) {
                lng_ofs = 140.f;
                slash_prog = (p-0.4f)/0.22f;
                if (!lng_hit) {
                    int ti = lng_target;
                    if (ti>=0 && ti<MAX_PLAYERS+MAX_ENEMIES) flash[ti]=1.f;
                    attack_fx_target = ti;
                    attack_fx_timer = ATTACK_FX_DUR;
                    lng_hit = true;
                }
            } else if (p < 1.f) {
                lng_ofs = (1.f-(p-0.62f)/0.38f)*140.f;
            } else {
                lng_actor=-1; lng_target=-1;
            }
        }

        // ── Advance idle animation (only used during enemy attack) ─────────
        int ntot = gs->num_players + gs->num_enemies;
        for (int i=0; i<ntot; i++) {
            if (i==lng_actor) continue;
            if (i>=MAX_PLAYERS) continue;
            idle_t[i]+=dt;
            if (idle_t[i]>=idle_spd) {
                idle_t[i]=0.f;
                idle_f[i]=(idle_f[i]+1)%ANIM_FRAMES;
            }
        }

        // ── Decay hit flash ────────────────────────────────────────────────
        for (int i=0;i<MAX_PLAYERS+MAX_ENEMIES;i++) {
            flash[i]-=dt*3.f; if(flash[i]<0)flash[i]=0;
        }
        if (attack_fx_timer > 0.f) {
            attack_fx_timer -= dt;
            if (attack_fx_timer < 0.f) attack_fx_timer = 0.f;
        }

        // ── Clear + arena bg ──────────────────────────────────────────────
        win.clear({14,11,26});
        // draw_arena_bg(win, now);
        sf::Sprite bg(background_texture);
        bg.setTextureRect({0,0,WIN_W,WIN_H});
        win.draw(bg);

        int np=gs->num_players, ne=gs->num_enemies;

        // Helper to tint a sprite
        auto apply_color=[&](sf::Sprite& spr, bool dead, float fl){
            if (dead)       { spr.setColor({80,80,80,140}); return; }
            if (fl>0.f) {
                sf::Uint8 gg=(sf::Uint8)(255*(1.f-fl));
                spr.setColor({255,gg,gg,255}); return;
            }
            spr.setColor(sf::Color::White);
        };

        auto draw_flipped_sprite = [&](sf::Texture& tex, int frame, float cx, float fy, bool flip_x, bool dead, float fl) {
            sf::Sprite spr(tex);
            spr.setTextureRect({frame*CELL_W, 0, CELL_W, CELL_H});
            spr.setOrigin(CELL_W/2.f, (float)CELL_H);
            spr.setScale(flip_x ? -1.f : 1.f, 1.f);
            spr.setPosition(cx, fy);
            apply_color(spr, dead, fl);
            win.draw(spr);
        };

        // ── Draw players ──────────────────────────────────────────────────
        for (int i=0;i<np;i++) {
            Entity& p=gs->players[i];
            sf::Vector2f fp=player_foot(i,np);
            float cx=fp.x, fy=fp.y;
            bool is_act=(gs->active_turn==i);
            bool lunging=(lng_actor==i);
            float ox=lunging?lng_ofs:0.f;

            // Shadow
            sf::CircleShape sh(28.f,18);sh.setScale(1.f,0.2f);
            sh.setFillColor({0,0,0,p.is_alive?60:20});
            sh.setOrigin(28.f,28.f); sh.setPosition(cx+ox,fy); win.draw(sh);

            // Active glow ring
            if (is_act&&p.is_alive) {
                float pulse=0.55f+0.45f*sinf(now*5.f);
                sf::CircleShape g(48.f,32); g.setFillColor({0,0,0,0});
                g.setOutlineThickness(4.f);
                g.setOutlineColor({255,(sf::Uint8)(220*pulse),0,(sf::Uint8)(210*pulse)});
                g.setOrigin(48.f,48.f); g.setPosition(cx+ox,fy-48.f); win.draw(g);
            }

            if (hasP) {
                // players.png: 5 different heroes in columns, each CELL_W wide
                // (idle only — no row animation; each column = 1 hero)
                int hero_col=i%5;
                draw_flipped_sprite(texP, hero_col, cx+ox, fy, true, !p.is_alive, flash[i]);
            } else {
                // Fallback: simple rectangle
                sf::RectangleShape r({40.f,70.f});
                r.setFillColor(p.is_alive?sf::Color{100,160,255}:sf::Color{60,60,60});
                r.setOrigin(20.f,70.f); r.setPosition(cx+ox,fy); win.draw(r);
            }

            // Stun stars
            if (p.is_stunned&&p.is_alive)
                for(int s=0;s<3;s++){
                    float ang=(float)s/3.f*6.2832f+now*3.f;
                    sf::CircleShape star(5.f,5); star.setFillColor({255,220,0});
                    star.setOrigin(5.f,5.f);
                    star.setPosition(cx+ox+cosf(ang)*22.f,fy-CELL_H+sinf(ang)*8.f);
                    win.draw(star);
                }

            char lbl[16]; snprintf(lbl,sizeof(lbl),"P%d",i+1);
            draw_bars(win,font,cx+ox,fy,p.hp,p.max_hp,p.stamina,p.max_stamina,!p.is_alive,lbl,false);
        }

        // ── Draw enemies ──────────────────────────────────────────────────
        for (int i=0;i<ne;i++) {
            Entity& e=gs->enemies[i];
            sf::Vector2f fp=enemy_foot(i,ne);
            float cx=fp.x, fy=fp.y;
            int eid=MAX_PLAYERS+i;
            bool is_act=(gs->active_turn==eid);
            bool lunging=(lng_actor==eid);
            float ox=lunging?-lng_ofs:0.f;   // enemies lunge LEFT

            // Shadow
            sf::CircleShape sh(28.f,18); sh.setScale(1.f,0.2f);
            sh.setFillColor({0,0,0,e.is_alive?60:20});
            sh.setOrigin(28.f,28.f); sh.setPosition(cx+ox,fy); win.draw(sh);

            // Active glow ring
            if (is_act&&e.is_alive) {
                float pulse=0.55f+0.45f*sinf(now*5.f);
                sf::CircleShape g(48.f,32); g.setFillColor({0,0,0,0});
                g.setOutlineThickness(4.f);
                g.setOutlineColor({255,(sf::Uint8)(80*pulse),80,(sf::Uint8)(210*pulse)});
                g.setOrigin(48.f,48.f); g.setPosition(cx+ox,fy-48.f); win.draw(g);
            }

            // Select texture by round-robin type
            sf::Texture* et=nullptr;
            if      (i%3==0&&hasS) et=&texS;
            else if (i%3==1&&hasR) et=&texR;
            else if (hasB)          et=&texB;
            else if (hasS)          et=&texS;

            int frame = lunging ? (int)(slash_prog * (ANIM_FRAMES - 1)) : 0;

            if (et) {
                draw_flipped_sprite(*et, frame, cx+ox, fy, true, !e.is_alive, flash[eid]);
            } else {
                sf::RectangleShape r({40.f,60.f});
                r.setFillColor(e.is_alive?sf::Color{200,60,60}:sf::Color{60,60,60});
                r.setOrigin(20.f,60.f); r.setPosition(cx+ox,fy); win.draw(r);
            }

            // Stun stars
            if (e.is_stunned&&e.is_alive)
                for(int s=0;s<3;s++){
                    float ang=(float)s/3.f*6.2832f+now*3.f;
                    sf::CircleShape star(5.f,5); star.setFillColor({255,220,0});
                    star.setOrigin(5.f,5.f);
                    star.setPosition(cx+ox+cosf(ang)*22.f,fy-CELL_H+sinf(ang)*8.f);
                    win.draw(star);
                }

            char lbl[16]; snprintf(lbl,sizeof(lbl),"E%d",i+1);
            draw_bars(win,font,cx+ox,fy,e.hp,e.max_hp,e.stamina,e.max_stamina,!e.is_alive,lbl,true);
        }

        // ── Slash effect at target ────────────────────────────────────────
        if (lng_actor!=-1 && slash_prog>0.f) {
            bool tp=(lng_target<MAX_PLAYERS);
            float sx,sy;
            if (tp&&lng_target<np) {
                sf::Vector2f fp=player_foot(lng_target,np);
                sx=fp.x; sy=fp.y-CELL_H*0.5f;
            } else {
                int ti=lng_target-MAX_PLAYERS;
                if(ti<ne){ sf::Vector2f fp=enemy_foot(ti,ne); sx=fp.x; sy=fp.y-CELL_H*0.5f; }
                else { sx=WIN_W/2.f; sy=GROUND_Y-60.f; }
            }
            draw_slash(win,sx,sy,slash_prog);
        }

        // ── Attack hit overlay (1 second) ────────────────────────────────
        // if (hasAttack && attack_fx_timer > 0.f && attack_fx_target >= 0) 
        if ( attack_fx_timer > 0.f && attack_fx_target >= 0) {
            float alpha = attack_fx_timer / ATTACK_FX_DUR;
            // sf::Sprite atk(attack_texture);
            float ax = WIN_W * 0.5f;
            float ay = GROUND_Y - 180.f;
            if (attack_fx_target < MAX_PLAYERS) {
                sf::Vector2f fp = player_foot(attack_fx_target, np);
                ax = fp.x;
                ay = fp.y - CELL_H - 40.f;
            } else {
                int ti = attack_fx_target - MAX_PLAYERS;
                if (ti >= 0 && ti < ne) {
                    sf::Vector2f fp = enemy_foot(ti, ne);
                    ax = fp.x;
                    ay = fp.y - CELL_H - 40.f;
                }
            }

            sf::IntRect bounds = attack_texture.getSize().x > 0 && attack_texture.getSize().y > 0
                ? sf::IntRect(0, 0, (int)attack_texture.getSize().x, (int)attack_texture.getSize().y)
                : sf::IntRect();
            // atk.setTextureRect(bounds);
            // atk.setOrigin((float)bounds.width / 2.f, (float)bounds.height / 2.f);
            // atk.setPosition(ax, ay);
            // atk.setScale(0.6f, 0.6f);
            // atk.setColor({255, 255, 255, (sf::Uint8)(255.f * alpha)});
            // win.draw(atk);
        }

        // ── TOP HUD ───────────────────────────────────────────────────────
        sf::RectangleShape hud_top({(float)WIN_W,42.f});
        hud_top.setFillColor({0,0,0,200}); win.draw(hud_top);

        char ks[64]; snprintf(ks,sizeof(ks),"Kills: %d / 10",gs->enemies_killed);
        sf::Text kt(ks,font,20); kt.setFillColor({255,210,0}); kt.setPosition(20.f,10.f); win.draw(kt);

        if (gs->active_turn>=0) {
            bool ip=(gs->active_turn<MAX_PLAYERS);
            int idx=ip?gs->active_turn+1:gs->active_turn-MAX_PLAYERS+1;
            char ts[64]; snprintf(ts,sizeof(ts),"%s %d's Turn",ip?"Player":"Enemy",idx);
            sf::Text tt(ts,font,22);
            float pulse=0.5f+0.5f*sinf(now*4.f);
            sf::Uint8 pr=ip?255:(sf::Uint8)(200+55*pulse);
            sf::Uint8 pg=ip?(sf::Uint8)(200+55*pulse):80;
            sf::Uint8 pb=ip?255:80;
            tt.setFillColor({pr,pg,pb,255});
            sf::FloatRect tb=tt.getLocalBounds();
            tt.setPosition(WIN_W/2.f-tb.width/2.f,10.f); win.draw(tt);
        }

        // Ultimate label
        if (gs->ultimate_pause) {
            sf::Text ult("ULTIMATE: Enemies Frozen!",font,17);
            ult.setFillColor({0,220,255});
            sf::FloatRect ub=ult.getLocalBounds();
            ult.setPosition(WIN_W/2.f-ub.width/2.f,WIN_H-52.f); win.draw(ult);
        }

        // ── Team labels ────────────────────────────────────────────────────
        sf::Text pl("PLAYERS",font,13); pl.setFillColor({100,180,255,150});
        pl.setPosition(P_X1, GROUND_Y+6.f); win.draw(pl);
        sf::Text el("ENEMIES",font,13); el.setFillColor({220,80,80,150});
        el.setPosition(E_X1, GROUND_Y+6.f); win.draw(el);

        // ── BOTTOM LOG BAR ─────────────────────────────────────────────────
        sf::RectangleShape bot({(float)WIN_W,34.f});
        bot.setFillColor({0,0,0,215}); bot.setPosition(0.f,(float)WIN_H-34.f); win.draw(bot);
        sf::Text log(gs->action_log,font,14);
        log.setFillColor({0,230,195}); log.setPosition(16.f,(float)WIN_H-27.f); win.draw(log);

        win.display();
    }

    // ── Game Over ─────────────────────────────────────────────────────────────
    if (win.isOpen()) {
        bool won=gs->player_won;
        for (int frame=0; frame<240&&win.isOpen(); frame++) {
            sf::Event ev; while(win.pollEvent(ev)) if(ev.type==sf::Event::Closed) win.close();
            float ft=(float)frame/60.f;
            win.clear({8,5,15});
            sf::RectangleShape ov({(float)WIN_W,(float)WIN_H});
            ov.setFillColor({0,0,0,(sf::Uint8)min(200,(int)(ft*80))}); win.draw(ov);

            float sc=ft<0.5f?ft/0.5f:1.f;
            sf::Text mt(won?"VICTORY!":"DEFEAT",font,(unsigned)(80*sc));
            mt.setFillColor(won?sf::Color{60,255,120}:sf::Color{255,60,60});
            sf::FloatRect mb=mt.getLocalBounds();
            mt.setOrigin(mb.width/2.f,mb.height/2.f);
            mt.setPosition(WIN_W/2.f,WIN_H/2.f-50.f); win.draw(mt);

            if (ft>0.65f) {
                char sub[64]; snprintf(sub,sizeof(sub),"Enemies slain: %d",gs->enemies_killed);
                sf::Text st(sub,font,26);
                st.setFillColor({200,200,200,(sf::Uint8)min(255,(int)((ft-0.65f)*3.f*255))});
                sf::FloatRect sb=st.getLocalBounds();
                st.setOrigin(sb.width/2.f,0.f);
                st.setPosition(WIN_W/2.f,WIN_H/2.f+45.f); win.draw(st);
            }
            win.display();
        }
        win.close();
    }
    return nullptr;
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    signal(SIGTERM,signal_handler);
    signal(SIGCHLD,signal_handler);
    signal(SIGALRM,signal_handler);

    init_shared_memory();
    init_game();

    pthread_t rtid, dtid;
    pthread_create(&rtid, nullptr, render_thread_func,   nullptr);
    pthread_create(&dtid, nullptr, deadlock_monitor_func, nullptr);

    cout<<"Arbiter: waiting for HIP and ASP..."<<endl;
    sleep(2);

    while (!gs->is_game_over) {
        stamina_tick();
        usleep(100000);
        int actor=find_next_actor();
        if (actor==-1) continue;
        gs->active_turn=actor;

        int timeout=0;
        while (gs->pending_action==ACTION_NONE&&!gs->is_game_over) {
            usleep(50000); timeout++;
            if (actor>=MAX_PLAYERS&&timeout>60) {
                gs->pending_action=ACTION_SKIP; gs->action_actor=actor;
                snprintf(gs->action_log,MAX_ACTION_LOG,"Enemy %d timed out — auto SKIP",actor-MAX_PLAYERS);
                break;
            }
        }
        execute_action();
        check_win_loss();
    }

    cout<<(gs->player_won?"Players WIN!":"GAME OVER")<<endl;
    pthread_join(rtid,nullptr); pthread_join(dtid,nullptr);
    cleanup();
    return 0;
}