#include "cc_session.h"

#include "comicchat.h"     // cc_proto_event (real union + enum, Plan 3 Task 5a)
#include "defines.h"       // BM_*/CGESTUREPREFIX/CEXPRESSIONPREFIX/CMODEPREFIX (Plan 3 Task 4)
#include "protsupp.h"      // IndexToByte (Plan 3 Task 4 outbound wiring)
#include <cassert>
#include <cstring>
#include <cstdio>

static CCSession* g_session = nullptr;   // single-threaded: one active session
CCSession* ccSession() { assert(g_session && "no active cc_session"); return g_session; }

void ccEmitProtoEvent(const cc_proto_event* ev) {
    CCSession* s = ccSession();
    if (s->cfg.on_event) s->cfg.on_event(s->cfg.user_data, ev);
}

void ccActivateSessionForTest(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return;
    g_session = s;
}
void ccDeactivateSessionForTest() {
    g_session = nullptr;
}

cc_session* cc_session_create(const cc_session_config* cfg) {
    if (!cfg || !cfg->send) return nullptr;
    CCSession* s = new CCSession();
    s->cfg = *cfg;
    return reinterpret_cast<cc_session*>(s);
}
void cc_session_destroy(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (g_session == s) g_session = nullptr;
    delete s;
}
void cc_session_feed_bytes(cc_session* h, const uint8_t* d, size_t n) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return;
    g_session = s;                       // activate for lifted code (Task 3+)
    s->inbuf.append(reinterpret_cast<const char*>(d), n);
    // Task 3 replaces this with the lifted line-framer + parse dispatch.
    g_session = nullptr;
}
void cc_session_fire_timer(cc_session* h, int32_t) {
    if (!h) return;
    /* Task 4 */
}

void cc_session_test_echo(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return;
    g_session = s;
    if (s->cfg.send) s->cfg.send(s->cfg.user_data,
                                 reinterpret_cast<const uint8_t*>("ECHO\r\n"), 6);
    cc_proto_event ev; std::memset(&ev, 0, sizeof(ev));  // type 0 placeholder
    ccEmitProtoEvent(&ev);
    g_session = nullptr;
}

// ============================================================================
// Plan 3 Task 4: outbound command builders. Thin C wrappers over ircproto.h's
// CIrcProto methods; every one activates the session (g_session, so
// ccSessionSendRaw below can find its way back to cfg.send), resolves the
// room_token to a channel name, sets proto.m_strChannel, then delegates.

// --- ccSessionSendRaw: the single outbound choke point (R19) ---------------
// Declared in engine/ircproto.h; CIrcProto::SendMessageText calls this
// instead of a CAsyncSocket. Reaches ccSession()->cfg.send exactly like every
// other engine->bridge crossing in this port (ccEmitProtoEvent above is the
// same pattern for the inbound-event direction).
void ccSessionSendRaw(const char* data, size_t len) {
    CCSession* s = ccSession();
    if (s->cfg.send) s->cfg.send(s->cfg.user_data, reinterpret_cast<const uint8_t*>(data), len);
}

// --- PFNGETOWNIDENTITY resolver: cfg.own_nick is the only own-identity hook
// this session config exposes (Task 1). own_user/ident_length have no
// session-config counterpart yet (own_user was never part of Task 1's
// cc_session_config -- only own_nick) -- both stand in with the same
// "unknown" sentinels bChatSendToTarget's original theApp fallback path used
// (empty user string, ident_length 0 forces the "+32 unknown-hostname"
// branch, matching the original's own behavior when
// theApp.m_nMyIdentLength was not yet set). Task 7 (once the session tracks
// real login/ident state) can extend cc_session_config with own_user/
// ident_length fields and wire them through here for byte-exact receiver-
// side prefix accounting; until then this is the honest "best information
// currently available" resolver.
static cc_own_identity ccSessionGetOwnIdentity() {
    CCSession* s = ccSession();
    cc_own_identity id;
    id.own_nick = s->cfg.own_nick ? s->cfg.own_nick(s->cfg.user_data) : "";
    if (!id.own_nick) id.own_nick = "";
    id.own_user = "";
    id.ident_length = 0;
    return id;
}

