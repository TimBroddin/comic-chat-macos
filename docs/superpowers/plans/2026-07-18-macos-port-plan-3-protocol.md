# macOS Port — Plan 3: Protocol (IRC / MS Chat + annotations) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lift the original client's IRC/IRCX parse + build logic and the comic-annotation codec into the headless `cchat-engine`, exposing a "bytes in, typed events out / outbound bytes + one timer request out" C boundary that Swift drives over `NWConnection`, so a real MS Chat / IRC session round-trips through the same engine that already renders the comic strip.

**Architecture:** Swift owns the socket and the event loop (spec §4.4). The engine never opens a socket. A new opaque `cc_session` handle receives inbound bytes through `cc_session_feed_bytes()`, runs the lifted line-framer + parser, and emits (a) outbound byte buffers via a `send` callback, (b) typed protocol events via a `cc_proto_events` callback vtable (the same function-pointer-vtable idiom as the existing `cc_canvas`), and (c) at most one one-shot timer request via a `timer` callback. **Critical discovery correction (see the four reports in `docs/superpowers/plans/2026-07-18-plan3-discovery/`): the parser lives in `ircsock.cpp`, NOT `ircproto.cpp`; `CRoomInfo` (chatprot.h) is an OUTBOUND-only command seam with no inbound event interface — the inbound event vocabulary is designed fresh in this plan from the ~30 free-function callees the parse handlers invoke.** Session/user/room state that the original kept in MFC `CChatDoc`/`theApp`/ListView is **tracked on the Swift side**; the engine holds only what parsing needs and calls back through a small resolver for the one lookup it can't avoid (nick → opaque user handle, mirroring `LookupPui`).

**Tech Stack:** C++ (lifted, minimally edited per Edit Rules) in target `cchat-engine`; pure-C boundary in `comicchat.h`; Swift (`ComicChatKit`, `Network.framework`) for the socket, event stream, and session-state model; C++ selftest harness (`cc_run_selftests`/`CC_CHECK`) for engine-side tests; Swift Testing for bridge-and-up. Bun capture rig (`.superpowers/rig/`) supplies real captured byte streams for replay tests.

## Global Constraints

Copied from the spec (`docs/superpowers/specs/2026-07-17-macos-port-design.md`) and the standing engine contracts; every task's requirements implicitly include these.

- **Lifted files keep their original names and as much original code as possible; every deviation is attributable to a numbered Edit Rule.** Edit Rules R1–R13 (Plan 1 doc §Edit Rules) + R14–R17 (Plan 2 doc §Edit Rules, incl. amendments R14(v), R15 instance 2) are carried forward **verbatim and binding**. This plan adds R18–R21 (below). Never fork the table; never improvise — escalate (BLOCKED/NEEDS_CONTEXT) and turn the ruling into an amendment.
- **The engine never opens a socket** (spec §4.4). All I/O crosses the C boundary as byte buffers. Timers are *requested* by the engine and *scheduled* by Swift.
- **Permissive protocol handling is preserved** (spec §7): added strictness would break interop. Unknown verbs/numerics keep their original TRACE-or-status-drop behavior; the tolerant low-level-unquoting heuristic is reproduced exactly.
- **Wire text stays bytes end-to-end inside the engine** (spec §4.5). CP-1252 is the default; the engine does not transcode. Swift converts for display (CP-1252 default, UTF-8 per-connection). JIS/SJIS units (`jis2sjis.cpp`/`sjis2jis.cpp`) are **not** ported.
- **`v2.5-beta-1-modern/` and `artifacts*/inc/` and `artifacts/core/` are read-only lift sources — never edit them.** Copy into `Sources/cchat-engine/engine/` (or the artifacts-core string half into `engine/`) and edit the copy per the rules.
- **Engine is single-threaded with process-global mutable state and no internal locking** (documented in `comicchat.h`). All `cc_*` calls originate from one thread at a time. `cc_session_*` inherits this contract. `srand(0x5EED)` determinism (set in `cc_strip_create`) is engine-owned; protocol-driven layout inherits it.
- **The recording-canvas log grammar is a stable test contract** (`cc_recording_canvas.h`). Protocol work does not touch it (annotation rendering reuses the existing `cc_strip` compose path).
- **The C bridge never throws across the boundary** (spec §7). Engine errors surface as status codes plus error events. `ASSERT` logs in release, traps in debug.
- Engine Swift module name is `cchat_engine` (hyphen → underscore). `swift test` from `macos/ComicChatKit/` is the truth; SourceKit "No such module" diagnostics are false. Test files resolve the repo root with **FIVE** `deletingLastPathComponent()` calls from `#filePath`.
- New global-state test suites nest inside the existing `.serialized` ancestor suite `EngineGlobalStateSelfTests` (the serialized-suite pattern) — protocol tests touch engine-global session state and will race otherwise.
- Commits are SSH-signed via 1Password; if `ssh-add -l` shows no identities, **ask Tim to unlock — never bypass signing without explicit authorization**. When authorized, `git commit --no-gpg-sign` and say so in the record.

---

## Edit Rules added by this plan (R18–R21)

Append to the authoritative table. Each is introduced at the task noted; extend further only by amendment.

