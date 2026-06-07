#include <gtest/gtest.h>
#include <cstring>
#include <cstdlib>
#include "game_logic.h"

// ── Helpers ────────────────────────────────────────────────────────

static void init_entity(Entity& e, int hp, int stamina, int damage,
                        int speed, int max_stamina = 100) {
    memset(&e, 0, sizeof(e));
    e.is_alive    = true;
    e.is_stunned  = false;
    e.hp          = hp;
    e.max_hp      = hp;
    e.stamina     = stamina;
    e.max_stamina = max_stamina;
    e.damage      = damage;
    e.speed       = speed;
    e.lt_count    = 0;
    e.waiting_for = ART_NONE;
    memset(e.inventory, WPN_NONE, sizeof(e.inventory));
    memset(e.long_term,  WPN_NONE, sizeof(e.long_term));
}

static GameState make_default_gs() {
    GameState gs{};
    memset(&gs, 0, sizeof(gs));
    gs.num_players      = 1;
    gs.num_enemies      = 1;
    gs.is_game_over     = false;
    gs.player_won       = false;
    gs.enemies_killed   = 0;
    gs.active_turn      = -1;
    gs.pending_action   = ACTION_NONE;
    gs.ultimate_pause   = false;
    gs.ui_events.count  = 0;

    init_entity(gs.players[0], 500, 0, 15, 50);
    init_entity(gs.enemies[0], 100, 0, 19, 20, 150);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        gs.player_buffers[i].head  = 0;
        gs.player_buffers[i].tail  = 0;
        gs.player_buffers[i].count = 0;
    }
    return gs;
}

// ═══════════════════════════════════════════════════════════════════
//  common.h — constants and data-structure sanity checks
// ═══════════════════════════════════════════════════════════════════

TEST(CommonConstants, GameLimits) {
    EXPECT_EQ(MAX_PLAYERS, 4);
    EXPECT_EQ(MAX_ENEMIES, 9);
    EXPECT_EQ(INVENTORY_SIZE, 20);
    EXPECT_EQ(LONG_TERM_SIZE, 50);
    EXPECT_EQ(MAX_ACTION_LOG, 512);
}

TEST(CommonConstants, RollSeedValues) {
    EXPECT_EQ(ROLL_SEED, 795);
    EXPECT_EQ(LAST_DIGIT, 5);
    EXPECT_EQ(SECOND_LAST, 9);
    EXPECT_EQ(LAST_TWO, 95);
    EXPECT_EQ(ROLL_FULL, 795);
}

TEST(CommonConstants, WeaponCount) {
    EXPECT_EQ(WPN_COUNT, 9);
    EXPECT_EQ(WPN_NONE, -1);
}

TEST(CommonConstants, WeaponSlotsDamageConsistency) {
    for (int i = 0; i < WPN_COUNT; i++) {
        EXPECT_GT(WPN_SLOTS[i], 0)  << "weapon " << i << " has non-positive slot count";
        EXPECT_GT(WPN_DAMAGE[i], 0) << "weapon " << i << " has non-positive damage";
        EXPECT_LE(WPN_SLOTS[i], INVENTORY_SIZE)
            << "weapon " << i << " needs more slots than inventory allows";
    }
}

TEST(CommonConstants, WeaponNamesNonNull) {
    for (int i = 0; i < WPN_COUNT; i++) {
        ASSERT_NE(WPN_NAMES[i], nullptr) << "WPN_NAMES[" << i << "] is null";
        EXPECT_GT(strlen(WPN_NAMES[i]), 0u);
    }
}

TEST(CommonConstants, ActionCodes) {
    EXPECT_EQ(ACTION_NONE, 0);
    EXPECT_NE(ACTION_STRIKE, ACTION_NONE);
    EXPECT_NE(ACTION_HEAL, ACTION_NONE);
    EXPECT_NE(ACTION_QUIT, ACTION_NONE);
}

TEST(CommonConstants, ArtifactIDs) {
    EXPECT_EQ(ART_NONE, -1);
    EXPECT_NE(ART_SOLAR_CORE, ART_NONE);
    EXPECT_NE(ART_LUNAR_BLADE, ART_NONE);
    EXPECT_NE(ART_ECLIPSE_RELIC, ART_NONE);
}