// --- PFNISJOINEDCHANNEL resolver: "is this encoded channel name one of the
// rooms this session has registered a token for" -- a linear scan of the
// (small, at most a handful of rooms) channel table. Good enough for this
// task's scope; Task 7 can index this properly if it ever matters.
static BOOL ccSessionIsJoinedChannel(const char* encodedChannel) {
    CCSession* s = ccSession();
    for (size_t i = 1; i < s->channels.size(); i++)
        if (s->channels[i] == encodedChannel) return TRUE;
    return FALSE;
}

uint32_t cc_session_register_room(cc_session* h, const char* channel) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !channel) return CC_ROOM_TOKEN_NONE;
    s->channels.push_back(channel);
    return (uint32_t)(s->channels.size() - 1);
}

const char* cc_session_room_channel(cc_session* h, uint32_t room_token) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || room_token == CC_ROOM_TOKEN_NONE || room_token >= s->channels.size()) return "";
    return s->channels[room_token].c_str();
}

// Sets proto.m_strChannel from room_token, returns FALSE if the token is
// unresolvable (caller should then fail the whole outbound call). Every
// per-room outbound C function below calls this first.
static BOOL ccSessionSelectRoom(CCSession* s, uint32_t room_token) {
    if (room_token == CC_ROOM_TOKEN_NONE || room_token >= s->channels.size()) return FALSE;
    s->proto.m_strChannel = s->channels[room_token].c_str();
    return TRUE;
}

int32_t cc_session_join(cc_session* h, const char* channel, const char* key) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !channel) return 1;
    g_session = s;
    s->proto.ChatJoinAux(channel, key);
    g_session = nullptr;
    return 0;
}

int32_t cc_session_create_room(cc_session* h, const char* channel, const char* creation_modes, uint32_t max_users, const char* key) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !channel) return 1;
    g_session = s;
    s->proto.ChatCreateAux(channel, creation_modes, (DWORD)max_users, key);
    g_session = nullptr;
    return 0;
}

int32_t cc_session_part(cc_session* h, uint32_t room_token, const char* /*reason*/) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        // ChatPartChannel doesn't take a reason in this port's lift (the
        // original PART builder never sent one either -- ircproto.cpp:772
        // "PART %s\r\n" has no reason field; only some IRC servers even
        // accept a PART reason and the original never sent one).
        //
        // DEVIATION (flagged for Task 5b/7): m_bInRoom is normally set TRUE
        // by the inbound JOIN-confirm handler (not lifted this task -- no
        // parser yet) and FALSE by the inbound PART/KICK confirm. Since this
        // task has no inbound flow to set it, cc_session_part force-sets it
        // TRUE immediately before every call so the PART wire command is
        // never silently swallowed (ChatPartChannel's `if (m_bInRoom)` guard
        // exists so a stale/already-parted CIrcProto doesn't re-send PART).
        // This means cc_session_part currently ALWAYS sends the wire command,
        // even if the caller already parted or never joined that room_token
        // for real (a token registered via cc_session_register_room is not
        // proof the server confirmed the join) -- correct enough for this
        // task's outbound-only byte-compare scope, but Task 7 (once
        // inbound join/part confirms exist) should let the real m_bInRoom
        // state drive this instead of overwriting it here.
        s->proto.m_bInRoom = TRUE;
        s->proto.ChatPartChannel();
        rc = 0;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_change_nick(cc_session* h, const char* new_nick) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !new_nick) return 1;
    g_session = s;
    BOOL ok = s->proto.ChatChangeNick(new_nick);
    g_session = nullptr;
    return ok ? 0 : 1;
}

