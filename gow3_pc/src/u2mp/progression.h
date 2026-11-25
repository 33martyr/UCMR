// u2mp progression: NetPlayerData persistence, local leaderboards. See progression.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace u2mp {

// Installs the progression hooks (byte-checked; each one is skipped when its bytes differ).
void install_progression(int &installed);

// Writes the multiplayer progression to the local store if it changed since the last write.
// For the session layer to call when the player leaves Multiplayer (before NET_SM drops to 0, after
// which the game clears the in-memory copy, as PS3 GetVolatileData 0x4c4f74 did).
void progression_publish();
// One career publish (progression.md 1.5.4: counter +1, store written) when NetPlayerData is connected, changed or not.
void progression_publish_career();
// Adds the leaderboard page's "shown" handler to the menu page registry (once; spec progression.md 1.5.19). For the
// session layer to call with its own page handler, before PAGE_MULTIPLAYER can open the leaderboards.
void progression_register_leaderboard_page();

// The Players Met rows (local store, spec parity-g-friends.md G1): players added at a match start, no friend,
// most recently seen first, at most 50. Online it is empty until the RPCN friend list is known.
std::vector<std::string> progression_players_met();
// The local player's career headshots (local store; the friends ticker's Headshots and presence version 3).
uint32_t progression_headshots();
std::string progression_player_summary(const std::string &name);   // Players Met: View Profile
// Match and round start (PS3 Playing::Enter 0x444660, spec parity-e-flow2.md 2.9, parity-g-friends.md G1): the
// remote players that are no friend and no Players Met row yet go into Players Met at once (at most 16) and the
// store is written; the match statistics still come at Leave.
void progression_note_met(const std::vector<std::string> &names);

// clantag.cpp: BUTTON_CLANTAG's text box (NetPlayerData +0x1a8).
bool edit_clan_tag();
// Per frame of the profile page: takes the clan tag once the text input closes.
void clan_tag_frame();
// Every game frame (online::frame): while NET_SM > 1, checks a new clan tag and publishes it after the verdict
// (PS3 parity A 2.1).
void clan_tag_check_frame();

}  // namespace u2mp