TEST(CommonConstants, BufferCapacities) {
    EXPECT_EQ(UI_EVENT_CAP, 32);
    EXPECT_EQ(ACTION_BUF_CAP, 8);
}

// ═══════════════════════════════════════════════════════════════════
//  Inventory — find_contiguous_slots
// ═══════════════════════════════════════════════════════════════════

TEST(FindContiguousSlots, EmptyInventoryReturnsZero) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    EXPECT_EQ(find_contiguous_slots(e, 1), 0);
    EXPECT_EQ(find_contiguous_slots(e, 10), 0);
    EXPECT_EQ(find_contiguous_slots(e, INVENTORY_SIZE), 0);
}

TEST(FindContiguousSlots, FullInventoryReturnsNegOne) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    for (int i = 0; i < INVENTORY_SIZE; i++) e.inventory[i] = WPN_SPLINTER_STICK;
    EXPECT_EQ(find_contiguous_slots(e, 1), -1);
}

TEST(FindContiguousSlots, GapInMiddle) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    // Fill first 5 slots
    for (int i = 0; i < 5; i++) e.inventory[i] = WPN_OBSIDIAN_AXE;
    // Slots 5..19 are free → first free block at index 5
    EXPECT_EQ(find_contiguous_slots(e, 3), 5);
}

TEST(FindContiguousSlots, ExactFit) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    // Fill all but last 4
    for (int i = 0; i < INVENTORY_SIZE - 4; i++) e.inventory[i] = WPN_IRON_HALBERD;
    EXPECT_EQ(find_contiguous_slots(e, 4), INVENTORY_SIZE - 4);
    EXPECT_EQ(find_contiguous_slots(e, 5), -1);
}

TEST(FindContiguousSlots, RequestLargerThanInventory) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    EXPECT_EQ(find_contiguous_slots(e, INVENTORY_SIZE + 1), -1);
}

// ═══════════════════════════════════════════════════════════════════
//  Inventory — entity_has_weapon
// ═══════════════════════════════════════════════════════════════════

TEST(EntityHasWeapon, EmptyInventory) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    EXPECT_FALSE(entity_has_weapon(e, WPN_SOLAR_CORE));
    EXPECT_FALSE(entity_has_weapon(e, WPN_SPLINTER_STICK));
}

TEST(EntityHasWeapon, WeaponPresent) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    e.inventory[3] = WPN_VENOM_DAGGER;
    EXPECT_TRUE(entity_has_weapon(e, WPN_VENOM_DAGGER));
    EXPECT_FALSE(entity_has_weapon(e, WPN_SOLAR_CORE));
}

TEST(EntityHasWeapon, MultipleWeapons) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    // Place two different weapons
    for (int i = 0; i < WPN_SLOTS[WPN_IRON_HALBERD]; i++)
        e.inventory[i] = WPN_IRON_HALBERD;
    for (int i = WPN_SLOTS[WPN_IRON_HALBERD];
         i < WPN_SLOTS[WPN_IRON_HALBERD] + WPN_SLOTS[WPN_VENOM_DAGGER]; i++)
        e.inventory[i] = WPN_VENOM_DAGGER;
    EXPECT_TRUE(entity_has_weapon(e, WPN_IRON_HALBERD));
    EXPECT_TRUE(entity_has_weapon(e, WPN_VENOM_DAGGER));
    EXPECT_FALSE(entity_has_weapon(e, WPN_FROSTBOW));
}

// ═══════════════════════════════════════════════════════════════════
//  Inventory — pick_up_weapon
// ═══════════════════════════════════════════════════════════════════

TEST(PickUpWeapon, EmptyInventorySuccess) {
    GameState gs = make_default_gs();
    Entity& e = gs.players[0];
    EXPECT_TRUE(pick_up_weapon(&gs, e, WPN_VENOM_DAGGER));
    EXPECT_TRUE(entity_has_weapon(e, WPN_VENOM_DAGGER));
}