int32_t cc_session_set_topic(cc_session* h, uint32_t room_token, const char* topic) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !topic) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        BOOL ok = s->proto.ChatSetTopic(topic);
        rc = ok ? 0 : 1;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_kick(cc_session* h, uint32_t room_token, const char* nickname, const char* reason) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !nickname) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        BOOL ok = s->proto.ChatKickUser(nickname, reason);
        rc = ok ? 0 : 1;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_invite(cc_session* h, uint32_t room_token, const char* nickname) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !nickname) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        BOOL ok = s->proto.ChatSendInvitation(nickname);
        rc = ok ? 0 : 1;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_ban(cc_session* h, uint32_t room_token, const char* ban_pattern, int32_t ban) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !ban_pattern) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        BOOL ok = s->proto.ChatBanUser(ban_pattern, ban ? TRUE : FALSE);
        rc = ok ? 0 : 1;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_set_mode(cc_session* h, uint32_t room_token, uint32_t new_mode, uint32_t new_max_users, const char* new_password) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        BOOL ok = s->proto.ChatSetMode((DWORD)new_mode, (DWORD)new_max_users, new_password ? new_password : "");
        rc = ok ? 0 : 1;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_away(cc_session* h, int32_t is_away, const char* message) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return 1;
    g_session = s;
    s->proto.ChatSetAway(is_away ? TRUE : FALSE, message ? message : "");
    g_session = nullptr;
    return 0;
}

int32_t cc_session_who(cc_session* h, const char* mask) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return 1;
    g_session = s;
    BOOL ok = s->proto.bExecuteQuery(qpUserListDlg, ctWho, dtMax, nullptr, "", mask ? mask : "");
    g_session = nullptr;
    return ok ? 0 : 1;
}

int32_t cc_session_list(cc_session* h, const char* query) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return 1;
    g_session = s;
    // ChatFillRoomList's core (R20-dropped whole function, ircproto-map.md
    // §6 site #5): decide ctList vs ctListX based on whether an explicit
    // LIST/LISTX command string was supplied, else default to ctListX on an
    // IRCX server. `query`, if non-NULL, is a bare channel-name filter (the
    // original's szParam, extracted from the dialog's persisted "LIST foo"/
    // "LISTX foo" string) -- this C boundary skips that string-command-
    // parsing layer entirely per R20 and takes the filter directly.
    enumCommandType ct = s->sock.m_bIrcXServer ? ctListX : ctList;
    BOOL ok = s->proto.bExecuteQuery(qpRoomListDlg, ct, dtMax, nullptr, query ? query : "", "");
    g_session = nullptr;
    return ok ? 0 : 1;
}

int32_t cc_session_probe_ircx(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s) return 1;
    g_session = s;
    BOOL ok = s->proto.bExecuteQuery(qpIsIrcX, ctModeIsIrcX, dtMax, nullptr, "", "");
    if (ok && s->cfg.set_timer) {
        s->cfg.set_timer(s->cfg.user_data, CC_TIMER_ISIRCX_PROBE, 50000);
    }
    g_session = nullptr;
    return ok ? 0 : 1;
}

