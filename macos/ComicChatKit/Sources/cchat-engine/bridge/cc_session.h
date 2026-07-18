// cc_session.h — internal to the bridge; not part of the public API.
#ifndef CC_SESSION_H
#define CC_SESSION_H
#include "comicchat.h"
#include "ircproto.h"
#include <string>
#include <vector>

// --- room-token <-> channel-name mapping (Plan 3 Task 4) --------------------
// The outbound C functions (cc_session_join/part/send_say/...) take a
// `room_token` (a uint32_t), not a channel-name string -- callers resolve a
// channel name to a token once (on join), then refer to the room by token
// thereafter. Scheme (documented per the task brief's requirement to decide
// AND document one): `CCSession::channels` is a session-held
// std::vector<std::string> indexed by token; token 0 is reserved/invalid
// (CC_ROOM_TOKEN_NONE) so a default-zeroed token is never confused with a
// real room, and tokens are otherwise assigned densely starting at 1 in
// registration order (cc_session_register_room below, called by Swift on a
// join-confirm once Task 7 wires the real inbound join flow; this task's
// selftest calls it directly to seed the table). No token is ever reused
// within a session's lifetime (a part/leave does not shrink the vector or
// recycle its slot) -- simpler and sufficient for this task's scope; Task 7
// can add token invalidation/reuse bookkeeping if session-lifetime memory
// growth ever matters in practice (channel names are short strings, and
// real chat sessions join at most a handful of rooms).
//
// CIrcProto per-room state (m_strChannel/m_strPassword/m_strTopic/m_dwModes/
// m_dwMaxUsers) is NOT yet multiplexed per-token -- this task's CCSession
// holds exactly ONE CIrcProto (`proto`, shared for every room, matching the
// brief's literal Step 5 instruction), and each outbound C function sets
// `proto.m_strChannel` from the token-resolved channel name immediately
// before delegating to the matching CIrcProto method. This is correct for
// this task's single-room selftest scope (byte-compare against one room at a
// time) but is a documented simplification: a real multi-room session needs
// either N CIrcProto instances (one per joined room) or the per-room fields
// hoisted onto the channel-table entries directly. Left for Task 7, which
// wires the real join/part lifecycle and needs to solve this properly
// alongside inbound per-room reply routing.
#define CC_ROOM_TOKEN_NONE 0u

struct CCSession {
    cc_session_config cfg{};
    std::string       inbuf;      // accumulates fed bytes; line-framed in Task 3

    CIrcSocket        sock;       // outbound-scoped subset (ircproto.h)
    CIrcProto         proto;      // see room-token mapping note above
    std::vector<std::string> channels;  // index 0 unused (CC_ROOM_TOKEN_NONE)

    CCSession() {
        channels.push_back("");   // token 0 == CC_ROOM_TOKEN_NONE, reserved
        proto.m_pSock = &sock;
    }
};

// The active session for the current single-threaded call (set on entry to any
// cc_session_* that runs lifted code). Lifted code reaches callbacks/resolvers
// through this, never through theApp. ASSERTs if no session is active.
CCSession* ccSession();

// R18 dispatcher: fill a cc_proto_event and invoke cfg.on_event. Defined in
// cc_session.cpp; declared here so lifted files can call it.
void ccEmitProtoEvent(const struct cc_proto_event* ev);

// --- R19 resolvers (Plan 3 Task 6) ------------------------------------------
// ccSessionOwnNick(): replaces every lifted GetMyNickName() call. Reads
// ccSession()->cfg.own_nick(user_data); "" (never NULL) if the config didn't
// supply one, matching cc_own_nick_fn's documented contract.
const char* ccSessionOwnNick();

// ccSessionResolveUser(): replaces every lifted LookupPui(nick, doc) call in
// the payload-stage codec path (protsupp.cpp's ProcessSay/GetTalkTos/
// IdentifyWhispers). Reads ccSession()->cfg.resolve_user(user_data, nick,
// room_token); returns CC_USER_REF_NONE (0) if the config didn't supply a
// resolver -- callers must treat 0 as "unknown user", never dereference it.
cc_user_ref ccSessionResolveUser(const char* nick, uint32_t room_token);

// Test-only activation hook (Plan 3 Task 5a): every public cc_session_* entry
// point activates g_session on entry and clears it on exit (see
// cc_session.cpp), but ccEmitProtoEvent has no public entry point of its own
// yet (Task 5b's parser will call it from inside an already-activated
// context). The event-union completeness selftest calls ccEmitProtoEvent
// directly, so it needs the same activate/deactivate bracketing without a
// parse to hang it on. no-op if h is NULL, same convention as the public
// cc_session_* entry points.
void ccActivateSessionForTest(cc_session* h);
void ccDeactivateSessionForTest();

#endif
