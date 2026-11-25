// u2mp: the session layer's party, as other parts of the module see it (session.cpp implements it).
#pragma once
#include <cstdint>
#include <string>

namespace u2mp::session {

// The party member shown in roster row `row` (the order of the PARTY_PLAYERn rows: the host first, then the
// members on the host's team, then the others, each in slot order).
bool member_at(int row, uint32_t &net_id, std::string &name);
// The player shown in matchmaking row `row` (PLAYERn, cond 6..15); false for an open or "Found Player" slot.
bool mm_member_at(int row, uint32_t &net_id, std::string &name);
// The member with this net id (host 1, a client in transport slot s has s + 1).
bool member_name(uint32_t net_id, std::string &name);
// The member's chosen hero and villain actor packages; false when the member is unknown.
bool member_skins(uint32_t net_id, std::string &hero, std::string &villain);
// Registers/loads an actor package by name; NULL when the game cannot create it.
uint8_t *request_package(const char *name);
// This PC's net id; 0 when it is not in a party.
uint32_t local_id();
// The online layer saw the client enter NET_SM 35; the session returns it to the Multiplayer page when
// the game then jumps to state 3 without a frame in 35.
void note_leaving();
// True when this PC leads the party (it hosts the transport).
bool is_host();
// Host only (Kick From Party, PS3 0x439cf8 with the block flag): tells the member with this net id that it
// was kicked and refuses its online id on later joins until this PC stops hosting (the transport closes).
// It leaves the party, and the host drops it (NetEventPlayerLeft, roster) when its goodbye arrives. False
// when not hosting, for the host itself, or for an unknown member.
bool kick(uint32_t net_id);
// Host only, for NetEventKickGriefer: kick(). Naming this PC itself makes it leave the session, as PS3 does.
// False when not hosting or for an unknown member.
bool kick_griefer(uint32_t net_id);
// This PC's player name (UNCHARTED_U2MP_NAME, else "Drake").
std::string local_name();
int rank();   // this PC's rank badge index (profile panel)
// Players in the party, this PC included; 0 when not in one.
int member_count();

}  // namespace u2mp::session