| # | Pattern in original | Required action |
|---|---|---|
| R18 | Inbound parse handlers in lifted code call an **app-layer free function directly** (`OnTextMsg`, `OnDataMsg`, `AddAndExecute(new JoinEntry…)`, `AddToStatus`, `OnKick`, `OnInvite`, …) — the "events out" surface the original never abstracted (discovery: `CRoomInfo` is outbound-only) | Replace the direct call with a single typed **event emission** through `ccEmitProtoEvent(&ev)` (a new thin dispatcher in `bridge/cc_session.cpp` that fills a `cc_proto_event` union and invokes the registered `cc_proto_events` callback). The lifted handler keeps its parsing/extraction logic verbatim up to the emit point; only the terminal side-effecting call is replaced. Each replaced call site is individually listed in the task report with the original callee and the event type it maps to. History-entry side effects (`AddAndExecute`) that exist only to update MFC doc/ListView state become the corresponding event with **no** engine-side state write. |
| R19 | Lifted code needs a datum the headless engine does not own because Swift tracks it (own nick, a nick→user resolution, per-room membership, connection status) | Reroute through a **resolver callback** on `ccSession()` (extending the session context minimally, R9-style, each addition selftested), never through `theApp`/`GetChatDoc`. The two resolvers this plan needs: `ccSessionOwnNick()` (replaces `GetMyNickName()`; the Plan-2 `cc_link_stubs.cpp` stub is deleted) and `ccSessionResolveUser(nick, room_token)` returning an opaque `cc_user_ref` (replaces `LookupPui`; used only to rebuild `m_udi.m_talkTos` for annotation decode, exactly as the original `GetTalkTos` did). Every rerouted site individually listed. |
| R20 | A whole function in lifted protocol code is a **dialog / UI action or an app-policy decision** with no wire effect (`DoChannelDialog`, `DoKickDlg`, `TryNewNick` retry dialog, `ShowMOTD` gating, rules/notifs daemon glue, ratings prompts, the status-window activation tail of `ProcessSlashCommand`) | Wrap the **entire definition** in `#ifndef CC_NO_UI` (R11 mechanics) OR, where the function must still exist for a caller but its *body* is pure UI, replace the body with a single `ccEmitProtoEvent` of the decision to Swift (e.g. `TryNewNick` → emit `nick_rejected`, Swift owns the retry) and note which. Message boxes that are **user-facing protocol errors** become **error events carrying the text** (NOT R7/`ccLog` — the user must see them); only truly internal diagnostics use `ccLog`. List every wrap/replacement individually. |
| R21 | Lifted code opens or listens on a socket, or performs SSPI/NTLM auth, or does WinInet/registry I/O (`CIdentdSocket` identd listener ircproto.cpp:1406–1457; the SSPI block ircsock.cpp:413–998; `WriteToRegistry`; `gethostname`) | **Do not port the I/O.** identd → drop (emit nothing; modern ircds don't require it — decision recorded in Task 2) OR, only if a captured session proves a server demands it, a Swift `NWListener` reimplements it from the lifted reply-string format. SSPI/NTLM auth → **drop entirely**; keep only `authtypeNone`/`authtypePlainText` (PASS/NICK/USER + OPER). Where a dropped auth path was the only branch, emit `auth_unsupported` and fall through to plain login. `gethostname` → a value Swift supplies via the session config (`local_host`). Each drop/replacement listed. |

---

## File structure

New and modified files, with single responsibilities. "Lift" = copy from the read-only reference, then apply Edit Rules.

```
Sources/cchat-engine/
  include/comicchat.h            MODIFY  + cc_session API: handle, feed_bytes,
                                          cc_proto_events vtable, cc_proto_event
                                          union, timer callback, send callback,
                                          resolver callbacks. (the ONLY thing Swift sees)
  bridge/
    cc_session.cpp               CREATE  the C boundary: cc_session_create/destroy/
                                          feed_bytes/fire_timer/outbound helpers;
                                          ccEmitProtoEvent dispatcher (R18);
                                          ccSession() context + resolvers (R19)
    cc_session.h                 CREATE  internal: ccSession(), ccEmitProtoEvent(),
                                          the CCSession struct (holds the lifted
                                          CIrcSocket/CIrcProto + the callback set)
    cc_selftest.cpp              MODIFY  + protocol selftests (parse vectors, codec
                                          round-trips, captured-bytes replay)
  engine/
    ircsock.cpp / ircsock.h      LIFT    the parser: OnReceive framing (→ fed by
                                          cc_session), ParseIt, HandleCommand,
                                          HandleResultCode, HandleErrorCode; SSPI
                                          block R21-dropped; AfxMessageBox → error
                                          events (R20)
    ircproto.cpp / ircproto.h    LIFT    outbound builders + charset codec; identd
                                          R21-dropped; UI-ish sites dispositioned
                                          (17 sites, see ircproto-map.md §6)
    protsupp.cpp / protsupp.h    LIFT    Slash* + connection status + the annotation
                                          codec fns + payload second stage
                                          (ProcessSay/ProcessComment/ProcessUDIData);
                                          session-state fns R19/R20; dialog fns R20
    query.cpp / query.h          LIFT    CCQuery request/response correlation list
    ccommon_str.cpp / .h         CREATE  the string half of artifacts/core/ccommon.cpp
                                          lifted: bLowLevelQuoting/Unquoting,
                                          UTF-8 codec (bConvertWideStringToUTF8 etc.),
                                          SzNextUTF8Char — NO windows.h socket/registry
    cc_link_stubs.cpp            MODIFY  delete GetMyNickName stub (R19 replaces it);
                                          delete intl.c stubs IF Task decides CP-1252
                                          permanent (else keep, documented)
    mfc_compat.h / .cpp          MODIFY  + CPtrList shim (R9, selftested); + CharNext
                                          real member goes live + selftest (Plan 2 debt)
    engine_context.h / .cpp      MODIFY  Capitalize R11 restore (Plan 2 debt, NLS)
Sources/ComicChatKit/
    ProtocolSession.swift        CREATE  Swift session-state model (nick, rooms,
                                          members, props) + NWConnection driver +
                                          the cc_proto_events → AsyncStream adapter
    ProtocolEvents.swift         CREATE  Swift value types mirroring cc_proto_event
    WireCodec.swift              CREATE  CP-1252/UTF-8 display transcoding boundary
Tests/ComicChatKitTests/
    ProtocolParseTests.swift     CREATE  event-stream assertions over hand + captured bytes
    ProtocolCodecTests.swift     CREATE  annotation encode/decode + byte-compare
    ProtocolSessionTests.swift   CREATE  Swift session-state + NWConnection loopback
    Fixtures/captures/*.jsonl    CREATE  captured byte streams (from the Wine rig)
```

**Not lifted (verified against `chat.mak` / discovery):** `nmproto.cpp/.h` (dead — `#ifdef CB32SUPPORT`, not in OBJS); `chatsrv.cpp` (server *directory* + connect racer — zero session state; Swift's `NWConnection`/config replaces it wholesale); `webreq.cpp` (WinInet avatar download → Swift `URLSession`, Plan 4); `filesend.cpp` (DCC — deferred, out of scope this plan); the SSPI half of `ircsock.cpp`; `jis2sjis.cpp`/`sjis2jis.cpp` (spec §4.5).

---

## Architecture reference: the C boundary (defined in Task 1, cited by all later tasks)

This is the contract every task builds toward. Full definitions live in Task 1; this is the map.

- **Opaque handle:** `cc_session* cc_session_create(const cc_session_config* cfg)` / `void cc_session_destroy(cc_session*)`.
- **Inbound bytes:** `void cc_session_feed_bytes(cc_session*, const uint8_t* data, size_t len)` — Swift calls this with whatever `NWConnection` delivered; the engine buffers + line-frames internally (lifted `OnReceive` logic).
- **Outbound bytes:** the engine calls `cfg->send(user_data, const uint8_t* data, size_t len)` — Swift writes them to the socket. This is the single choke point `SendMessageText`→`Send` becomes.
- **Timer:** the engine calls `cfg->set_timer(user_data, int32_t timer_id, int32_t ms)` and `cfg->cancel_timer(user_data, int32_t timer_id)`; Swift schedules and later calls `void cc_session_fire_timer(cc_session*, int32_t timer_id)`. Exactly one timer id is used (`CC_TIMER_ISIRCX_PROBE`).
- **Events out:** the engine calls `cfg->on_event(user_data, const cc_proto_event* ev)` for every typed inbound event. `cc_proto_event` is a tagged union (`type` enum + `union`). Designed fresh in Task 5 from the discovery's callee list.
- **Resolvers (Swift → engine answers):** `cfg->own_nick(user_data) -> const char*` and `cfg->resolve_user(user_data, const char* nick, uint32_t room_token) -> cc_user_ref`. These let the engine rebuild talk-to references during annotation decode without owning the user table (R19).
- **Outbound commands (Swift → engine → bytes):** thin C functions wrapping the lifted `CRoomInfo` virtuals, e.g. `cc_session_send_say(cc_session*, uint32_t room_token, const cc_annotations* ann, const char* text_cp1252, uint16_t modes)`, `cc_session_join(cc_session*, const char* channel, const char* key)`, `cc_session_change_nick`, `cc_session_set_topic`, etc. Each maps 1:1 to an outbound builder.

---

### Task 1: The `cc_session` C boundary skeleton (no protocol yet)

**Files:**
- Modify: `Sources/cchat-engine/include/comicchat.h` (append the session API; no existing decls change)
- Create: `Sources/cchat-engine/bridge/cc_session.h`
- Create: `Sources/cchat-engine/bridge/cc_session.cpp`
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp` (register a new selftest)

**Interfaces:**
- Consumes: nothing (first protocol task); follows the existing opaque-handle + status-code idiom of `cc_strip_*` in `comicchat.h`.
- Produces: `cc_session`, `cc_session_config`, `cc_session_create/destroy/feed_bytes/fire_timer`, the callback typedefs (`cc_send_fn`, `cc_set_timer_fn`, `cc_cancel_timer_fn`, `cc_on_event_fn`, `cc_own_nick_fn`, `cc_resolve_user_fn`), `cc_user_ref` (typedef `uint32_t`), and the `ccSession()` accessor + `CCSession` struct. **No parsing yet** — `feed_bytes` just accumulates into an internal buffer and `fire_timer` is a no-op stub; this task proves the boundary compiles, links, round-trips a byte, and invokes a callback.

- [ ] **Step 1: Write the failing selftest** — add to `cc_selftest.cpp` a `cc_selftest_session_skeleton()` that: creates a `cc_session` with a config whose `send` callback appends to a `std::string` capture and whose `on_event` increments a counter; calls `cc_session_feed_bytes` with `"PING x\r\n"`; asserts the session is non-null and destroyable; asserts a helper `cc_session_test_echo(s)` (a temporary test-only hook that calls `cfg->send` with a fixed buffer) drove one `send` callback with the expected bytes. Register it in `cc_run_selftests`.

```cpp
// in cc_selftest.cpp, new function, registered in cc_run_selftests()
static int cc_selftest_session_skeleton() {
    std::string sent;
    int events = 0;
    cc_session_config cfg = {};
    cfg.user_data = &sent;            // send appends here
    cfg.send = [](void* ud, const uint8_t* d, size_t n) {
        static_cast<std::string*>(ud)->append(reinterpret_cast<const char*>(d), n);
    };
    cfg.on_event = [](void*, const cc_proto_event*) { /* counted via a static in the real test */ };
    // route the event counter through user_data2-style capture instead:
    struct Cap { std::string sent; int events = 0; } cap;
    cfg.user_data = &cap;
    cfg.send = [](void* ud, const uint8_t* d, size_t n) {
        static_cast<Cap*>(ud)->sent.append(reinterpret_cast<const char*>(d), n);
    };
    cfg.on_event = [](void* ud, const cc_proto_event*) { static_cast<Cap*>(ud)->events++; };
    cc_session* s = cc_session_create(&cfg);
    CC_CHECK(s != nullptr);
    cc_session_feed_bytes(s, reinterpret_cast<const uint8_t*>("PING x\r\n"), 8);
    cc_session_test_echo(s);          // test-only: drives one send + one event
    CC_CHECK(cap.sent == "ECHO\r\n");
    CC_CHECK(cap.events == 1);
    cc_session_destroy(s);
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails** — `cd macos/ComicChatKit && swift test 2>&1 | grep -i session`. Expected: compile failure (`cc_session_create` undeclared).

- [ ] **Step 3: Declare the boundary in `comicchat.h`** — append (do not disturb existing decls):

```c
/* ---- Protocol session (Plan 3): bytes in, events out ---------------------
 * The engine never opens a socket. Swift owns NWConnection and the event loop.
 * All calls are single-threaded (see the engine threading contract above).
 * Text buffers are wire bytes (CP-1252 by default); the engine does not
 * transcode — Swift converts for display. */

typedef struct cc_session cc_session;
typedef uint32_t cc_user_ref;     /* opaque per-user handle from resolve_user; 0 = unknown */
#define CC_USER_REF_NONE 0u

/* One-shot timer ids the engine may request (Swift schedules, then calls
 * cc_session_fire_timer). Exactly one is used today. */
#define CC_TIMER_ISIRCX_PROBE 1

typedef void        (*cc_send_fn)(void* user_data, const uint8_t* data, size_t len);
typedef void        (*cc_set_timer_fn)(void* user_data, int32_t timer_id, int32_t ms);
typedef void        (*cc_cancel_timer_fn)(void* user_data, int32_t timer_id);
struct cc_proto_event;  /* defined below in Task 5's block */
typedef void        (*cc_on_event_fn)(void* user_data, const struct cc_proto_event* ev);
typedef const char* (*cc_own_nick_fn)(void* user_data);   /* never NULL; "" if unknown */
typedef cc_user_ref (*cc_resolve_user_fn)(void* user_data, const char* nick, uint32_t room_token);

typedef struct cc_session_config {
    void*               user_data;
    cc_send_fn          send;
    cc_set_timer_fn     set_timer;
    cc_cancel_timer_fn  cancel_timer;
    cc_on_event_fn      on_event;
    cc_own_nick_fn      own_nick;
    cc_resolve_user_fn  resolve_user;
    const char*         local_host;    /* value gethostname would have returned; may be NULL */
    int32_t             encoding;      /* 0 = CP-1252 (default), 1 = UTF-8 (per spec §4.5) */
} cc_session_config;

cc_session* cc_session_create(const cc_session_config* cfg);   /* NULL on bad cfg */
void        cc_session_destroy(cc_session* s);                 /* no-op if NULL */
void        cc_session_feed_bytes(cc_session* s, const uint8_t* data, size_t len);
void        cc_session_fire_timer(cc_session* s, int32_t timer_id);

/* Test-only hook (Task 1): drives one send("ECHO\r\n") + one on_event. Removed
 * once real parsing lands; kept behind CC_SESSION_TESTHOOK. */
void        cc_session_test_echo(cc_session* s);
```

- [ ] **Step 4: Define `CCSession` + `ccSession()` in `cc_session.h`** — the internal struct that later tasks grow (it will own the lifted `CIrcSocket`/`CIrcProto`; for now just the config + inbound buffer):

```cpp
// cc_session.h — internal to the bridge; not part of the public API.
#ifndef CC_SESSION_H
#define CC_SESSION_H
#include "comicchat.h"
#include <string>

struct CCSession {
    cc_session_config cfg{};
    std::string       inbuf;      // accumulates fed bytes; line-framed in Task 3
};

// The active session for the current single-threaded call (set on entry to any
// cc_session_* that runs lifted code). Lifted code reaches callbacks/resolvers
// through this, never through theApp. ASSERTs if no session is active.
CCSession* ccSession();

// R18 dispatcher: fill a cc_proto_event and invoke cfg.on_event. Defined in
// cc_session.cpp; declared here so lifted files can call it.
void ccEmitProtoEvent(const struct cc_proto_event* ev);

#endif
```

- [ ] **Step 5: Implement `cc_session.cpp`** — create/destroy, the `ccSession()` thread-local-free global (single-threaded contract, so a plain file-static pointer), `feed_bytes` appends to `inbuf`, `fire_timer` no-op, and the test echo hook:

```cpp
#include "cc_session.h"
#include "comicchat.h"     // cc_proto_event (Task 5 fills it; Task 1 forward-uses type=0)
#include <cassert>
#include <cstring>

static CCSession* g_session = nullptr;   // single-threaded: one active session
CCSession* ccSession() { assert(g_session && "no active cc_session"); return g_session; }

void ccEmitProtoEvent(const cc_proto_event* ev) {
    CCSession* s = ccSession();
    if (s->cfg.on_event) s->cfg.on_event(s->cfg.user_data, ev);
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
    g_session = s;                       // activate for lifted code (Task 3+)
    s->inbuf.append(reinterpret_cast<const char*>(d), n);
    // Task 3 replaces this with the lifted line-framer + parse dispatch.
    g_session = nullptr;
}
void cc_session_fire_timer(cc_session* h, int32_t) { (void)h; /* Task 4 */ }

void cc_session_test_echo(cc_session* h) {
    CCSession* s = reinterpret_cast<CCSession*>(h);
    g_session = s;
    if (s->cfg.send) s->cfg.send(s->cfg.user_data,
                                 reinterpret_cast<const uint8_t*>("ECHO\r\n"), 6);
    cc_proto_event ev; std::memset(&ev, 0, sizeof(ev));  // type 0 placeholder
    ccEmitProtoEvent(&ev);
    g_session = nullptr;
}
```

Add `cc_session.cpp` to the target (SwiftPM globs `Sources/cchat-engine/**`, so no `Package.swift` edit — confirm by build). Add a minimal forward `struct cc_proto_event { int32_t type; };` **temporarily** at the top of `cc_session.cpp` guarded `#ifndef CC_PROTO_EVENT_DEFINED` so Task 1 compiles before Task 5 defines the real union; Task 5 removes the placeholder.

- [ ] **Step 6: Run to verify it passes** — `swift test 2>&1 | tail -20`. Expected: all suites green including the new session selftest (surfaced via `cc_run_selftests` → the existing `EngineSelfTests` Swift wrapper).

- [ ] **Step 7: Commit**

```bash
git add macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.cpp \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp
git commit -m "macos: Plan 3 Task 1 - cc_session C boundary skeleton (bytes in, events out)"
```

---

### Task 2: Entry debt + lift the string-codec core (`ccommon_str`) and shims

This is the Plan-2-style entry-debt task: clear the handed-over debt and stand up the low-level string primitives every later protocol task needs, before touching the parser. No socket, no protocol dispatch.

**Files:**
- Create: `Sources/cchat-engine/engine/ccommon_str.cpp` (lift the string half of `artifacts/core/ccommon.cpp`)
- Create: `Sources/cchat-engine/engine/ccommon_str.h` (lift the relevant decls from `artifacts/inc/ccommon.h`)
- Modify: `Sources/cchat-engine/shim/mfc_compat.h` + `.cpp` (add `CPtrList`; make the `CharNext` shim member live + selftest — Plan 2 debt)
- Modify: `Sources/cchat-engine/shim/engine_context.h`/`.cpp` (restore `Capitalize` verbatim — Plan 2 real-metrics debt)
- Modify: `Sources/cchat-engine/engine/cc_link_stubs.cpp` (record the intl.c decision)
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp` (codec + shim selftests)

**Interfaces:**
- Consumes: Task 1's build (nothing at the API level).
- Produces: `bLowLevelQuoting`, `bLowLevelUnquoting` (exact signatures from `ccommon.h`), `bConvertWideStringToUTF8`, `bConvertUTF8StringToWide`, `SzNextUTF8Char`, and the constants `g_chLLQuoteCTCP` (0x10), `g_chLLQuoteIRCX` ('\\'); a working `CPtrList`; a live `CharNext`; a restored `Capitalize`. These are the primitives Tasks 6/7/8 call.

- [ ] **Step 1: Decide + record the identd and intl.c postures** — in the task report and as code comments: (a) **identd is dropped** (R21) — modern ircds do not require it; the Wine rig confirmed a clean connect with no identd (`wine-capture-rig.md`). (b) **CP-1252 is made the permanent default posture** (spec §4.5) — so the `intl.c` stubs in `cc_link_stubs.cpp` (`GetMime`→NULL, `iBytesofChar`→1, `FindSubStringForINTLThatFits`) **stay as documented byte-identical reimplementations**, not a deferred lift; update their comment to cite this decision (they are no longer "delete when intl.c lifts" — they are the permanent CP-1252 implementation). No code change beyond the comment.

- [ ] **Step 2: Write the failing low-level-quoting selftest** — hand-verified vectors from `ircproto-map.md` §5 (escape set {0x0A→`Qn`, 0x0D→`Qr`, Q→`QQ`}, Q=0x10; tolerant all-or-nothing decode):

```cpp
static int cc_selftest_llquote() {
    const char Q = (char)0x10;
    char* dst = nullptr; BOOL freeit = FALSE;
    // "a\nb" -> "a" Q 'n' "b"
    BOOL changed = bLowLevelQuoting(Q, TRUE, "a\nb", &dst, &freeit, FALSE);
    CC_CHECK(changed && dst[0]=='a' && dst[1]==Q && dst[2]=='n' && dst[3]=='b' && dst[4]==0);
    if (freeit) free(dst);
    // decode round-trips
    char buf[16]; strcpy(buf, "a\x10n" "b");
    bLowLevelUnquoting(Q, TRUE, buf, buf);   // in-place
    CC_CHECK(strcmp(buf, "a\nb") == 0);
    // tolerant: stray naked Q not followed by n/r/Q => whole string verbatim
    char buf2[16]; strcpy(buf2, "a\x10z" "b");
    bLowLevelUnquoting(Q, TRUE, buf2, buf2);
    CC_CHECK(strcmp(buf2, "a\x10z" "b") == 0);
    return 0;
}
```

- [ ] **Step 3: Run to verify it fails** — `swift test 2>&1 | grep -i llquote`. Expected: link error (`bLowLevelQuoting` undefined).

- [ ] **Step 4: Lift `ccommon_str`** — copy ONLY the string/byte functions from `artifacts/core/ccommon.cpp` into `engine/ccommon_str.cpp` (leave every socket/registry/`windows.h`-heavy function behind; the file must compile with no `winsock`/registry includes): `bLowLevelQuoting` (ccommon.cpp:945), `bLowLevelUnquoting` (:1026), `bConvertWideStringToUTF8` (:218), `bConvertUTF8StringToWide` (:364), `SzNextUTF8Char` (:500), and any pure helper they call. Apply R1 (`stdafx.h`→`mfc_compat.h`), R13 for any pre-standard idiom. Copy the matching decls + constants into `ccommon_str.h` from `artifacts/inc/ccommon.h:39-60`. Provenance comment at the top: `// lifted from artifacts/core/ccommon.cpp (string half only) — see plan-3 R-notes`.

- [ ] **Step 5: Run to verify the codec test passes** — `swift test 2>&1 | grep -i llquote`. Expected: PASS.

- [ ] **Step 6: Add `CPtrList` to `mfc_compat.h`** (R9) — the parser's query list is `CPtrList`-derived (`ircproto-map.md` §8). Add a minimal `CPtrList` (the same `POSITION`/`GetHead`/`AddTail`/`RemoveHead`/`GetNext`/`RemoveAll`/`IsEmpty`/`GetCount` surface the existing `CPtrArray` shim exposes, list-flavored). Write a selftest exercising add/iterate/remove.

- [ ] **Step 7: Make `CharNext` live + selftest** (Plan 2 debt) — the `CharNext` shim member added in Plan 2 Task 5 was compiled out; its callers (`nGetBreakingPoint`, DecodeNick) go live this plan. Remove the compile-out guard, add a selftest that advances through an ASCII and a two-byte sequence (single-byte behavior under CP-1252).

- [ ] **Step 8: Restore `Capitalize` verbatim** (Plan 2 real-metrics debt) — Plan 2 Task 6 R11-wrapped `Capitalize`'s NLS internals as a no-op, a latent real-metrics layout divergence. Restore the original body (the CP-1252/`AnsiUpper`-equivalent single-byte capitalization) so protocol-driven layout matches the original. Selftest: capitalize a known string, assert the first-letter-of-line behavior the original produced. Cite the original `Capitalize` in `balloon.cpp`/`format.cpp` (whichever holds it) and restore only that function.

- [ ] **Step 9: Run all + fidelity diff** — `swift test` fully green. In the report, diff `ccommon_str.cpp` against the original `artifacts/core/ccommon.cpp` functions (every hunk R1/R13-attributed); list the `CPtrList`/`CharNext`/`Capitalize` additions as R9/restore with their selftests.

- [ ] **Step 10: Commit**

```bash
git add macos/ComicChatKit/Sources/cchat-engine/engine/ccommon_str.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/ccommon_str.h \
        macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.h \
        macos/ComicChatKit/Sources/cchat-engine/shim/mfc_compat.cpp \
        macos/ComicChatKit/Sources/cchat-engine/shim/engine_context.h \
        macos/ComicChatKit/Sources/cchat-engine/shim/engine_context.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/cc_link_stubs.cpp \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp
git commit -m "macos: Plan 3 Task 2 - entry debt (CharNext/Capitalize/intl.c) + ccommon_str codec core + CPtrList"
```

---

### Task 3: Lift the annotation codec (encode/decode) — pure functions, no session

The codec is the highest-value, lowest-risk lift (discovery: ~15 small CString/CDWordArray-only functions). Build it standalone and freeze it with byte-exact round-trip tests before wiring any parser.

**Files:**
- Create: `Sources/cchat-engine/engine/protsupp.cpp` + `protsupp.h` — but this task lifts **only** the codec subset (byte packers, `bInsertAnnotations`, `ProcessUDIData`, the tokenizers, key-string codec); the rest of protsupp is stubbed/deferred to Task 7. Mark the file `// PARTIAL LIFT — codec only in Task 3; session/pipeline in Task 7`.
- Modify: `Sources/cchat-engine/engine/avatario.cpp` — remove `#ifndef CC_NO_PROTOCOL` around `EmotionToBytes`/`BytesToEmotion` (they go live)
- Modify: `Sources/cchat-engine/include/comicchat.h` — add `cc_annotations` value struct (the decoded/encodable UDI block)
- Modify: `Package.swift` — remove `.define("CC_NO_PROTOCOL")`
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp` — codec round-trip vectors

**Interfaces:**
- Consumes: Task 2's `ccommon_str` (quoting), the already-lifted `format.cpp` codec, the already-lifted `CAvatarX`.
- Produces: `cc_annotations` (C struct: gesture idx/emotion/intensity, expression idx/emotion/intensity, requested flag, mode, up to 5 addressee nick strings); `IndexToByte`/`ByteToIndex`, `SM2BM`/`BM2SM`, `bInsertAnnotations` (encoder), `ProcessUDIData` (decoder) — callable engine-internal; and the wire-grammar contract frozen by tests. `EmotionToBytes`/`BytesToEmotion` now compile.

- [ ] **Step 1: Define `cc_annotations` in `comicchat.h`** — the value type the wire grammar `#G<gp><ge><gi>E<ep><ee><ei>[R]M<m>[T…]` decodes to (from `state-and-codec.md` §3.3):

```c
/* Decoded comic annotation block ("User Display Info"). Values are indices,
 * NOT the +'0' wire bytes. addressees are encoded nick strings (CP-1252). */
#define CC_MAX_ADDRESSEES 5
typedef struct cc_annotations {
    int32_t gesture_pose, gesture_emotion, gesture_intensity;   /* G group */
    int32_t face_pose,    face_emotion,    face_intensity;      /* E group */
    int32_t requested;                                          /* R flag (0/1) */
    int32_t mode;                                               /* SM_* say mode 1..5 */
    int32_t addressee_count;
    char    addressees[CC_MAX_ADDRESSEES][64];                  /* encoded nicks */
    int32_t cooked;                                             /* both intensities present */
} cc_annotations;
```

- [ ] **Step 2: Write the failing codec round-trip selftest** — hand-encode a known block, decode it, assert field-exact; then encode from a `cc_annotations` and byte-compare against the hand-authored wire string. Arithmetic documented in the comment (index+'0' packing):

```cpp
static int cc_selftest_annotation_codec() {
    // Wire: "#G" + (2+'0')(9+'0')(5+'0') + "E" + (1+'0')(9+'0')(3+'0') + "M" + (1+'0')
    //  gesture pose 2 emotion 9(NEUTRAL) intensity 5; face pose 1 emotion 9 int 3; mode 1(SAY)
    const char* wire = "#G295E193M1";   // '2'..= 0x32; hand-traced below
    cc_annotations a; memset(&a, 0, sizeof a);
    int ok = cc_test_decode_udi(wire, &a);         // thin test wrapper over ProcessUDIData
    CC_CHECK(ok == 0);
    CC_CHECK(a.gesture_pose==2 && a.gesture_emotion==9 && a.gesture_intensity==5);
    CC_CHECK(a.face_pose==1 && a.face_emotion==9 && a.face_intensity==3);
    CC_CHECK(a.mode==1 && a.addressee_count==0);
    char out[128];
    int n = cc_test_encode_udi(&a, out, sizeof out);   // thin wrapper over bInsertAnnotations
    CC_CHECK(n > 0 && strcmp(out, "#G295E193M1") == 0);
    return 0;
}
```
Document the byte arithmetic in a comment: `'2'=0x32=2+'0'`, etc., and that `IndexToByte(v)=v+'0'` per `protsupp.cpp:1023`.

- [ ] **Step 3: Run to verify it fails** — `swift test 2>&1 | grep -i annotation`. Expected: link error (`cc_test_decode_udi` / `bInsertAnnotations` undefined).

- [ ] **Step 4: Lift the codec subset into `protsupp.cpp`** — copy from the original `protsupp.cpp` ONLY: `IndexToByte`/`ByteToIndex` (:1023), `SM2BM`/`BM2SM` (:1035), tokenizers `GetToken`/`GetToken1`/`GetToken2`/`bForEachWord` (:257-422), `bInsertAnnotations` (:3057), `ProcessUDIData` (:1485), `GetAddressees`/`GetWhisperedAddressees` (:3006), `GetTalkTos` ×2 (:1066) — but **stub `LookupPui` behind R19 for now** (Task 7 wires the resolver; here `GetTalkTos` may take an injected resolver or the test provides a trivial one), key-string codec (:5073-5241). Apply R1/R2/R8. Anything these reference that isn't codec (ignore/flood/history) is NOT copied — if a copied function calls into it, that call is R20-wrapped or hoisted to a parameter. List every such boundary in the report.

- [ ] **Step 5: Un-gate `EmotionToBytes`/`BytesToEmotion`** — remove the `#ifndef CC_NO_PROTOCOL`/`#endif` around them in `avatario.cpp` (R11 removal = restoring original code). Remove `.define("CC_NO_PROTOCOL")` from `Package.swift`.

- [ ] **Step 6: Add the test wrappers** — `cc_test_decode_udi`/`cc_test_encode_udi` in `cc_selftest.cpp` (or a small `bridge/cc_proto_testhooks.cpp`), thin adapters that call `ProcessUDIData`/`bInsertAnnotations` with a `cc_annotations` in/out and a trivial resolver.

- [ ] **Step 7: Run to verify it passes** — `swift test 2>&1 | grep -i annotation`. Expected: PASS. Also confirm the whole suite still green (un-gating `CC_NO_PROTOCOL` must not break existing avatario paths).

- [ ] **Step 8: Add the two-transport + edge-case vectors** — freeze: (a) IRCX transport is just the `#G…` blob (no parens); (b) plain-IRC transport wraps it `(#G…) text` — test the decoder's `strncmp(szMesg,"(#",2)` + `") "` split (`state-and-codec.md` §3.3); (c) the anti-spoof mask (private SAY/THINK → WHISPER, `protsupp.cpp:1588`); (d) addressee `T` list with 5 nicks + the clip-at-5 truncation; (e) `cooked` set only when both intensities present. One `CC_CHECK` per behavior, arithmetic hand-traced in comments.

- [ ] **Step 9: Fidelity diff + commit** — diff the lifted `protsupp.cpp` codec functions against the original (every hunk rule-attributed; R19/R20 boundaries listed). Then:

```bash
git add macos/ComicChatKit/Sources/cchat-engine/engine/protsupp.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/protsupp.h \
        macos/ComicChatKit/Sources/cchat-engine/engine/avatario.cpp \
        macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h \
        macos/ComicChatKit/Package.swift \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp
git commit -m "macos: Plan 3 Task 3 - annotation codec lift (encode/decode) + un-gate CC_NO_PROTOCOL"
```

---

### Task 4: Lift the outbound builders (`ircproto.cpp`) + the CCQuery list (`query.cpp`)

Outbound is a clean, testable unit: given a command, produce exact wire bytes. No inbound parsing yet — the `send` callback captures bytes for byte-compare.

**Files:**
- Create: `Sources/cchat-engine/engine/ircproto.cpp` + `ircproto.h` (lift; identd R21-dropped; 17 UI-ish sites dispositioned per `ircproto-map.md` §6)
- Create: `Sources/cchat-engine/engine/query.cpp` + `query.h` (lift the CCQuery correlation list)
- Modify: `Sources/cchat-engine/bridge/cc_session.cpp`/`.h` — `CCSession` gains the `CIrcProto`/`CIrcSocket` members; wire `SendMessageText`→`cfg.send`
- Modify: `Sources/cchat-engine/include/comicchat.h` — outbound command C functions
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp` — outbound byte-compare tests

**Interfaces:**
- Consumes: Task 2 (`ccommon_str`, `CPtrList`), Task 3 (codec, for `cc_session_send_say`), Task 1 (`ccSession()`, `cfg.send`).
- Produces: `cc_session_join/part/change_nick/set_topic/kick/invite/set_mode/away/who/whois/list/send_say/send_whisper/send_action` (each maps to an `ircproto.cpp` builder); the `CIrcProto::SendMessageText` → `cfg.send` binding; the timer request path (`cfg.set_timer(CC_TIMER_ISIRCX_PROBE)`); the CCQuery enqueue-before-send discipline.

- [ ] **Step 1: Write the failing outbound byte-compare selftest** — drive a JOIN and a plain-IRC say through the builders; assert exact bytes on the `send` capture:

```cpp
static int cc_selftest_outbound_join_say() {
    struct Cap { std::string sent; } cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap;
    cfg.send = [](void* ud, const uint8_t* d, size_t n){
        static_cast<Cap*>(ud)->sent.append((const char*)d, n); };
    cfg.own_nick = [](void*){ return "Anon"; };
    cc_session* s = cc_session_create(&cfg);
    cc_session_join(s, "#comicrig", nullptr);
    CC_CHECK(cap.sent == "JOIN #comicrig\r\n");          // ircproto.cpp:810
    cap.sent.clear();
    cc_annotations a; memset(&a,0,sizeof a); a.mode = 1;  // SAY, no pose/addressees
    cc_session_send_say(s, /*room_token*/1, &a, "hi", /*modes*/0);
    // plain-IRC transport: "(#...M1) hi" — exact blob per Task 3 grammar
    CC_CHECK(cap.sent.find("PRIVMSG ") == 0 && cap.sent.find("hi") != std::string::npos);
    cc_session_destroy(s);
    return 0;
}
```
(The say assertion is loosened here only because the room-token→channel-name mapping is finalized in this task's Step 4; tighten to an exact byte string once the mapping exists.)

- [ ] **Step 2: Run to verify it fails** — `swift test 2>&1 | grep -i outbound`. Expected: link error.

- [ ] **Step 3: Lift `query.cpp`/`query.h`** — the `CCQuery`/`CQueryPtrList` correlation list (`ircproto-map.md` §1). Apply R1/R8. It's `CPtrList`-derived (Task 2 shim). Selftest: enqueue two queries, `FindQuery` by command-type dequeues oldest-matching.

- [ ] **Step 4: Lift `ircproto.cpp`/`ircproto.h`** — apply the full disposition table from `ircproto-map.md` §6:
  - `SendMessageText` (ircproto.cpp:475) — body becomes `ccSession()->cfg.send(...)` (R19; the single outbound choke point). List it.
  - identd `CIdentdSocket`/`StartIdentD`/`StopIdentD` (:1406-1457) — **R21 drop** (whole-function `#ifndef CC_NO_UI` or delete-with-note; `StopIdentD` call in `OnLogin` becomes a no-op).
  - `bChatSendToTarget` chunking loop (:481-698) — lift verbatim; its `GetMyNickName`/ident-length accounting (:527-535) → R19 `ccSessionOwnNick()`; buffer via session.
  - `bExecuteQuery` (:1034) — lift; `SendMessageText` binding above carries it; the `MODE ISIRCX` builder's `SetTimer` (via `OnConnect`, but the timer request is engine-side) → `cfg.set_timer(CC_TIMER_ISIRCX_PROBE, 50000)` (R21-adjacent: engine requests, Swift schedules).
  - Charset codec `EncodeNick`/`DecodeNick`/`EncodeChan`/`DecodeChan`/`EncodeString` (:204-395) — lift; `bConvertWideStringToUTF8` etc. from Task 2; `theApp.m_wszBuffer`/`m_szBuffer` scratch → R19 session scratch (or thread-safe locals). Under the CP-1252 posture (Task 2) the DBCS arms may be dead — keep them but note.
  - The room-list/user-list dialog fillers `ChatFillRoomList`/`ChatFillUserList` (:101-184, the two live `GetWindowText` UI reads) — **R20** whole-function wrap; their core resurfaces as the `cc_session_list`/`cc_session_who` outbound commands (which call `bExecuteQuery` directly).
  - `GetMyIP` (:187) — R20/`CC_NO_UI` (DCC-only caller, deferred).
  - Each of the 17 sites individually listed in the report with its disposition.

- [ ] **Step 5: Grow `CCSession`** — add `CIrcSocket sock; CIrcProto proto;` members (or pointers), wired so `proto.m_pSock = &sock` and `SendMessageText` reaches `cfg.send`. Add the room-token↔channel mapping the outbound commands need (a small `std::vector<std::string>` indexed by token, or reuse the doc-less `CIrcProto` per room — decide and document; simplest: token is an index into a session-held channel-name table Swift populates on join-confirm).

- [ ] **Step 6: Add the outbound C functions** to `comicchat.h` + implement in `cc_session.cpp`, each a thin wrapper calling the matching `CIrcProto` virtual/builder:

```c
int32_t cc_session_join(cc_session*, const char* channel, const char* key /*nullable*/);
int32_t cc_session_part(cc_session*, uint32_t room_token, const char* reason);
int32_t cc_session_change_nick(cc_session*, const char* new_nick);
int32_t cc_session_set_topic(cc_session*, uint32_t room_token, const char* topic);
int32_t cc_session_send_say(cc_session*, uint32_t room_token, const cc_annotations*, const char* text, uint16_t modes);
int32_t cc_session_send_whisper(cc_session*, uint32_t room_token, const cc_annotations*, const char* text, const char* const* nicks, int32_t nick_count);
int32_t cc_session_who(cc_session*, const char* mask /*nullable*/);
int32_t cc_session_list(cc_session*);
/* … kick/invite/set_mode/away as needed, each 1:1 with a builder … */
```

- [ ] **Step 7: Run + tighten** — `swift test 2>&1 | grep -i outbound` PASS; now tighten the say assertion to the exact plain-IRC wire string once the room-token mapping is in.

- [ ] **Step 8: Fidelity diff + commit** — diff `ircproto.cpp`/`ircproto.h`/`query.*` vs originals; every hunk rule-attributed; all 17 sites + R21 drops listed.

```bash
git add macos/ComicChatKit/Sources/cchat-engine/engine/ircproto.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/ircproto.h \
        macos/ComicChatKit/Sources/cchat-engine/engine/query.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/query.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.cpp \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.h \
        macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp
git commit -m "macos: Plan 3 Task 4 - outbound builders (ircproto) + CCQuery list + timer request; identd/SSPI dropped (R21)"
```

---

### Task 5: Design the inbound event vocabulary + lift the parser (`ircsock.cpp`)

The core of the plan. Define `cc_proto_event` fresh (discovery: no inbound interface exists), then lift the parse dispatch, replacing each direct app-callee with an event emission (R18).

**Files:**
- Modify: `Sources/cchat-engine/include/comicchat.h` — the full `cc_proto_event` union + `cc_proto_event_type` enum
- Create: `Sources/cchat-engine/engine/ircsock.cpp` + `ircsock.h` (lift the parser; SSPI block R21-dropped; AfxMessageBox → error events R20)
- Modify: `Sources/cchat-engine/bridge/cc_session.cpp`/`.h` — feed_bytes runs the lifted framer+dispatch; `ccEmitProtoEvent` filled; `fire_timer` → `HrModeIsIrcXFailure`
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp` — parse-vector event assertions

**Interfaces:**
- Consumes: all prior tasks (codec for text/data events, outbound for reply-triggered emits like PONG, `ccSession()`/`ccEmitProtoEvent`).
- Produces: the complete inbound event surface; `cc_session_feed_bytes` fully functional; `cc_session_fire_timer(CC_TIMER_ISIRCX_PROBE)` → plain-IRC fallback.

- [ ] **Step 1: Define `cc_proto_event` in `comicchat.h`** — one variant per feature-level event, derived from the callee inventory in `ircproto-map.md` §3 and the command surface in `command-surface.md` §3. The enum (grouped):

```c
typedef enum cc_proto_event_type {
    CC_EV_NONE = 0,
    /* connection lifecycle */
    CC_EV_LOGGED_IN,          /* 001 → own actual nick */
    CC_EV_SERVER_CAPS,        /* 800 → ircx?, max_msg_len */
    CC_EV_DISCONNECTED_HINT,  /* fatal ERROR text */
    /* membership */
    CC_EV_SELF_JOINED, CC_EV_SELF_PARTED,
    CC_EV_USER_JOINED, CC_EV_USER_PARTED, CC_EV_USER_QUIT,
    CC_EV_NAMES, CC_EV_END_OF_NAMES,
    CC_EV_NICK_CHANGED,
    /* messages (the core comic events) */
    CC_EV_TEXT,               /* PRIVMSG/NOTICE: nick, target, text, kind, annotations? */
    CC_EV_DATA,               /* IRCX DATA CCUDI1: annotations */
    CC_EV_WHISPER,
    CC_EV_ACTION, CC_EV_SOUND, CC_EV_AWAY_PEER,   /* CTCP-derived */
    CC_EV_APPEARS_AS,         /* "# Appears as name.url" avatar announce */
    /* room state */
    CC_EV_TOPIC_CHANGED, CC_EV_CHANNEL_MODE, CC_EV_USER_MODE,
    CC_EV_ROOM_PROP,          /* PROP CLIENT (bk backdrop etc.) */
    CC_EV_ROOM_LIST_BEGIN, CC_EV_ROOM_LIST_ITEM, CC_EV_ROOM_LIST_END,
    CC_EV_WHOIS_RESULT, CC_EV_WHO_RESULT,
    CC_EV_MOTD,
    /* errors & prompts (Swift owns retry/prompt) */
    CC_EV_ERROR,              /* typed error + human text (from numerics/AfxMessageBox) */
    CC_EV_NICK_REJECTED,      /* 431/432/433 → Swift retries */
    CC_EV_AUTH_UNSUPPORTED,   /* SSPI path dropped (R21) */
    CC_EV_STATUS_LINE,        /* catch-all status text (permissive: unknown numerics) */
} cc_proto_event_type;
```
And the union (representative variants shown — every type gets a struct; text/data carry an embedded `cc_annotations` + a `has_annotations` flag; nicks/targets/text are CP-1252 byte pointers valid only for the callback duration):

```c
typedef struct cc_proto_event {
    cc_proto_event_type type;
    uint32_t room_token;          /* 0 if not room-scoped */
    union {
        struct { const char* nick; } logged_in;
        struct { int32_t ircx; int32_t max_msg_len; } server_caps;
        struct { const char* nick; const char* ident; } user_joined;
        struct { const char* old_nick; const char* new_nick; int32_t is_self; } nick_changed;
        struct { const char* nick; const char* ident; const char* target;
                 const char* text; int32_t kind;      /* MT_* */
                 int32_t has_annotations; cc_annotations annotations; } text;
        struct { const char* nick; cc_annotations annotations; } data;
        struct { const char* channel; const char* topic; } topic_changed;
        struct { const char* channel; const char* nicks; } names;   /* space-joined */
        struct { const char* key; const char* value; } room_prop;
        struct { const char* name; int32_t users; const char* topic; } room_list_item;
        struct { int32_t code; const char* text; } error;
        struct { int32_t kind; const char* bad_nick; } nick_rejected;
        struct { const char* text; } status_line;
        /* … one struct per remaining type … */
    } u;
} cc_proto_event;
```
Remove the Task-1 placeholder `struct cc_proto_event`.

- [ ] **Step 2: Write the failing parse-vector selftest** — feed a captured registration + JOIN + PRIVMSG-with-annotations byte sequence; assert the emitted event sequence. Use bytes from a real capture (Task-9 rig) or hand-authored to the grammar:

```cpp
static int cc_selftest_parse_join_privmsg() {
    struct Cap { std::vector<int> types; std::string last_text; std::string last_nick; } cap;
    cc_session_config cfg = {};
    cfg.user_data = &cap; cfg.send = [](void*,const uint8_t*,size_t){};
    cfg.own_nick = [](void*){ return "Anon"; };
    cfg.on_event = [](void* ud, const cc_proto_event* ev){
        Cap* c = static_cast<Cap*>(ud); c->types.push_back(ev->type);
        if (ev->type==CC_EV_TEXT){ c->last_text=ev->u.text.text; c->last_nick=ev->u.text.nick; }
    };
    cc_session* s = cc_session_create(&cfg);
    const char* wire =
        ":srv 001 Anon :Welcome\r\n"
        ":Bob!bob@h JOIN :#comicrig\r\n"
        ":Bob!bob@h PRIVMSG #comicrig :(#G295E193M1) hello\r\n";
    cc_session_feed_bytes(s, (const uint8_t*)wire, strlen(wire));
    // expect: LOGGED_IN, USER_JOINED, TEXT(with annotations, "hello", "Bob")
    CC_CHECK(cap.types.size() >= 3);
    CC_CHECK(cap.types[0]==CC_EV_LOGGED_IN);
    CC_CHECK(std::find(cap.types.begin(),cap.types.end(),CC_EV_USER_JOINED)!=cap.types.end());
    CC_CHECK(cap.last_text=="hello" && cap.last_nick=="Bob");
    cc_session_destroy(s);
    return 0;
}
```

- [ ] **Step 3: Run to verify it fails** — `swift test 2>&1 | grep -i parse_join`. Expected: fail (feed_bytes still just buffers).

- [ ] **Step 4: Lift `ircsock.cpp`/`ircsock.h`** — the big one. Apply, per `ircproto-map.md` §2–§3:
  - `OnReceive` framing (:1000-1032) — the line-splitter body moves into `cc_session_feed_bytes`: append to `inbuf`, loop on `\n`, hand each line to `ProcessMessage`. The `CAsyncSocket` inheritance is dropped (R21).
  - `ParseIt`/`NGetCmd`/`ParseChannelMode` (:137-400) — lift verbatim (pure parse).
  - `ProcessMessage`/`HandleCommand`/`HandleResultCode`/`HandleErrorCode` (:1115-3496) — lift; **each direct app-callee → `ccEmitProtoEvent` (R18)**, individually listed. Mapping (from §3 tables): `OnTextMsg`→`CC_EV_TEXT`, `OnDataMsg`→`CC_EV_DATA`, `AddAndExecute(JoinEntry)`→`CC_EV_USER_JOINED`, `(PartEntry)`→`CC_EV_USER_PARTED`, `(NickEntry)`→`CC_EV_NICK_CHANGED`, `OnKick`→(kick event), `OnInvite`→(invite event), `AddToStatus`→`CC_EV_STATUS_LINE`, `bProcessAddChannel`→`CC_EV_SELF_JOINED`, `ShowMOTD`→`CC_EV_MOTD`, room-list feeders→`CC_EV_ROOM_LIST_*`, `bMatchAndApplyRules`→**dropped** (rules/notifs are app-layer, R20 — the event is emitted; Swift may run rules).
  - The **SSPI/NTLM block** (:413-998) — **R21 drop**; `HrIrcXLogin`'s auth branch emits `CC_EV_AUTH_UNSUPPORTED` and falls through to plain `HrIrcLogin` (PASS/NICK/USER). Keep `HrIrcLogin`, `HrModeIsIrcXFailure`, the 800/IRCX capability parse.
  - `AfxMessageBox` sites (~20, §3.3) — user-facing → `CC_EV_ERROR` with the text (R20, NOT R7); internal diagnostics → `ccLog`.
  - The RoomList dialog-widget pokes (403 handler, :3072-3084) — R20 wrap; emit `CC_EV_ERROR(bad-channel)`.
  - `TryNewNick` dialog (via 431/432/433) — R20: emit `CC_EV_NICK_REJECTED`; Swift owns retry.
  - Preserve the RFC2812 `JOIN #chan` modernization (:1364-1369) — the captured-bytes tests against ngircd depend on it.
  - Direct room-property writes (`ParseChannelMode`→`m_dwModes` etc.) — these write the lifted `CIrcProto`'s `CRoomInfo` base, which the engine still holds transiently; ALSO emit `CC_EV_CHANNEL_MODE`/`CC_EV_TOPIC_CHANGED` so Swift tracks the canonical copy. (The engine's copy is scratch; Swift's is canonical per `state-and-codec.md` §1.4.)

- [ ] **Step 5: Wire `feed_bytes` + `fire_timer`** — `cc_session_feed_bytes` now runs framer→`ProcessMessage`; `cc_session_fire_timer(CC_TIMER_ISIRCX_PROBE)` → `sock.HrModeIsIrcXFailure()` (plain-IRC fallback).

- [ ] **Step 6: Run to verify it passes** — `swift test 2>&1 | grep -i parse_join` PASS; full suite green.

- [ ] **Step 7: Add coverage vectors** — one selftest per event family with hand-verified expected streams: self-join+MODE+WHO auto-queries, NICK across users, TOPIC set, channel MODE delta, IRCX `DATA CCUDI1` out-of-band annotation (vs the inline plain-IRC form from Task 3), WHISPER inbound, `# Appears as` avatar announce, room LIST, MOTD, a 433 nick-collision, a fatal ERROR. Each documents which handler + line it exercises.

- [ ] **Step 8: Fidelity diff + commit** — diff `ircsock.cpp`/`ircsock.h` vs original; every R18 emit-replacement, R20 wrap, R21 drop individually listed; confirm the parse tables (`g_rgIrcCmd`, numerics) are verbatim.

```bash
git add macos/ComicChatKit/Sources/cchat-engine/engine/ircsock.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/ircsock.h \
        macos/ComicChatKit/Sources/cchat-engine/include/comicchat.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.cpp \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp
git commit -m "macos: Plan 3 Task 5 - inbound event vocabulary + parser lift (ircsock); SSPI dropped (R21), app-callees -> events (R18)"
```

---

### Task 6: Lift the payload second stage (`ProcessSay`/`ProcessComment`) + finish `protsupp`

Task 3 lifted the codec; Task 5 emits raw text/data events. This task lifts the payload interpreter that sits between them — CTCP dispatch, `#`-comment grammar, the inline-annotation parse block — extracting it from the UI/policy it's tangled with (R20), and wires the R19 resolver so talk-to decode works end-to-end.

**Files:**
- Modify: `Sources/cchat-engine/engine/protsupp.cpp`/`.h` — add `ProcessSay`, `ProcessComment`, `OnTextMsg`, `OnDataMsg`, CTCP `Reply*`/`Show*` pairs; extract the inline-annotation parse block; wire `LookupPui`→`ccSessionResolveUser` (R19)
- Modify: `Sources/cchat-engine/bridge/cc_session.cpp`/`.h` — implement `ccSessionResolveUser`/`ccSessionOwnNick` resolvers; delete the `GetMyNickName` link stub
- Modify: `Sources/cchat-engine/engine/cc_link_stubs.cpp` — remove `GetMyNickName()` stub (R19)
- Modify: `Sources/cchat-engine/bridge/cc_selftest.cpp` — end-to-end text→event-with-annotations + CTCP tests

**Interfaces:**
- Consumes: Task 3 codec, Task 5 events + `ccEmitProtoEvent`, Task 1 resolvers.
- Produces: `OnTextMsg`/`OnDataMsg` fully implemented (Task 5's emits route through here for CTCP/comment classification); `ccSessionResolveUser`; CTCP events (`CC_EV_ACTION`/`SOUND`/`AWAY_PEER`/`APPEARS_AS`) populated.

- [ ] **Step 1: Write the failing end-to-end CTCP + comment selftests** — feed a `\x01ACTION waves\x01`, a `# Appears as Anna.http://x/anna.avb`, and an inline `(#…) ` say; assert `CC_EV_ACTION`, `CC_EV_APPEARS_AS(name,url)`, `CC_EV_TEXT(has_annotations)` respectively, with a resolver that maps `"Bob"`→ref 7 and asserting the decoded `talkTos` came back as ref 7.

- [ ] **Step 2: Run to verify it fails** — expected: the events arrive as raw `CC_EV_TEXT` without CTCP/comment classification (Task 5 emitted them unparsed).

- [ ] **Step 3: Lift the payload stage into `protsupp.cpp`** — `OnTextMsg` (:4358), `OnDataMsg` (:4374), `ProcessSay` (:1545 — extract the codec/CTCP-dispatch core; leave ignore/flood/history/UI to R20 wraps or drop), `ProcessComment` (:846 — `#`-grammar; policy checks R20), the CTCP `Reply*`/`Show*` pairs (:1126-1163, 1626-1869). **Extraction**: the inline-annotation parse block (`protsupp.cpp:1566-1612`) duplicates `ProcessUDIData` (Task 3) — unify them (one decoder, two call sites) per `state-and-codec.md` §3.2. `IdentifyWhispers` (:1448) copies talk-to element-wise — wire through the resolver.

- [ ] **Step 4: Implement the resolvers** in `cc_session.cpp`: `ccSessionOwnNick()` → `ccSession()->cfg.own_nick(...)`; `ccSessionResolveUser(nick, room_token)` → `ccSession()->cfg.resolve_user(...)`. Replace every lifted `GetMyNickName()` call with `ccSessionOwnNick()` and every `LookupPui(nick,doc)` in the codec path with a resolver call returning `cc_user_ref` (the `(DWORD)pui` slot becomes the ref). Delete the `GetMyNickName()` stub from `cc_link_stubs.cpp` (R19).

- [ ] **Step 5: Route Task 5's text/data emits through the payload stage** — `HandleCommand`'s PRIVMSG/NOTICE/DATA cases call `OnTextMsg`/`OnDataMsg` (as the original did), which now do CTCP/comment classification and emit the *specific* event (`CC_EV_ACTION` etc.) rather than a raw `CC_EV_TEXT`. Plain says still emit `CC_EV_TEXT` with decoded annotations.

- [ ] **Step 6: Run to verify it passes** — the CTCP/comment/inline-annotation selftests green; full suite green.

- [ ] **Step 7: Fidelity diff + commit** — diff the newly-added `protsupp.cpp` functions vs original; the ProcessUDIData/inline-block unification noted as the one intentional structural merge (cite both original sites); every R20 wrap + R19 reroute listed.

```bash
git add macos/ComicChatKit/Sources/cchat-engine/engine/protsupp.cpp \
        macos/ComicChatKit/Sources/cchat-engine/engine/protsupp.h \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.cpp \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_session.h \
        macos/ComicChatKit/Sources/cchat-engine/engine/cc_link_stubs.cpp \
        macos/ComicChatKit/Sources/cchat-engine/bridge/cc_selftest.cpp
git commit -m "macos: Plan 3 Task 6 - payload stage (ProcessSay/ProcessComment/CTCP) + resolvers (R19), GetMyNickName stub removed"
```

---

### Task 7: Swift session-state model + `NWConnection` driver + event stream

Cross the boundary to Swift. This owns the canonical session state (per `state-and-codec.md` §1.4: nick, membership, user metadata, room props, whisper targets, connection status) that the engine deliberately does not, and turns the C event callbacks into an `AsyncStream`.

**Files:**
- Create: `Sources/ComicChatKit/ProtocolEvents.swift` — Swift value types mirroring `cc_proto_event`
- Create: `Sources/ComicChatKit/ProtocolSession.swift` — the `ProtocolSession` actor/class: owns `cc_session`, the `NWConnection`, the state model, the `AsyncStream<ProtocolEvent>`
- Create: `Sources/ComicChatKit/WireCodec.swift` — CP-1252 ↔ String display transcoding (spec §4.5)
- Modify: `Sources/ComicChatKit/Engine.swift` — expose the session type if the existing `Engine` façade should vend it

**Interfaces:**
- Consumes: the full `cc_session` C API (Tasks 1/4/5/6).
- Produces: `ProtocolSession` (connect/join/say/whisper/part/disconnect), `ProtocolSession.events: AsyncStream<ProtocolEvent>`, and the observable state model (`rooms`, `members`, `ownNick`).

- [ ] **Step 1: Write the failing Swift test** — `ProtocolSessionTests`, nested in the `.serialized` suite tree. Stand up a loopback: an in-process TCP server (or the rig's ngircd) the `ProtocolSession` connects to; drive a scripted server that sends `001`+`JOIN`+`PRIVMSG`; assert the `AsyncStream` yields `.loggedIn`, `.userJoined`, `.text(annotations:)` and that `session.members["#comicrig"]` contains the joined nick.

```swift
@Suite(.serialized) struct ProtocolSessionTests {
    @Test func joinAndReceiveAnnotatedMessage() async throws {
        let server = try LoopbackIRCServer()          // helper: scripts server lines
        let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "Anon", encoding: .cp1252)
        try await session.connect()
        var seen: [ProtocolEvent] = []
        let task = Task { for await ev in session.events { seen.append(ev); if seen.count >= 3 { break } } }
        try await session.join("#comicrig")
        server.send(":srv 001 Anon :Welcome",
                    ":Bob!bob@h JOIN :#comicrig",
                    ":Bob!bob@h PRIVMSG #comicrig :(#G295E193M1) hello")
        await task.value
        #expect(seen.contains { if case .loggedIn = $0 { true } else { false } })
        #expect(session.members["#comicrig"]?.contains("Bob") == true)
    }
}
```

- [ ] **Step 2: Run to verify it fails** — `swift test --filter ProtocolSession`. Expected: `ProtocolSession` undefined.

- [ ] **Step 3: Define `ProtocolEvent`** in `ProtocolEvents.swift` — a Swift enum with associated values mirroring `cc_proto_event` (strings decoded from CP-1252/UTF-8 via `WireCodec` at the boundary, so Swift consumers get `String`, not bytes). Annotations mirror `cc_annotations`.

- [ ] **Step 4: Implement `WireCodec`** — `func decode(_ bytes: UnsafePointer<CChar>) -> String` and `encode(_ s: String) -> [UInt8]` for CP-1252 (a 256-entry table) and UTF-8 (pass-through), selected by the session's `encoding`. Test both directions with a byte that differs between CP-1252 and UTF-8 (e.g. 0x93 “smart quote”).

- [ ] **Step 5: Implement `ProtocolSession`** — owns `cc_session` (config callbacks bridge to Swift closures via an `Unmanaged<AnyObject>` `user_data`); `send` writes to the `NWConnection`; `NWConnection`'s receive handler calls `cc_session_feed_bytes`; `set_timer`/`cancel_timer` schedule a `DispatchSourceTimer` that calls `cc_session_fire_timer`; `on_event` maps the C union → `ProtocolEvent`, updates the state model, and yields into the `AsyncStream`; `own_nick`/`resolve_user` answer from the Swift state model. The state model tracks exactly the `state-and-codec.md` §1.4 set (own nick with the echo-only update rule, per-room membership with soft-departs, user metadata, room props, whisper targets, connection status).

- [ ] **Step 6: Run to verify it passes** — `swift test --filter ProtocolSession`. Expected: PASS (loopback round-trips).

- [ ] **Step 7: Commit**

```bash
git add macos/ComicChatKit/Sources/ComicChatKit/ProtocolEvents.swift \
        macos/ComicChatKit/Sources/ComicChatKit/ProtocolSession.swift \
        macos/ComicChatKit/Sources/ComicChatKit/WireCodec.swift \
        macos/ComicChatKit/Sources/ComicChatKit/Engine.swift \
        macos/ComicChatKit/Tests/ComicChatKitTests/ProtocolSessionTests.swift
git commit -m "macos: Plan 3 Task 7 - Swift ProtocolSession (NWConnection driver, state model, AsyncStream events)"
```

---

### Task 8: Captured-bytes replay tests + annotation byte-compare (spec §8.2)

Wire the Wine rig's real captured sessions into the test suite: replay captured bytes through the parser and assert events; re-encode outbound annotations and byte-compare. This is the spec §8.2 acceptance for the test layer.

**Files:**
- Create: `Tests/ComicChatKitTests/Fixtures/captures/*.jsonl` — captured sessions (copied from `.superpowers/rig/captures/`, curated + committed as test fixtures)
- Create: `Tests/ComicChatKitTests/ProtocolParseTests.swift` — replay `c2s`/`s2c` streams, assert event sequences
- Create: `Tests/ComicChatKitTests/ProtocolCodecTests.swift` — decode captured annotation blobs, re-encode, byte-compare
- Create: `Tests/ComicChatKitTests/Support/CaptureReplay.swift` — parses the rig's JSONL capture format

**Interfaces:**
- Consumes: Task 7's `ProtocolSession` (feed replay bytes without a live socket) + the codec.
- Produces: the frozen replay corpus + byte-compare guarantees.

- [ ] **Step 1: Produce a curated capture corpus** — using the rig (`.superpowers/rig/run-rig.sh`), capture at minimum: connect+register (plain-IRC fallback), join, a plain say, an annotated say, a think, an action, a whisper, an avatar `# Appears as`. **NOTE the rig gap** (`wine-capture-rig.md`): the annotated-message captures need GUI interaction — if this session is headless, request the corpus from Tim (interactive) or drive a scripted second client. Do NOT block the parse-replay tests on it: the connect/join captures already exist (`smoke-2.jsonl`) and hand-authored annotated vectors (Tasks 3/5) cover the codec until real annotated captures land. Copy curated captures into `Fixtures/captures/`, redacting nothing (loopback only, no secrets).

- [ ] **Step 2: Write `CaptureReplay`** — parses the JSONL (`{t,dir,hex,latin1}`); yields `(dir, bytes)` in order. A `replay(_ capture:into:)` helper feeds all `s2c` bytes to a `ProtocolSession` (via a test seam that calls `cc_session_feed_bytes` directly, no socket) and collects the event stream + all `send`-callback bytes.

- [ ] **Step 3: Write the failing replay test** — replay `smoke-2.jsonl`; assert the event sequence includes `.loggedIn(nick: "Anonymous")`, a `.selfJoined("#comicrig")`, and that no unexpected `.error` events appear. (This capture is the real `cchat.exe` connect handshake.)

- [ ] **Step 4: Run to verify it fails, then passes** — the fixture + replay harness make it pass; if the parser diverges from the real client's expectations, the captured bytes expose it (that's the point).

- [ ] **Step 5: Write the annotation byte-compare test** — for each captured `c2s` `PRIVMSG` carrying a `(#…)`/`DATA CCUDI1` blob: decode it to `cc_annotations`, re-encode via `cc_test_encode_udi`, and **byte-compare** the re-encoded blob against the captured bytes (accounting for low-level unquoting — decode-then-compare on the unquoted form, per `ircproto-map.md` §5). Any mismatch is a codec-fidelity bug. (If no annotated `c2s` captures exist yet, this test is written against a hand-authored capture fixture and marked to strengthen when real captures land — documented, not silently skipped.)

- [ ] **Step 6: Commit**

```bash
git add macos/ComicChatKit/Tests/ComicChatKitTests/Fixtures/captures/ \
        macos/ComicChatKit/Tests/ComicChatKitTests/ProtocolParseTests.swift \
        macos/ComicChatKit/Tests/ComicChatKitTests/ProtocolCodecTests.swift \
        macos/ComicChatKit/Tests/ComicChatKitTests/Support/CaptureReplay.swift
git commit -m "macos: Plan 3 Task 8 - captured-bytes replay + annotation byte-compare (spec 8.2)"
```

---

### Task 9: End-to-end proof — a received annotated message renders as a comic panel

The exit milestone. Prove the two halves connect: a protocol event carrying annotations drives the existing `cc_strip` compose path to a rendered panel — the same proof-of-life shape Plan 2 ended on, now fed by the wire instead of a script.

**Files:**
- Create: `Sources/ComicChatKit/ProtocolStripBridge.swift` — turns a stream of `.text(annotations:)` / `.userJoined` events into `cc_strip_add_participant`/`cc_strip_add_line` calls, then `cc_strip_compose`
- Create: `Tests/ComicChatKitTests/ProtocolToStripTests.swift` — event stream → PNG, asserted
- Modify: `Sources/cc-dumpart/main.swift` (+ a new mode) — `cc-dumpart --replay <capture.jsonl> out.png` drives it from the CLI

**Interfaces:**
- Consumes: Task 7 events + the existing `cc_strip_*` API (Plan 2).
- Produces: the wire→comic path; a CLI replay-to-PNG; the exit-milestone test.

- [ ] **Step 1: Write the failing end-to-end test** — replay a capture (or hand-authored event stream) containing two participants and an annotated line; feed the events to `ProtocolStripBridge`; compose to a recording canvas; assert the draw-call stream contains the expected participants and a balloon with the message text (reuse the Plan 2 recording-canvas assertions).

- [ ] **Step 2: Run to verify it fails** — `ProtocolStripBridge` undefined.

- [ ] **Step 3: Implement `ProtocolStripBridge`** — map `.userJoined`/`.selfJoined` → `cc_strip_add_participant`; map `.text(nick:text:annotations:)` → resolve speaker index, translate `cc_annotations` (pose/emotion/mode/addressees) into the `cc_strip_add_line` parameters (the annotations carry exactly what `add_line` needs — pose/emotion drive the same textpose path); `.appearsAs` → set the participant's avatar. Compose on demand.

- [ ] **Step 4: Run to verify it passes** — recording-canvas assertions green.

- [ ] **Step 5: Add the CLI replay mode** — `cc-dumpart --replay <capture.jsonl> <out.png>`: parse the capture, feed `s2c` bytes to a `ProtocolSession`, bridge events to a strip, PNG-export. Run it against `smoke-2.jsonl` (connect/join only → an empty-or-titled strip) and, when available, an annotated capture → a real comic panel.

- [ ] **Step 6: Visual verification** — export a PNG from an annotated capture (or hand-authored event stream) and confirm (coordinator + Tim) it renders a readable comic panel with the right speaker/pose/balloon — the wire-fed equivalent of Plan 2's exit proof.

- [ ] **Step 7: Commit**

```bash
git add macos/ComicChatKit/Sources/ComicChatKit/ProtocolStripBridge.swift \
        macos/ComicChatKit/Tests/ComicChatKitTests/ProtocolToStripTests.swift \
        macos/ComicChatKit/Sources/cc-dumpart/main.swift
git commit -m "macos: Plan 3 Task 9 - wire->comic end-to-end (protocol events -> cc_strip); cc-dumpart --replay"
```

---

## Exit milestone

A real captured MS Chat / IRC session, replayed through the lifted parser, emits typed events that the Swift session model tracks and that drive the existing comic-strip compositor to a rendered panel — poses, balloons, and speakers correct — verified by (a) the captured-bytes replay + annotation byte-compare tests (spec §8.2) green, (b) the `ProtocolSession` loopback test green, and (c) a visually-verified PNG from `cc-dumpart --replay`. `CC_NO_PROTOCOL` is gone; the SSPI/identd/DBCS paths are dropped with recorded rulings; `swift test` fully green.

## Debt handed to Plan 4

- **Live interop acceptance** (spec §8 manual): connect to a hobbyist MS Chat server alongside the real Windows 2.5 client — poses/avatars/whispers both directions. Needs a live/IRCX server; the rig covers plain-IRC offline.
- **IRCX-only surface** exercised only by injection, not live capture (needs an IRCX server or shim): `800`/`AUTH`/`DATA`/`PROP`/`LISTX`/inbound `WHISPER`/extended nicks. Parser handles them (tested via hand-authored vectors); a live IRCX capture strengthens the byte-compare corpus.
- **Avatar download** (`# Appears as <url>` → fetch the `.avb`): the URL-handshake protocol lifts in this plan; the actual HTTP fetch is Swift `URLSession` (Plan 4, `webreq.cpp` replacement).
- **DCC file transfer** (`filesend.cpp`) — deferred; the CTCP `DCC SEND` parse is recognized but the transfer is not ported this plan.
- **`GetQualifiedName` R12(b) trap stub** — still owned by `userinfo.cpp` (not lifted); delete when `userinfo.cpp` fully lifts (its data half is already in `engine/userinfo.h`).
- **NetMeeting CTCP** (`\x01NETMEET\x01` + `ConferenceConnect`) — recognized, not wired to any macOS conferencing; dropped with a note.
- **Rules/notifications daemons** — the engine emits the events; running user-defined rules against them is a Plan 4 app-layer feature.

## Self-review (against the spec + discovery)

- **Spec §4.4 (bytes in, events out, Swift timers):** Tasks 1/5/7 — `feed_bytes`, `cc_proto_events`, `set_timer`/`fire_timer`, `send` callback ✓.
- **Spec §4.5 (CP-1252 default, UTF-8 opt-in, no JIS):** Task 2 (CP-1252 posture permanent), Task 7 (`WireCodec`, `encoding` config), JIS not ported ✓.
- **Spec §7 (permissive):** unknown verbs/numerics keep TRACE/status behavior (Task 5); tolerant unquoting reproduced (Task 2) ✓.
- **Spec §8.2 (record real sessions, replay, byte-compare annotations):** Tasks 8/9 + the Wine rig ✓; annotated-capture gap flagged with the hand-authored-vector fallback, not silently skipped.
- **Spec §9 (format.cpp/protsupp untangling risk):** de-risked by discovery; codec lift is Tasks 3/6, low-to-moderate ✓.
- **Roadmap Plan-3 debt:** `intl.c` posture decided (Task 2), `GetMyNickName` stub removed (Task 6), Capitalize restored (Task 2), talkTos-never-serialized confirmed (discovery; the codec uses nick strings + resolver — Tasks 3/6), `CharNext` shim live+selftested (Task 2), `format.cpp` codec half confirmed already-lifted (Task 3), `CC_NO_PROTOCOL` removed (Task 3) ✓.
- **Type consistency:** `cc_session`, `cc_session_config`, `cc_annotations`, `cc_proto_event`, `cc_user_ref`, `ccSession()`, `ccEmitProtoEvent()`, `ccSessionOwnNick()`, `ccSessionResolveUser()` used identically across Tasks 1–9 ✓.
- **Placeholder scan:** every code step shows code; event union variants are representative with "one struct per type" explicitly flagged for the implementer to complete from the enum (each enumerated), not a hidden TODO.