TEST(PickUpWeapon, PlacesInContiguousSlots) {
    GameState gs = make_default_gs();
    Entity& e = gs.players[0];
    pick_up_weapon(&gs, e, WPN_IRON_HALBERD); // 7 slots
    // First 7 slots should all be WPN_IRON_HALBERD
    for (int i = 0; i < WPN_SLOTS[WPN_IRON_HALBERD]; i++)
        EXPECT_EQ(e.inventory[i], WPN_IRON_HALBERD);
    // Rest still empty
    for (int i = WPN_SLOTS[WPN_IRON_HALBERD]; i < INVENTORY_SIZE; i++)
        EXPECT_EQ(e.inventory[i], WPN_NONE);
}

TEST(PickUpWeapon, MultipleWeaponsStackNext) {
    GameState gs = make_default_gs();
    Entity& e = gs.players[0];
    pick_up_weapon(&gs, e, WPN_SPLINTER_STICK); // 2 slots at [0..1]
    pick_up_weapon(&gs, e, WPN_VENOM_DAGGER);   // 4 slots at [2..5]
    EXPECT_EQ(e.inventory[0], WPN_SPLINTER_STICK);
    EXPECT_EQ(e.inventory[1], WPN_SPLINTER_STICK);
    EXPECT_EQ(e.inventory[2], WPN_VENOM_DAGGER);
    EXPECT_EQ(e.inventory[5], WPN_VENOM_DAGGER);
    EXPECT_EQ(e.inventory[6], WPN_NONE);
}

TEST(PickUpWeapon, MakeRoomMovesToLongTerm) {
    GameState gs = make_default_gs();
    Entity& e = gs.players[0];
    // Fill inventory with Splinter Sticks (2 slots each), 10 total
    for (int i = 0; i < 10; i++)
        pick_up_weapon(&gs, e, WPN_SPLINTER_STICK);
    // Inventory should be full (20 slots)
    EXPECT_EQ(find_contiguous_slots(e, 1), -1);

    // Now pick up a Solar Core (10 slots) — make_room must move sticks to long-term
    EXPECT_TRUE(pick_up_weapon(&gs, e, WPN_SOLAR_CORE));
    EXPECT_TRUE(entity_has_weapon(e, WPN_SOLAR_CORE));
    EXPECT_GT(e.lt_count, 0); // Some weapons moved to long-term
}

// ═══════════════════════════════════════════════════════════════════
//  Inventory — make_room
// ═══════════════════════════════════════════════════════════════════

TEST(MakeRoom, MovesWeaponToLongTermStorage) {
    GameState gs = make_default_gs();
    Entity& e = gs.players[0];
    // Fill inventory completely with Splinter Sticks
    for (int i = 0; i < INVENTORY_SIZE; i += WPN_SLOTS[WPN_SPLINTER_STICK])
        pick_up_weapon(&gs, e, WPN_SPLINTER_STICK);

    int old_lt = e.lt_count;
    make_room(&gs, e, 4); // Need 4 contiguous slots
    EXPECT_GT(e.lt_count, old_lt);
    EXPECT_NE(find_contiguous_slots(e, 4), -1);
}

TEST(MakeRoom, ActionLogUpdated) {
    GameState gs = make_default_gs();
    Entity& e = gs.players[0];
    for (int i = 0; i < INVENTORY_SIZE; i += WPN_SLOTS[WPN_SPLINTER_STICK])
        pick_up_weapon(&gs, e, WPN_SPLINTER_STICK);

    make_room(&gs, e, 4);
    EXPECT_NE(strstr(gs.action_log, "Swapped"), nullptr);
    EXPECT_NE(strstr(gs.action_log, "long-term storage"), nullptr);
}

// ═══════════════════════════════════════════════════════════════════
//  Game-state — stamina_tick
// ═══════════════════════════════════════════════════════════════════

TEST(StaminaTick, IncreasesStaminaBySpeed) {
    GameState gs = make_default_gs();
    gs.players[0].speed = 25;
    gs.enemies[0].speed = 10;
    stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 25);
    EXPECT_EQ(gs.enemies[0].stamina, 10);
}

TEST(StaminaTick, CapsAtMaxStamina) {
    GameState gs = make_default_gs();
    gs.players[0].speed       = 60;
    gs.players[0].stamina     = 80;
    gs.players[0].max_stamina = 100;
    stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 100);
}

