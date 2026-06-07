#include "game_logic.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

// ── Inventory helpers ──────────────────────────────────────────────

int find_contiguous_slots(Entity& e, int size) {
    for (int i = 0; i <= INVENTORY_SIZE - size; i++) {
        bool ok = true;
        for (int j = i; j < i + size; j++)
            if (e.inventory[j] != WPN_NONE) { ok = false; break; }
        if (ok) return i;
    }
    return -1;
}

void make_room(GameState* gs, Entity& e, int size) {
    while (find_contiguous_slots(e, size) == -1) {
        for (int i = 0; i < INVENTORY_SIZE; i++) {
            if (e.inventory[i] != WPN_NONE) {
                int w = e.inventory[i], ws = WPN_SLOTS[w];
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

bool pick_up_weapon(GameState* gs, Entity& e, int id) {
    int size = WPN_SLOTS[id];
    int slot = find_contiguous_slots(e, size);
    if (slot == -1) { make_room(gs, e, size); slot = find_contiguous_slots(e, size); }
    if (slot == -1) return false;
    for (int i = slot; i < slot + size; i++) e.inventory[i] = id;
    return true;
}

bool entity_has_weapon(Entity& e, int wpn_id) {
    for (int s = 0; s < INVENTORY_SIZE; s++)
        if (e.inventory[s] == wpn_id) return true;
    return false;
}

// ── Game-state logic ───────────────────────────────────────────────

void stamina_tick(GameState* gs) {
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
}

int find_next_actor(GameState* gs) {
    int best = -1, best_sta = -1;
    for (int i = 0; i < gs->num_players; i++) {
        Entity& p = gs->players[i];
        if (p.is_alive && !p.is_stunned && p.stamina >= p.max_stamina)
            if (p.stamina > best_sta) { best_sta = p.stamina; best = i; }
    }
    for (int i = 0; i < gs->num_enemies; i++) {
        Entity& e = gs->enemies[i];
        if (e.is_alive && !e.is_stunned && e.stamina >= e.max_stamina)
            if (e.stamina > best_sta) { best_sta = e.stamina; best = MAX_PLAYERS + i; }
    }
    return best;
}

void check_win_loss(GameState* gs) {
    if (gs->enemies_killed >= 10) {
        gs->is_game_over = true;
        gs->player_won   = true;
        return;
    }
    bool any_e = false;
    for (int i = 0; i < gs->num_enemies; i++)
        if (gs->enemies[i].is_alive) { any_e = true; break; }
    if (!any_e && gs->enemies_killed < 10) respawn_enemies(gs);

    bool any_p = false;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].is_alive) { any_p = true; break; }
    if (!any_p) { gs->is_game_over = true; gs->player_won = false; }
}

void respawn_enemies(GameState* gs) {
    int count = rand() % 3 + 2;
    if (count > MAX_ENEMIES) count = MAX_ENEMIES;
    gs->num_enemies = count;
    for (int i = 0; i < count; i++) {
        Entity& e     = gs->enemies[i];
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
    snprintf(gs->action_log, MAX_ACTION_LOG,
             "New wave! %d enemies approach...", count);
}

// ── Action ring-buffer ─────────────────────────────────────────────

void push_player_action(GameState* gs, int player_idx,
                        int action, int target, int weapon_id)
{
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
}

bool pop_player_action(GameState* gs, int player_idx,
                       PlayerInputEvent* out)
{
    PlayerActionBuffer& buf = gs->player_buffers[player_idx];
    bool has = (buf.count > 0);
    if (has) {
        *out     = buf.events[buf.head];
        buf.head = (buf.head + 1) % ACTION_BUF_CAP;
        buf.count--;
    }
    return has;
}

// ── Enemy AI ───────────────────────────────────────────────────────

void enemy_decide(GameState* gs, int enemy_idx) {
    int target = -1;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].is_alive) { target = i; break; }
    if (target == -1) return;

    if (rand() % 10 < 8) {
        gs->action_actor   = MAX_PLAYERS + enemy_idx;
        gs->action_target  = target;
        gs->pending_action = ACTION_STRIKE;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d attacks Player %d!", enemy_idx + 1, target + 1);
    } else {
        gs->action_actor   = MAX_PLAYERS + enemy_idx;
        gs->pending_action = ACTION_SKIP;
        snprintf(gs->action_log, MAX_ACTION_LOG,
                 "Enemy %d skips.", enemy_idx + 1);
    }
}

// ── UI event queue ─────────────────────────────────────────────────

void send_ui_event(GameState* gs, int action, int target,
                   int weapon_id, int player_idx)
{
    UIEvent ev{ action, target, weapon_id, player_idx };
    if (gs->ui_events.count < UI_EVENT_CAP)
        gs->ui_events.events[gs->ui_events.count++] = ev;
}