// --- annotation-block encoder (deviation from bInsertAnnotations; see
// comicchat.h's cc_session_send_say doc comment) ----------------------------
// Encodes a decoded cc_annotations struct directly into the
// "#G<gp><ge><gi>E<ep><ee><ei>[R]M<m>[T<nick>,...]" wire grammar
// (protsupp.cpp:335-350's sprintf format, reproduced byte-for-byte) without
// reconstructing a CAvatarX/CUserInfo pair to feed the lifted
// bInsertAnnotations -- the C boundary already carries the exact decoded
// index/emotion/intensity values bInsertAnnotations would have read off
// those objects, so re-deriving them from fake objects would be pure
// indirection, not fidelity. IndexToByte (protsupp.h, lifted verbatim Task 3)
// packs each value byte identically to the original.
static void ccEncodeAnnotations(const cc_annotations* ann, USHORT uModes, BOOL bIncludeParenthesis, char* out, size_t outSize) {
    out[0] = '\0';
    if (!ann) return;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s#%c%c%c%c%c%c%c%c%s%c%c",
                      bIncludeParenthesis ? "(" : "",
                      CGESTUREPREFIX,
                      (char)IndexToByte((BYTE)ann->gesture_pose),
                      (char)IndexToByte((BYTE)ann->gesture_emotion),
                      (char)IndexToByte((BYTE)ann->gesture_intensity),
                      CEXPRESSIONPREFIX,
                      (char)IndexToByte((BYTE)ann->face_pose),
                      (char)IndexToByte((BYTE)ann->face_emotion),
                      (char)IndexToByte((BYTE)ann->face_intensity),
                      ann->requested ? "R" : "",
                      CMODEPREFIX,
                      (char)IndexToByte((BYTE)ann->mode));
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;

    if (ann->addressee_count > 0) {
        // T<nick>[,<nick>...] suffix -- built directly from the already-
        // decoded addressees array (see comicchat.h doc comment: skips
        // GetAddressees/GetWhisperedAddressees's CUserInfo-table walk, same
        // "already have the values" reasoning as above). Clipped to
        // CC_MAX_ADDRESSEES by the struct's own array bound, matching the
        // original's min(GetUpperBound(),4) clip-at-5 (protsupp.cpp:3009).
        int cap = ann->addressee_count;
        if (cap > CC_MAX_ADDRESSEES) cap = CC_MAX_ADDRESSEES;
        int w = snprintf(buf + n, sizeof(buf) - (size_t)n, "T");
        n += (w > 0) ? w : 0;
        for (int i = 0; i < cap && (size_t)n < sizeof(buf) - 1; i++) {
            w = snprintf(buf + n, sizeof(buf) - (size_t)n, "%s%s", i ? "," : "", ann->addressees[i]);
            n += (w > 0) ? w : 0;
        }
    }
    if (bIncludeParenthesis) {
        int w = snprintf(buf + n, sizeof(buf) - (size_t)n, ") ");
        n += (w > 0) ? w : 0;
    }
    strncpy(out, buf, outSize - 1);
    out[outSize - 1] = '\0';
    (void)uModes;  // uModes only selects addressee-source (T-group has none here); kept for signature symmetry with bInsertAnnotations
}

int32_t cc_session_send_say(cc_session* h, uint32_t room_token, const cc_annotations* ann, const char* text, uint16_t modes) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !text) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        // Plain-IRC transport (parenthesized annotation blob prefixed to the
        // text) vs IRCX out-of-band DATA line is decided INSIDE
        // bChatSendToTarget by IsIRCX() -- bIncludeParenthesis here mirrors
        // that same test so the encoded annotation string matches whichever
        // transport bChatSendToTarget is about to pick (protsupp.cpp's own
        // ProcessSlashCommand callers make this same IsIRCX()-gated choice
        // before calling bInsertAnnotations).
        char annBuf[256];
        ccEncodeAnnotations(ann, (USHORT)modes, !s->proto.IsIRCX(), annBuf, sizeof(annBuf));
        BOOL ok = s->proto.bChatSendToChannel(annBuf[0] ? annBuf : nullptr, text, nullptr, (USHORT)modes, ccSessionGetOwnIdentity);
        rc = ok ? 0 : 1;
    }
    g_session = nullptr;
    return rc;
}

int32_t cc_session_send_whisper(cc_session* h, uint32_t room_token, const cc_annotations* ann, const char* text, const char* const* nicks, int32_t nick_count) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    if (!s || !text || !nicks || nick_count <= 0) return 1;
    g_session = s;
    int32_t rc = 1;
    if (ccSessionSelectRoom(s, room_token)) {
        char annBuf[256];
        ccEncodeAnnotations(ann, (USHORT)BM_WHISPER, !s->proto.IsIRCX(), annBuf, sizeof(annBuf));
        rc = 0;
        for (int32_t i = 0; i < nick_count; i++) {
            if (!nicks[i]) continue;
            BOOL ok = s->proto.bChatSendPrivMesg(nicks[i], annBuf[0] ? annBuf : nullptr, text, nullptr, FALSE, BM_WHISPER, ccSessionGetOwnIdentity);
            if (!ok) rc = 1;
        }
    }
    g_session = nullptr;
    return rc;
}