TEST(StaminaTick, SkipsDeadEntities) {
    GameState gs = make_default_gs();
    gs.players[0].is_alive = false;
    gs.players[0].speed    = 50;
    stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 0);
}

TEST(StaminaTick, SkipsStunnedEntities) {
    GameState gs = make_default_gs();
    gs.players[0].is_stunned = true;
    gs.players[0].speed      = 50;
    stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 0);
}

TEST(StaminaTick, UltimatePauseFreezesEnemies) {
    GameState gs = make_default_gs();
    gs.ultimate_pause  = true;
    gs.players[0].speed = 25;
    gs.enemies[0].speed = 30;
    stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 25); // Player still ticks
    EXPECT_EQ(gs.enemies[0].stamina, 0);  // Enemy frozen
}

TEST(StaminaTick, MultipleTicks) {
    GameState gs = make_default_gs();
    gs.players[0].speed       = 10;
    gs.players[0].max_stamina = 100;
    for (int i = 0; i < 5; i++) stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 50);
}

TEST(StaminaTick, MultiplePlayers) {
    GameState gs = make_default_gs();
    gs.num_players = 3;
    for (int i = 0; i < 3; i++) {
        init_entity(gs.players[i], 100, 0, 10, (i + 1) * 10);
    }
    stamina_tick(&gs);
    EXPECT_EQ(gs.players[0].stamina, 10);
    EXPECT_EQ(gs.players[1].stamina, 20);
    EXPECT_EQ(gs.players[2].stamina, 30);
}

// ═══════════════════════════════════════════════════════════════════
//  Game-state — find_next_actor
// ═══════════════════════════════════════════════════════════════════

TEST(FindNextActor, NoOneReadyReturnsNegOne) {
    GameState gs = make_default_gs();
    // stamina < max_stamina for all
    EXPECT_EQ(find_next_actor(&gs), -1);
}

TEST(FindNextActor, PlayerReady) {
    GameState gs = make_default_gs();
    gs.players[0].stamina     = 100;
    gs.players[0].max_stamina = 100;
    EXPECT_EQ(find_next_actor(&gs), 0);
}

TEST(FindNextActor, EnemyReady) {
    GameState gs = make_default_gs();
    gs.enemies[0].stamina     = 150;
    gs.enemies[0].max_stamina = 150;
    EXPECT_EQ(find_next_actor(&gs), MAX_PLAYERS); // enemy 0 → idx 4
}

TEST(FindNextActor, HighestStaminaWins) {
    GameState gs = make_default_gs();
    gs.num_players = 2;
    init_entity(gs.players[1], 100, 0, 10, 10);

    gs.players[0].stamina     = 100;
    gs.players[0].max_stamina = 100;
    gs.players[1].stamina     = 200;
    gs.players[1].max_stamina = 100;

    EXPECT_EQ(find_next_actor(&gs), 1);
}

TEST(FindNextActor, DeadEntitiesSkipped) {
    GameState gs = make_default_gs();
    gs.players[0].stamina     = 100;
    gs.players[0].max_stamina = 100;
    gs.players[0].is_alive    = false;
    EXPECT_EQ(find_next_actor(&gs), -1);
}

TEST(FindNextActor, StunnedEntitiesSkipped) {
    GameState gs = make_default_gs();
    gs.players[0].stamina     = 100;
    gs.players[0].max_stamina = 100;
    gs.players[0].is_stunned  = true;
    EXPECT_EQ(find_next_actor(&gs), -1);
}

TEST(FindNextActor, EnemyBeatsSameStaminaPlayer) {
    GameState gs = make_default_gs();
    gs.players[0].stamina     = 100;
    gs.players[0].max_stamina = 100;
    gs.enemies[0].stamina     = 200;
    gs.enemies[0].max_stamina = 150;
    // Enemy has higher stamina → should be chosen
    EXPECT_EQ(find_next_actor(&gs), MAX_PLAYERS);
}

// ═══════════════════════════════════════════════════════════════════
//  Game-state — check_win_loss
// ═══════════════════════════════════════════════════════════════════

TEST(CheckWinLoss, TenKillsTriggersWin) {
    GameState gs = make_default_gs();
    gs.enemies_killed = 10;
    check_win_loss(&gs);
    EXPECT_TRUE(gs.is_game_over);
    EXPECT_TRUE(gs.player_won);
}

