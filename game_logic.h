#ifndef GAME_LOGIC_H
#define GAME_LOGIC_H

#include "common.h"

// Inventory helpers
int  find_contiguous_slots(Entity& e, int size);
void make_room(GameState* gs, Entity& e, int size);
bool pick_up_weapon(GameState* gs, Entity& e, int id);
bool entity_has_weapon(Entity& e, int wpn_id);

// Game-state logic
void stamina_tick(GameState* gs);
int  find_next_actor(GameState* gs);
void check_win_loss(GameState* gs);
void respawn_enemies(GameState* gs);

// Action buffer (ring-buffer push / pop)
void push_player_action(GameState* gs, int player_idx,
                        int action, int target, int weapon_id);
bool pop_player_action(GameState* gs, int player_idx,
                       PlayerInputEvent* out);

// Enemy AI decision
void enemy_decide(GameState* gs, int enemy_idx);

// UI event queue
void send_ui_event(GameState* gs, int action, int target,
                   int weapon_id, int player_idx);

#endif
