// SPDX-License-Identifier: MIT
#include <cstdlib>
#include <chrono>
#include "netplay.h"
#include "netplay_session.hpp"

#include <string>

using namespace ssb64net;

static Session& S() {
    static Session s;
    return s;
}
static std::string sAbortReason;

extern "C" {

int np_host(uint16_t port, const char *name) { return S().Host(port, name) ? 1 : 0; }
int np_join(const char *host, uint16_t port, const char *name) { return S().Join(host, port, name) ? 1 : 0; }
void np_stop(void) { S().Stop(); }
void np_poll(void) { S().Poll(); }

int np_state(void) {
    switch (S().GetState()) {
        case State::Hosting: return NP_HOSTING;
        case State::Joining: return NP_JOINING;
        case State::Connected: return NP_CONNECTED;
        case State::Failed: return NP_FAILED;
        default: return NP_IDLE;
    }
}
int np_is_host(void) { return S().IsHost() ? 1 : 0; }
int np_local_slot(void) { return S().LocalSlot(); }
const char *np_status(void) { return S().Status(); }
int np_slot_used(int slot) { return (slot >= 0 && slot < kMaxSlots && S().Roster(slot).used) ? 1 : 0; }
const char *np_slot_name(int slot) { return (slot >= 0 && slot < kMaxSlots) ? S().Roster(slot).name : ""; }
void np_slot_choice(int slot, int *f, int *c) {
    if (slot < 0 || slot >= kMaxSlots) return;
    if (f) *f = S().Roster(slot).fighter;
    if (c) *c = S().Roster(slot).costume;
}
int np_player_count(void) { return S().ConnectedCount(); }
void np_set_choice(int f, int c) { S().SetChoice(f, c); }
void np_set_name(const char *n) { S().SetLocalName(n); }
void np_set_input_delay(int f) { S().SetInputDelay(f); }
int np_get_input_delay(void) { return S().InputDelay(); }
void np_set_mode(int m) { S().SetMode(m); }
int np_get_mode(void) { return S().Mode(); }
void np_set_max_players(int n) { S().SetMaxPlayers(n); }
int np_get_max_players(void) { return S().MaxPlayers(); }

static int sRunDifficulty = 2; // Normal
static int sRunStocks = 2;     // 3 lives
void np_set_run_options(int difficulty, int stock_count) {
    sRunDifficulty = difficulty < 0 ? 0 : (difficulty > 4 ? 4 : difficulty);
    sRunStocks = stock_count < 0 ? 0 : (stock_count > 4 ? 4 : stock_count);
}
int np_get_run_difficulty(void) { return sRunDifficulty; }
int np_get_run_stocks(void) { return sRunStocks; }
static int sFriendlyFire = 0;
void np_set_friendly_fire(int on) { sFriendlyFire = on ? 1 : 0; }
int np_get_friendly_fire(void) { return sFriendlyFire; }
static int sHostCostume = -1;
void np_set_host_costume(int c) { sHostCostume = (c < 0) ? -1 : (c & 3); }
int np_get_host_costume(void) { return sHostCostume; }
void np_set_room_cheats(int m) { S().SetRoomCheats(m); }
int np_get_room_cheats(void) { return S().RoomCheats(); }
int np_match_cheats(void) { return S().MatchCheats(); }
static int sStage = -1; // -1 = chosen on the stage select screen, -2 = random, >=0 = GRKind
void np_set_stage(int k) { sStage = k; }
int np_get_stage(void) { return sStage; }
int np_resolve_stage(void) {
    if (sStage == -2) return std::rand() % 9; // Castle..Mushroom Kingdom (host only: the result is sent to everybody)
    return sStage;
}
static int sPanels = 1;
void np_set_panels(int on) { sPanels = on ? 1 : 0; }
int np_get_panels(void) { return sPanels; }

static uint32_t LocalSettingsBits();
int np_host_begin_match(const void *b, int n) {
    S().SetRoomSettings(LocalSettingsBits()); // the host's settings win for everybody during the match
    return S().HostBeginMatch(b, n) ? 1 : 0;
}
int np_client_take_match(void *out, int cap) { return S().ClientTakeMatch(out, cap); }
int np_match_active(void) { return S().MatchActive() ? 1 : 0; }
int np_match_mask(void) { return S().MatchMask(); }
int np_match_delay(void) { return S().MatchDelay(); }
void np_ready(void) { S().Ready(); }
int np_start_released(void) { return S().StartReleased() ? 1 : 0; }
extern "C" {
int port_get_comp_ruleset(void);
int port_enhancement_neutral_spawns(void);
int port_enhancement_cpu_level_9(void);
int port_enhancement_bonus_stages_enabled(void);
int port_enhancement_tap_jump_disabled(int player_index);
int port_cheat_unlock_all_active(void);
}

// Simulation-affecting settings, packed: bit0 comp ruleset, 1 neutral spawns, 2 CPU lvl 9, 3 bonus stages, 4-7 tap jump off (P1-P4).
// During an online match every peer uses the host's bits instead of its own CVars (np_setting_override), so a
// different local setting can no longer cause a desync.
static uint32_t LocalSettingsBits() {
    uint32_t b = 0;
    if (port_get_comp_ruleset()) b |= 1u << 0;
    if (port_enhancement_neutral_spawns()) b |= 1u << 1;
    if (port_enhancement_cpu_level_9()) b |= 1u << 2;
    if (port_enhancement_bonus_stages_enabled()) b |= 1u << 3;
    for (int i = 0; i < 4; i++) if (port_enhancement_tap_jump_disabled(i)) b |= 1u << (4 + i);
    return b;
}
int np_setting_override(int bit, int local) {
    if (!S().SettingsActive()) return local;
    return (S().MatchSettings() >> bit) & 1u;
}

uint32_t np_settings_hash(void) {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) { h ^= v; h *= 16777619u; };
    mix((uint32_t)port_get_comp_ruleset());
    mix((uint32_t)port_enhancement_neutral_spawns());
    mix((uint32_t)port_enhancement_cpu_level_9());
    mix((uint32_t)port_enhancement_bonus_stages_enabled());
    for (int i = 0; i < 4; i++) mix((uint32_t)port_enhancement_tap_jump_disabled(i));
    return h;
}

void np_end_match(void) { S().EndMatch(); }
void np_submit_sync(uint32_t tick, uint32_t hash) { S().SubmitSync(tick, hash); }
static std::string sLastAbort;
static std::chrono::steady_clock::time_point sLastAbortAt;
int np_consume_abort(void) {
    if (!S().ConsumeAbort(&sAbortReason)) return 0;
    sLastAbort = sAbortReason;
    sLastAbortAt = std::chrono::steady_clock::now();
    return 1;
}
const char *np_last_abort_reason(void) { return sLastAbort.c_str(); }
int np_last_abort_age_ms(void) {
    if (sLastAbort.empty()) return -1;
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sLastAbortAt).count();
}
const char *np_abort_reason(void) { return sAbortReason.c_str(); }

void np_put_input(int slot, uint32_t tick, uint16_t b, int8_t sx, int8_t sy) {
    InputFrame f; f.buttons = b; f.stickX = sx; f.stickY = sy;
    S().PutInput(slot, tick, f);
}
int np_get_input(int slot, uint32_t tick, uint16_t *b, int8_t *sx, int8_t *sy) {
    InputFrame f;
    if (!S().GetInput(slot, tick, &f)) return 0;
    if (b) *b = f.buttons;
    if (sx) *sx = f.stickX;
    if (sy) *sy = f.stickY;
    return 1;
}

} // extern "C"