TEST(CheckWinLoss, AllPlayersDeadTriggersLoss) {
    GameState gs = make_default_gs();
    gs.players[0].is_alive = false;
    check_win_loss(&gs);
    EXPECT_TRUE(gs.is_game_over);
    EXPECT_FALSE(gs.player_won);
}

TEST(CheckWinLoss, AllEnemiesDeadRespawns) {
    GameState gs = make_default_gs();
    gs.enemies[0].is_alive = false;
    gs.enemies_killed       = 5; // Not 10 yet → respawn
    srand(42); // Deterministic for respawn
    check_win_loss(&gs);
    EXPECT_FALSE(gs.is_game_over);
    EXPECT_GT(gs.num_enemies, 0); // Enemies respawned
    // At least one enemy alive
    bool any_alive = false;
    for (int i = 0; i < gs.num_enemies; i++)
        if (gs.enemies[i].is_alive) any_alive = true;
    EXPECT_TRUE(any_alive);
}

TEST(CheckWinLoss, GameContinuesNormally) {
    GameState gs = make_default_gs();
    // Both sides alive, kills < 10
    check_win_loss(&gs);
    EXPECT_FALSE(gs.is_game_over);
}

TEST(CheckWinLoss, MultiplePlayersOneAlive) {
    GameState gs = make_default_gs();
    gs.num_players = 3;
    for (int i = 0; i < 3; i++) init_entity(gs.players[i], 100, 0, 10, 10);
    gs.players[0].is_alive = false;
    gs.players[1].is_alive = false;
    // Player 2 alive → game continues
    check_win_loss(&gs);
    EXPECT_FALSE(gs.is_game_over);
}

// ═══════════════════════════════════════════════════════════════════
//  Game-state — respawn_enemies
// ═══════════════════════════════════════════════════════════════════

TEST(RespawnEnemies, SpawnsBetween2And4) {
    GameState gs = make_default_gs();
    srand(0);
    respawn_enemies(&gs);
    EXPECT_GE(gs.num_enemies, 2);
    EXPECT_LE(gs.num_enemies, 4); // rand()%3+2 → [2,4]
}

TEST(RespawnEnemies, AllAliveAndInitialized) {
    GameState gs = make_default_gs();
    srand(123);
    respawn_enemies(&gs);
    for (int i = 0; i < gs.num_enemies; i++) {
        EXPECT_TRUE(gs.enemies[i].is_alive);
        EXPECT_FALSE(gs.enemies[i].is_stunned);
        EXPECT_GT(gs.enemies[i].hp, 0);
        EXPECT_EQ(gs.enemies[i].hp, gs.enemies[i].max_hp);
        EXPECT_EQ(gs.enemies[i].max_stamina, 150);
        EXPECT_EQ(gs.enemies[i].stamina, 0);
        EXPECT_EQ(gs.enemies[i].damage, SECOND_LAST + 10);
    }
}

TEST(RespawnEnemies, ActionLogUpdated) {
    GameState gs = make_default_gs();
    srand(0);
    respawn_enemies(&gs);
    EXPECT_NE(strstr(gs.action_log, "New wave!"), nullptr);
}

// ═══════════════════════════════════════════════════════════════════
//  Action ring-buffer — push / pop
// ═══════════════════════════════════════════════════════════════════

TEST(ActionBuffer, PushAndPopSingle) {
    GameState gs = make_default_gs();
    push_player_action(&gs, 0, ACTION_STRIKE, 4, -1);
    PlayerInputEvent out{};
    EXPECT_TRUE(pop_player_action(&gs, 0, &out));
    EXPECT_EQ(out.action, ACTION_STRIKE);
    EXPECT_EQ(out.target, 4);
    EXPECT_EQ(out.weapon_id, -1);
    EXPECT_TRUE(out.valid);
}

TEST(ActionBuffer, PopEmptyReturnsFalse) {
    GameState gs = make_default_gs();
    PlayerInputEvent out{};
    EXPECT_FALSE(pop_player_action(&gs, 0, &out));
}

TEST(ActionBuffer, FIFO_Order) {
    GameState gs = make_default_gs();
    push_player_action(&gs, 0, ACTION_STRIKE,  4, -1);
    push_player_action(&gs, 0, ACTION_HEAL,    1, -1);
    push_player_action(&gs, 0, ACTION_SKIP,   -1, -1);

    PlayerInputEvent out{};
    pop_player_action(&gs, 0, &out);
    EXPECT_EQ(out.action, ACTION_STRIKE);
    pop_player_action(&gs, 0, &out);
    EXPECT_EQ(out.action, ACTION_HEAL);
    pop_player_action(&gs, 0, &out);
    EXPECT_EQ(out.action, ACTION_SKIP);
    EXPECT_FALSE(pop_player_action(&gs, 0, &out));
}

TEST(ActionBuffer, FullBufferRejectsPush) {
    GameState gs = make_default_gs();
    for (int i = 0; i < ACTION_BUF_CAP; i++)
        push_player_action(&gs, 0, ACTION_STRIKE, i, -1);

    // Buffer is full — next push should be silently dropped
    push_player_action(&gs, 0, ACTION_HEAL, 99, -1);
    EXPECT_EQ(gs.player_buffers[0].count, ACTION_BUF_CAP);

    // Verify we can pop exactly ACTION_BUF_CAP items, all STRIKE
    PlayerInputEvent out{};
    for (int i = 0; i < ACTION_BUF_CAP; i++) {
        EXPECT_TRUE(pop_player_action(&gs, 0, &out));
        EXPECT_EQ(out.action, ACTION_STRIKE);
        EXPECT_EQ(out.target, i);
    }
    EXPECT_FALSE(pop_player_action(&gs, 0, &out));
}

TEST(ActionBuffer, WrapAround) {
    GameState gs = make_default_gs();
    // Push and pop several times to advance head/tail past boundaries
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < ACTION_BUF_CAP; i++)
            push_player_action(&gs, 0, ACTION_STRIKE, round * 100 + i, -1);
        PlayerInputEvent out{};
        for (int i = 0; i < ACTION_BUF_CAP; i++) {
            EXPECT_TRUE(pop_player_action(&gs, 0, &out));
            EXPECT_EQ(out.target, round * 100 + i);
        }
    }
}

TEST(ActionBuffer, IndependentPerPlayer) {
    GameState gs = make_default_gs();
    gs.num_players = 2;
    init_entity(gs.players[1], 100, 0, 10, 10);

    push_player_action(&gs, 0, ACTION_STRIKE, 4, -1);
    push_player_action(&gs, 1, ACTION_HEAL,   0, -1);

    PlayerInputEvent out{};
    pop_player_action(&gs, 0, &out);
    EXPECT_EQ(out.action, ACTION_STRIKE);
    pop_player_action(&gs, 1, &out);
    EXPECT_EQ(out.action, ACTION_HEAL);
}

// ═══════════════════════════════════════════════════════════════════
//  Enemy AI — enemy_decide
// ═══════════════════════════════════════════════════════════════════

TEST(EnemyDecide, TargetsFirstAlivePlayer) {
    GameState gs = make_default_gs();
    gs.num_players = 3;
    for (int i = 0; i < 3; i++) init_entity(gs.players[i], 100, 0, 10, 10);
    gs.players[0].is_alive = false;

    srand(0); // force rand()%10 < 8 → STRIKE
    enemy_decide(&gs, 0);
    // First alive player is index 1
    EXPECT_EQ(gs.action_target, 1);
    EXPECT_EQ(gs.action_actor, MAX_PLAYERS + 0);
}

TEST(EnemyDecide, StrikeOrSkipOnly) {
    GameState gs = make_default_gs();
    // Run many iterations and verify only STRIKE or SKIP is chosen
    srand(42);
    for (int i = 0; i < 100; i++) {
        gs.pending_action = ACTION_NONE;
        enemy_decide(&gs, 0);
        EXPECT_TRUE(gs.pending_action == ACTION_STRIKE ||
                    gs.pending_action == ACTION_SKIP);
    }
}

TEST(EnemyDecide, NoAlivePlayerDoesNothing) {
    GameState gs = make_default_gs();
    gs.players[0].is_alive = false;
    gs.pending_action = ACTION_NONE;
    enemy_decide(&gs, 0);
    EXPECT_EQ(gs.pending_action, ACTION_NONE); // Unchanged
}

TEST(EnemyDecide, ActionActorSetCorrectly) {
    GameState gs = make_default_gs();
    srand(0);
    enemy_decide(&gs, 2);
    EXPECT_EQ(gs.action_actor, MAX_PLAYERS + 2);
}

TEST(EnemyDecide, ActionLogWritten) {
    GameState gs = make_default_gs();
    memset(gs.action_log, 0, MAX_ACTION_LOG);
    srand(0);
    enemy_decide(&gs, 0);
    EXPECT_GT(strlen(gs.action_log), 0u);
}

// ═══════════════════════════════════════════════════════════════════
//  UI event queue — send_ui_event
// ═══════════════════════════════════════════════════════════════════

TEST(UIEventQueue, PushSingleEvent) {
    GameState gs = make_default_gs();
    send_ui_event(&gs, ACTION_STRIKE, 4, -1, 0);
    EXPECT_EQ(gs.ui_events.count, 1);
    EXPECT_EQ(gs.ui_events.events[0].action, ACTION_STRIKE);
    EXPECT_EQ(gs.ui_events.events[0].target, 4);
    EXPECT_EQ(gs.ui_events.events[0].weapon_id, -1);
    EXPECT_EQ(gs.ui_events.events[0].player_idx, 0);
}

TEST(UIEventQueue, MultipleEvents) {
    GameState gs = make_default_gs();
    send_ui_event(&gs, ACTION_STRIKE,  4, -1, 0);
    send_ui_event(&gs, ACTION_HEAL,    1, -1, 0);
    send_ui_event(&gs, ACTION_EXHAUST, 5, -1, 1);
    EXPECT_EQ(gs.ui_events.count, 3);
    EXPECT_EQ(gs.ui_events.events[0].action, ACTION_STRIKE);
    EXPECT_EQ(gs.ui_events.events[1].action, ACTION_HEAL);
    EXPECT_EQ(gs.ui_events.events[2].action, ACTION_EXHAUST);
}

TEST(UIEventQueue, FullQueueRejectsPush) {
    GameState gs = make_default_gs();
    for (int i = 0; i < UI_EVENT_CAP; i++)
        send_ui_event(&gs, ACTION_STRIKE, i, -1, 0);
    EXPECT_EQ(gs.ui_events.count, UI_EVENT_CAP);

    // Overflow should be silently dropped
    send_ui_event(&gs, ACTION_HEAL, 99, -1, 0);
    EXPECT_EQ(gs.ui_events.count, UI_EVENT_CAP);
    // Last stored event should still be the last STRIKE, not the HEAL
    EXPECT_EQ(gs.ui_events.events[UI_EVENT_CAP - 1].action, ACTION_STRIKE);
}

TEST(UIEventQueue, EmptyByDefault) {
    GameState gs = make_default_gs();
    EXPECT_EQ(gs.ui_events.count, 0);
}

// ═══════════════════════════════════════════════════════════════════
//  Data-structure sizes and layout
// ═══════════════════════════════════════════════════════════════════

TEST(DataStructures, PlayerActionBufferInit) {
    PlayerActionBuffer buf{};
    memset(&buf, 0, sizeof(buf));
    EXPECT_EQ(buf.head, 0);
    EXPECT_EQ(buf.tail, 0);
    EXPECT_EQ(buf.count, 0);
}

TEST(DataStructures, EntityDefaultInventoryEmpty) {
    Entity e;
    init_entity(e, 100, 0, 10, 10);
    for (int i = 0; i < INVENTORY_SIZE; i++)
        EXPECT_EQ(e.inventory[i], WPN_NONE);
    for (int i = 0; i < LONG_TERM_SIZE; i++)
        EXPECT_EQ(e.long_term[i], WPN_NONE);
    EXPECT_EQ(e.lt_count, 0);
}

TEST(DataStructures, WeaponDropStateInit) {
    WeaponDropState wds{};
    memset(&wds, 0, sizeof(wds));
    EXPECT_FALSE(wds.pending);
    EXPECT_EQ(wds.decision, 0);
}
