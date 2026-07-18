import Foundation
import Network
import cchat_engine

/// The Swift side of Plan 3's protocol boundary: owns the `cc_session` C
/// handle, drives an `NWConnection` for the actual socket I/O, tracks the
/// canonical session state the engine deliberately does NOT own (own nick,
/// per-room membership, room properties, connection status —
/// state-and-codec.md §1.4), and turns the engine's C event callbacks into an
/// `AsyncStream<ProtocolEvent>`.
///
/// THREADING / SERIALIZATION (binding, mirrors comicchat.h's engine contract):
/// the C engine is single-threaded with process-global mutable state (a
/// file-static `g_session` set on entry to every `cc_session_*` call — see
/// `bridge/cc_session.cpp`). `ProtocolSession` therefore funnels EVERY call
/// into the engine — `cc_session_feed_bytes`, `cc_session_fire_timer`, every
/// `cc_session_join/send_say/...` outbound builder — through one dedicated
/// serial `DispatchQueue` (`sessionQueue`). This is a plain `DispatchQueue`
/// rather than a Swift `actor` deliberately: the C callbacks
/// (`send`/`set_timer`/`cancel_timer`/`on_event`/`own_nick`/`resolve_user`)
/// are plain `@convention(c)` function pointers that fire SYNCHRONOUSLY and
/// REENTRANTLY while a `cc_session_*` call is already executing on
/// `sessionQueue` — e.g. `cc_session_feed_bytes` can call `on_event` many
/// times before returning, and `cc_session_send_say` calls `own_nick`
/// mid-call. An `actor`'s isolated methods can only be entered by awaiting,
/// which does not compose with a synchronous C callback trampoline that must
/// run to completion on the SAME thread that is already inside the engine
/// call (re-entering via `Task { await ... }` from the callback would at best
/// reorder events and at worst deadlock against the actor's serial executor).
/// A serial `DispatchQueue` gives the same "one call at a time" guarantee
/// while letting the trampolines touch `self`'s state directly and
/// synchronously, because by construction every path that reaches a
/// `cc_session_*` call already holds the queue.
///
/// All state mutation (own nick, membership, room props, connection status,
/// the pending-UDI re-pairing table) happens only on `sessionQueue`. Public
/// async methods hop onto the queue via `withCheckedContinuation`/`queue.async`
/// and never touch engine or state-model memory from any other thread.
public final class ProtocolSession: @unchecked Sendable {
    // MARK: Public state model (state-and-codec.md §1.4)

    /// One joined room's tracked state. Membership uses "soft departs" (a
    /// departed user's entry is kept, matching the original's
    /// `UF_DEPARTED`-flag-not-removal behavior, state-and-codec.md §1.2) so a
    /// late-arriving message from someone who just parted can still resolve.
    public struct RoomMember: Sendable {
        public var nick: String
        public var ident: String = ""
        public var isOp: Bool = false
        public var isVoice: Bool = false
        public var isOwner: Bool = false
        public var isAway: Bool = false
        public var isIgnored: Bool = false
        public var avatarName: String = ""
        public var avatarURL: String = ""
        /// Soft-depart: the user parted/quit but the entry is retained.
        public var departed: Bool = false
    }

    public struct RoomState: Sendable {
        public var channel: String
        public var topic: String = ""
        public var modes: String = ""
        public var maxUsers: Int32 = 0
        public var key: String = ""
        /// `PROP CLIENT` keystring value(s), by key (e.g. "bk" backdrop).
        public var clientProps: [String: String] = [:]
        /// nick (as-seen-on-wire, decoded) -> member record.
        public var members: [String: RoomMember] = [:]
        public var roomToken: UInt32 = CC_ROOM_TOKEN_NONE
    }

    public enum ConnectionStatus: Sendable, Equatable {
        case disconnected
        case connecting
        /// TCP connected, IRC login not yet confirmed (no `CC_EV_LOGGED_IN` yet).
        case socketConnected
        /// `CC_EV_LOGGED_IN` received — outbound protocol calls are permitted.
        case connected
    }

    // MARK: Construction-time config (immutable, read from any thread)

    public let host: String
    public let port: NWEndpoint.Port
    public let requestedNick: String
    public let encoding: WireEncoding

    /// Upper bound (milliseconds) on the engine's requested ISIRCX-probe
    /// timeout (Plan 4a Task 2). The engine always asks for the original's
    /// 50s (`cc_session_probe_ircx`'s `cfg.set_timer(..., 50000)` call,
    /// matching the 1998 client's `ID_ISIRCXTIMEOUT`) -- this property lets
    /// Swift schedule a SHORTER wait instead (`min(requested, probeTimeoutMs)`
    /// in `setTimer(id:ms:)` below), so a dead-air/non-responding server
    /// doesn't hang a real session (or a test) for the full 50 seconds.
    /// Default 5000ms; set before `connect()` to change it (read only from
    /// `sessionQueue` internally, but safe to set from any thread before the
    /// first `connect()` call since nothing reads it until then).
    public var probeTimeoutMs: Int32 = 5000

    // MARK: Engine-owned state (mutated ONLY on sessionQueue)

    private var cSession: OpaquePointer?
    private var connection: NWConnection?
    private let sessionQueue: DispatchQueue

    /// Own nick, updated ONLY on the server's NICK echo (the "echo-only
    /// update rule", state-and-codec.md §1.2: `ChatChangeNick`/the request
    /// path does NOT update this locally — only `CC_EV_LOGGED_IN` (the 001
    /// welcome, which carries the server-confirmed actual nick) and
    /// `CC_EV_NICK_CHANGED` with `isSelf == true` do). Read by the
    /// `own_nick` C resolver.
    /// Own nick, updated ONLY on the server's NICK echo (see doc comment
    /// above `refillOwnNickBuffer()` near the bottom of this type for the
    /// persistent C-string buffer kept in sync with this property, which
    /// backs the `own_nick` C callback's return value).
    private var _ownNick: String = ""
    private var rooms: [String: RoomState] = [:]          // channel -> state
    private var roomTokenToChannel: [UInt32: String] = [:]
    private var connectionStatus: ConnectionStatus = .disconnected

    // MARK: Login sequencing (Plan 4a Task 2 -- probe -> 451/800-pivot/timeout -> NICK/USER)

    /// `true` from `onSocketReady()` (the probe was sent) until the plain
    /// login fires; guards the `cCancelTimer` trampoline's login trigger so
    /// it only fires the probe's own cancel (the 451 fallback), never an
    /// unrelated future `cancel_timer` call the engine might make for some
    /// other reason.
    private var probing = false
    /// One-shot guard: `sendLoginIfNeeded()` fires from up to three race-y
    /// triggers (451-cancel, timer-fire fallback, IRCX second-800) and must
    /// send NICK/USER exactly once.
    private var loginSent = false
    /// `true` once the FIRST `CC_EV_SERVER_CAPS` (the 800 pivot's state-0
    /// reply) has been observed. A SECOND `.serverCaps` arriving after that
    /// is the second-800 edge-trigger (see `sendLoginIfNeeded`'s doc comment,
    /// trigger 3) -- distinguishes "first 800, switch to IRCX" from "second
    /// 800, log in" without needing a new event type on the C side.
    private var sawServerCaps = false

    /// Per-user handle table for the `resolve_user` C resolver (R19): a
    /// stable, session-lifetime-unique `cc_user_ref` per (nick) — the engine
    /// never dereferences this value, only compares/threads it through, so a
    /// dense counter is sufficient (state-and-codec.md §2's "opaque per-user
    /// handle" recipe).
    private var userRefs: [String: cc_user_ref] = [:]
    private var nextUserRef: cc_user_ref = 1

    /// IRCX DATA→PRIVMSG annotation re-pairing (Task 6 review amendment,
    /// carried forward here — see the type's doc comment below for the full
    /// rationale). Keyed by nick; single-use (removed on consume).
    private var pendingUDI: [String: Annotations] = [:]

    // MARK: AsyncStream plumbing

    private var eventContinuation: AsyncStream<ProtocolEvent>.Continuation?
    /// The event stream. Multiple concurrent consumers are not supported —
    /// like the underlying `cc_session`, this models ONE session's event feed
    /// (create the stream once via the lazy `events` accessor pattern below).
    public let events: AsyncStream<ProtocolEvent>

    // MARK: Init / lifecycle

    /// - Parameter engineQueue: the serial queue every `cc_session_*` call is
    ///   funneled through (see the type's top doc comment). Defaults to a
    ///   private queue created here, matching every existing caller's
    ///   behavior. Pass an explicit queue to SHARE it with other engine work
    ///   that must serialize against this session's calls — e.g. a live app's
    ///   `ProtocolStripBridge` work, which also touches the same
    ///   process-global engine (comicchat.h's single-thread contract covers
    ///   every `cc_*` entry point, not just `cc_session_*`) and therefore must
    ///   run on this SAME queue rather than a queue of its own
    ///   (`performOnEngineQueue`/`enqueueEngineWork`, below, are how callers
    ///   reach it).
    public init(host: String, port: UInt16, nick: String,
                encoding: WireEncoding = .cp1252,
                engineQueue: DispatchQueue? = nil) {
        self.host = host
        self.port = NWEndpoint.Port(rawValue: port) ?? 6667
        self.requestedNick = nick
        self.encoding = encoding
        self.sessionQueue = engineQueue ?? DispatchQueue(label: "com.comicchat.ProtocolSession")
        var continuation: AsyncStream<ProtocolEvent>.Continuation!
        self.events = AsyncStream { cont in continuation = cont }
        self.eventContinuation = continuation
        self._ownNick = nick
        self.refillOwnNickBuffer()
    }

    deinit {
        eventContinuation?.finish()
        if let s = cSession {
            cc_session_destroy(s)
        }
        connection?.cancel()
        ownNickStorage.deallocate()
    }

    /// Connect the `NWConnection`, wait for it to become ready, then create
    /// the `cc_session` and start feeding received bytes into it. Throws if
    /// the connection fails before becoming ready.
    public func connect() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            sessionQueue.async { [self] in
                guard connectionStatus == .disconnected else {
                    continuation.resume(throwing: ProtocolSessionError.alreadyConnected)
                    return
                }
                connectionStatus = .connecting
                let params = NWParameters.tcp
                let conn = NWConnection(host: NWEndpoint.Host(host), port: port, using: params)
                self.connection = conn

                // `NWConnection.stateUpdateHandler` fires on the queue passed
                // to `start(queue:)` below (sessionQueue), so this closure
                // already runs serialized with everything else here -- no
                // need to re-hop via `sessionQueue.async` from inside it.
                // `ResumeOnce` (a tiny `@unchecked Sendable` box) satisfies
                // the compiler's Sendable-closure-capture check for the
                // "resume the continuation exactly once" flag; the actual
                // safety comes from serial-queue delivery, not from the box.
                let resumeGuard = ResumeOnce()
                conn.stateUpdateHandler = { [weak self] state in
                    guard let self else { return }
                    switch state {
                    case .ready:
                        if resumeGuard.markResumed() {
                            self.onSocketReady()
                            continuation.resume()
                        }
                    case .failed(let error):
                        self.connectionStatus = .disconnected
                        if resumeGuard.markResumed() {
                            continuation.resume(throwing: error)
                        } else {
                            self.emit(.disconnectedHint(text: "\(error)"))
                        }
                    case .cancelled:
                        self.connectionStatus = .disconnected
                    default:
                        break
                    }
                }
                conn.start(queue: sessionQueue)
            }
        }
    }

    /// Called on `sessionQueue` once the `NWConnection` is `.ready`: creates
    /// the `cc_session` (wiring the config callbacks) and starts the receive
    /// loop. Must run on `sessionQueue` (touches `cSession`/state).
    private func onSocketReady() {
        connectionStatus = .socketConnected

        var cfg = cc_session_config()
        cfg.user_data = Unmanaged.passUnretained(self).toOpaque()
        cfg.send = ProtocolSession.cSend
        cfg.set_timer = ProtocolSession.cSetTimer
        cfg.cancel_timer = ProtocolSession.cCancelTimer
        cfg.on_event = ProtocolSession.cOnEvent
        cfg.own_nick = ProtocolSession.cOwnNick
        cfg.resolve_user = ProtocolSession.cResolveUser
        cfg.local_host = nil
        cfg.encoding = encoding.rawValue

        cSession = withUnsafePointer(to: cfg) { cc_session_create($0) }
        receiveLoop()

        // Plan 4a Task 2: probe for IRCX before doing anything else -- every
        // session now sends "MODE ISIRCX" as its first outbound line (D4 §2's
        // hard prerequisite this task fixes: no code anywhere sent NICK/USER
        // before this). `probing = true` arms the 451-fallback/timeout login
        // triggers below; the engine itself requests the probe timer via
        // cfg.set_timer (capped by probeTimeoutMs in setTimer(id:ms:)).
        if let s = cSession {
            probing = true
            _ = cc_session_probe_ircx(s)
        }
    }

    /// Sends the plain-IRC login (NICK/USER) exactly once, guarded by
    /// `loginSent`. Called (on `sessionQueue`) from all three triggers the
    /// engine can produce (Plan 4a Task 2 Step 1's findings):
    ///   1. the 451 (ERR_NOTREGISTERED) fallback -- the engine's
    ///      `ccModeIsIrcXFailure` cancels the probe timer via
    ///      `cfg.cancel_timer`, observed in the `cCancelTimer` trampoline
    ///      while `probing` is still true.
    ///   2. the probe TIMEOUT -- `cc_session_fire_timer` runs
    ///      `ccModeIsIrcXFailure` the same way if the server never answers.
    ///   3. the IRCX PIVOT's second `800` reply -- Step 1 found the lifted
    ///      800-handler's anon-allowed branch emits NOTHING today (a gap:
    ///      the original's `HrIrcXLogin` fell straight through to
    ///      `HrIrcLogin` in the same call frame, which this port's split
    ///      engine/bridge architecture can't do without an explicit event).
    ///      This task closes that gap the same way every other lifted-parse
    ///      site does (R18): the second 800's anon-allowed branch now
    ///      re-emits `CC_EV_SERVER_CAPS` (ircsock.cpp's RPL_IRCX handler,
    ///      see that file's comment) purely as an edge-trigger -- so
    ///      `.serverCaps` arriving a SECOND time (after `probing` was
    ///      already cleared by the first one, below) is what fires login
    ///      here instead of a distinct event type.
    private func sendLoginIfNeeded() {
        guard !loginSent, let s = cSession else { return }
        loginSent = true
        probing = false
        timerSource?.cancel()
        timerSource = nil
        _ = cc_session_login(s)
    }

    private func receiveLoop() {
        guard let connection else { return }
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            self.sessionQueue.async {
                if let data, !data.isEmpty, let s = self.cSession {
                    data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
                        let ptr = raw.bindMemory(to: UInt8.self).baseAddress
                        cc_session_feed_bytes(s, ptr, data.count)
                    }
                }
                if let error {
                    self.connectionStatus = .disconnected
                    self.emit(.disconnectedHint(text: "\(error)"))
                    return
                }
                if isComplete {
                    self.connectionStatus = .disconnected
                    return
                }
                self.receiveLoop()
            }
        }
    }

    /// Disconnect: cancels the connection and any pending timer, tears down
    /// the `cc_session`, and marks the session disconnected. Safe to call
    /// more than once.
    public func disconnect() {
        sessionQueue.async { [self] in
            timerSource?.cancel()
            timerSource = nil
            connection?.cancel()
            connection = nil
            if let s = cSession {
                cc_session_destroy(s)
                cSession = nil
            }
            connectionStatus = .disconnected
        }
    }

    // MARK: Public read-only state accessors (state-and-codec.md §1.4)

    /// Own nick — updated ONLY via the echo-only rule (see `_ownNick`'s doc
    /// comment). Safe to read from any thread (queue-hops to read the
    /// authoritative value).
    public var ownNick: String {
        sessionQueue.sync { _ownNick }
    }

    /// nick set for a room (empty set if the room is unknown), INCLUDING
    /// soft-departed members — matching the loopback test's
    /// `session.members["#comicrig"]?.contains("Bob")` usage. Callers that
    /// need to exclude departed users should read `roomMembers(_:)` and
    /// filter on `departed`.
    public var members: [String: Set<String>] {
        sessionQueue.sync {
            rooms.mapValues { Set($0.members.keys) }
        }
    }

    /// Full per-room state snapshot.
    public func room(_ channel: String) -> RoomState? {
        sessionQueue.sync { rooms[channel] }
    }

    public var currentConnectionStatus: ConnectionStatus {
        sessionQueue.sync { connectionStatus }
    }

    // MARK: Engine queue access (Plan 4a Task 1 — shared session/strip queue)
    //
    // The C engine is process-global and single-threaded: EVERY `cc_*` entry
    // point (not just `cc_session_*`) shares one set of mutable statics
    // (comicchat.h's threading contract). Plan 3 only ever drove
    // `cc_strip_*`/`ProtocolStripBridge` work phase-separated from a session
    // (drain the full event stream first, THEN feed it to the bridge — see
    // `ProtocolStripBridge`'s doc comment). A live app cannot do that: it must
    // apply events to a strip WHILE the session is still connected, i.e.
    // interleaved with `cc_session_feed_bytes` and friends on the SAME
    // process. These two accessors expose `sessionQueue` under the "engine
    // queue" name so callers outside this type (a live app's
    // `ProtocolStripBridge` driver) can serialize their own engine-touching
    // work against this session's calls, rather than needing a second serial
    // queue of their own (which would NOT be safe — two independent serial
    // queues each individually serialize their own calls but do nothing to
    // prevent a `cc_strip_*` call on one queue from running concurrently with
    // a `cc_session_*` call on the other). `EngineInterleaveTests` proves this
    // is safe via a byte-exact PNG comparison against the Plan 3
    // drain-then-render reference.

    /// Run engine-touching work (`cc_strip_*`, `ProtocolStripBridge`,
    /// `Strip.compose`) synchronously, serialized with this session's
    /// `cc_session_*` calls. Blocks the calling thread until `body` completes.
    ///
    /// MUST NOT be called from inside an event-handling closure that is
    /// itself running on the engine queue (e.g. synchronously from within a
    /// `session.events` consumer callback that the engine queue is currently
    /// blocked waiting on) — `sessionQueue.sync` from a context already
    /// running on `sessionQueue` deadlocks. Event consumers run on their own
    /// `Task` (reading `session.events`, an `AsyncStream`, off the queue) and
    /// hop here via a plain call, which is the supported shape and what
    /// `EngineInterleaveTests` exercises.
    public func performOnEngineQueue<T>(_ body: () throws -> T) rethrows -> T {
        dispatchPrecondition(condition: .notOnQueue(sessionQueue))
        return try sessionQueue.sync(execute: body)
    }

    /// Enqueue engine-touching work to run after any in-flight `cc_session_*`
    /// call (and anything already queued) completes, without blocking the
    /// caller. Safe to call from anywhere, including from inside an event
    /// handler that is itself running on the engine queue (unlike
    /// `performOnEngineQueue`, this never waits on the queue it schedules
    /// onto).
    public func enqueueEngineWork(_ body: @escaping @Sendable () -> Void) {
        sessionQueue.async(execute: body)
    }

    // MARK: Outbound commands
    //
    // CX_DISCONNECTED GUARD (Task 4 carry-forward #2, re-added here): the
    // original's `bExecuteQuery` refused to build/send outbound query
    // commands when `GetConnectionStatus() == CX_DISCONNECTED`
    // (state-and-codec.md §1.2's connection-status row). Task 4's C lift
    // documented dropping that guard (no connection tracking at the C layer)
    // and flagged Task 7 to re-add the equivalent check — done here: every
    // outbound method below requires `connectionStatus == .connected` (i.e.
    // login has been confirmed via `CC_EV_LOGGED_IN`) before it will call
    // into any `cc_session_*` builder. `join` is the one exception worth
    // noting: real IRC allows JOIN as soon as the socket is registered
    // (post-001), which is exactly what `.connected` represents here, so the
    // same gate applies uniformly.

    public enum ProtocolSessionError: Error, CustomStringConvertible {
        case alreadyConnected
        case notConnected
        case commandFailed(String)
        public var description: String {
            switch self {
            case .alreadyConnected: return "ProtocolSession is already connected"
            case .notConnected: return "not connected (CX_DISCONNECTED guard)"
            case .commandFailed(let m): return m
            }
        }
    }

    /// Join `channel`. Registers the room-token<->channel mapping BEFORE
    /// sending the wire JOIN command (rather than reactively on the inbound
    /// join-confirm): the engine's inbound JOIN handler resolves
    /// `room_token` via a synchronous, read-only scan of its own
    /// already-registered channel table (`ccSessionRoomTokenForChannel`,
    /// ircsock.cpp) — it does NOT register new entries itself. If Swift
    /// waited to register until AFTER seeing `CC_EV_SELF_JOINED`, that very
    /// event would arrive with `room_token == CC_ROOM_TOKEN_NONE` (unresolvable,
    /// since nothing had registered the channel yet) and there would be no
    /// second chance — `CC_EV_USER_JOINED`/etc. carry no channel-name field to
    /// recover from. Registering the moment we know we're ABOUT to request
    /// this channel (mirroring a real client, which always knows what it's
    /// asking to join) is therefore the only place this can correctly happen.
    /// A caller that re-joins an already-registered channel is a no-op here
    /// (the existing token is reused; `cc_session_register_room` is not
    /// called again — tokens are never invalidated for a channel's lifetime
    /// in this session, per `cc_session.h`'s documented scheme).
    public func join(_ channel: String, key: String? = nil) async throws {
        try await onQueueGated { s in
            if self.roomTokenFor(channel) == nil {
                let token = channel.withCString { cc_session_register_room(s, $0) }
                self.roomTokenToChannel[token] = channel
                var state = self.rooms[channel] ?? RoomState(channel: channel)
                state.roomToken = token
                self.rooms[channel] = state
            }
            let rc = channel.withCString { chanPtr in
                if let key { return key.withCString { cc_session_join(s, chanPtr, $0) } }
                return cc_session_join(s, chanPtr, nil)
            }
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("join(\(channel)) failed") }
        }
    }

    public func part(_ channel: String, reason: String? = nil) async throws {
        try await onQueueGated { s in
            guard let token = self.roomTokenFor(channel) else {
                throw ProtocolSessionError.commandFailed("part: unknown channel \(channel)")
            }
            let rc = (reason.map { r in r.withCString { cc_session_part(s, token, $0) } })
                ?? cc_session_part(s, token, nil)
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("part(\(channel)) failed") }
        }
    }

    public func changeNick(_ newNick: String) async throws {
        try await onQueueGated { s in
            let rc = newNick.withCString { cc_session_change_nick(s, $0) }
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("changeNick failed") }
        }
    }

    public func setTopic(_ channel: String, topic: String) async throws {
        try await onQueueGated { s in
            guard let token = self.roomTokenFor(channel) else {
                throw ProtocolSessionError.commandFailed("setTopic: unknown channel \(channel)")
            }
            let rc = topic.withCString { cc_session_set_topic(s, token, $0) }
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("setTopic failed") }
        }
    }

    public func say(_ channel: String, text: String, annotations: Annotations? = nil,
                    modes: UInt16 = UInt16(CC_MODE_SAY)) async throws {
        try await onQueueGated { s in
            guard let token = self.roomTokenFor(channel) else {
                throw ProtocolSessionError.commandFailed("say: unknown channel \(channel)")
            }
            let rc: Int32
            if let annotations {
                var cAnn = annotations.toCAnnotations(encoding: self.encoding)
                rc = withUnsafePointer(to: &cAnn) { annPtr in
                    text.withCString { textPtr in
                        cc_session_send_say(s, token, annPtr, textPtr, modes)
                    }
                }
            } else {
                rc = text.withCString { textPtr in
                    cc_session_send_say(s, token, nil, textPtr, modes)
                }
            }
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("say failed") }
        }
    }

    /// Outbound avatar announce ("# Appears as <name>" / "# Appears as
    /// <name>.<url>", `cc_session_announce_avatar` — see that C function's
    /// comicchat.h doc comment for the full grammar/lift rationale). `toNick`
    /// nil announces channel-wide; non-nil sends a private reply-announce to
    /// that nick (the "someone appeared, tell them who we are back" case
    /// Task 9 drives from a `.appearsAs` event for an unseen nick).
    ///
    /// `name`/`url` are encoded per `encoding` (mirrors `say`'s posture: wire
    /// text is CP-1252 or UTF-8 per the session's configured encoding, NOT
    /// assumed-ASCII `String.withCString` — an avatar name containing e.g. a
    /// CP-1252-only curly quote must round-trip correctly here exactly as
    /// `_ownNick`/addressee strings do via `WireCodec`, see
    /// `refillOwnNickBuffer`/`ProtocolEvents.swift`'s own encode call sites).
    public func announceAvatar(channel: String, toNick: String? = nil, name: String, url: String? = nil) async throws {
        try await onQueueGated { s in
            guard let token = self.roomTokenFor(channel) else {
                throw ProtocolSessionError.commandFailed("announceAvatar: unknown channel \(channel)")
            }
            let rc = self.withEncodedCString(name) { namePtr in
                self.withOptionalEncodedCString(toNick) { toNickPtr in
                    self.withOptionalEncodedCString(url) { urlPtr in
                        cc_session_announce_avatar(s, token, toNickPtr, namePtr, urlPtr)
                    }
                }
            }
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("announceAvatar failed") }
        }
    }

    /// Encodes `s` per `encoding` into a temporary NUL-terminated buffer and
    /// hands `body` a pointer valid for the closure's duration — the
    /// `withCString`-shaped equivalent of `WireCodec.encode`, needed because
    /// plain `String.withCString` always encodes as UTF-8 regardless of the
    /// session's `encoding` (see `announceAvatar`'s doc comment).
    private func withEncodedCString<R>(_ s: String, _ body: (UnsafePointer<CChar>) -> R) -> R {
        var bytes = WireCodec.encode(s, encoding: encoding)
        bytes.append(0)
        return bytes.withUnsafeBufferPointer { buf in
            body(buf.baseAddress!.withMemoryRebound(to: CChar.self, capacity: buf.count) { $0 })
        }
    }

    /// `withEncodedCString`, but passes `nil` straight through for a `nil` input.
    private func withOptionalEncodedCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
        guard let s else { return body(nil) }
        return withEncodedCString(s) { body($0) }
    }

    public func whisper(to nicks: [String], text: String, channel: String,
                        annotations: Annotations? = nil) async throws {
        try await onQueueGated { s in
            guard let token = self.roomTokenFor(channel) else {
                throw ProtocolSessionError.commandFailed("whisper: unknown channel \(channel)")
            }
            var cAnn = annotations?.toCAnnotations(encoding: self.encoding)
            let rc: Int32 = withCStringArray(nicks) { cNicks, count in
                text.withCString { textPtr in
                    if cAnn != nil {
                        return withUnsafePointer(to: &cAnn!) { annPtr in
                            cc_session_send_whisper(s, token, annPtr, textPtr, cNicks, count)
                        }
                    } else {
                        return cc_session_send_whisper(s, token, nil, textPtr, cNicks, count)
                    }
                }
            }
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("whisper failed") }
        }
    }

    public func who(_ mask: String? = nil) async throws {
        try await onQueueGated { s in
            let rc = (mask.map { m in m.withCString { cc_session_who(s, $0) } }) ?? cc_session_who(s, nil)
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("who failed") }
        }
    }

    public func list(_ query: String? = nil) async throws {
        try await onQueueGated { s in
            let rc = (query.map { q in q.withCString { cc_session_list(s, $0) } }) ?? cc_session_list(s, nil)
            guard rc == 0 else { throw ProtocolSessionError.commandFailed("list failed") }
        }
    }

    /// Run `body` on `sessionQueue` with the CX_DISCONNECTED guard applied:
    /// throws `.notConnected` instead of touching `cSession` if the session
    /// isn't in the `.connected` state. `body` runs synchronously on the
    /// queue and may throw.
    private func onQueueGated(_ body: @escaping @Sendable (OpaquePointer) throws -> Void) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            sessionQueue.async { [self] in
                do {
                    guard connectionStatus == .connected, let s = cSession else {
                        throw ProtocolSessionError.notConnected
                    }
                    try body(s)
                    continuation.resume()
                } catch {
                    continuation.resume(throwing: error)
                }
            }
        }
    }

    private func roomTokenFor(_ channel: String) -> UInt32? {
        let token = rooms[channel]?.roomToken
        return (token == nil || token == CC_ROOM_TOKEN_NONE) ? nil : token
    }

    // MARK: Timer (set_timer/cancel_timer -> DispatchSourceTimer)

    private var timerSource: DispatchSourceTimer?

    /// Called (on `sessionQueue`) from the `set_timer` C callback. Schedules
    /// a one-shot `DispatchSourceTimer` on `sessionQueue` that, on fire,
    /// calls `cc_session_fire_timer` — the engine REQUESTS, Swift SCHEDULES
    /// (R21-adjacent; matches `cc_session_probe_ircx`'s doc comment). Only
    /// one timer id is used today (`CC_TIMER_ISIRCX_PROBE`); a new
    /// `set_timer` call for the same id replaces any existing one.
    private func setTimer(id: Int32, ms: Int32) {
        timerSource?.cancel()
        // Plan 4a Task 2: the ISIRCX probe requests the original's 50s
        // (CC_TIMER_ISIRCX_PROBE, cc_session_probe_ircx); cap it at
        // `probeTimeoutMs` so a non-responding server doesn't hang the
        // login sequence (or a test) for the full 50 seconds. Any other
        // future timer id is scheduled as requested, uncapped.
        let effectiveMs = (id == CC_TIMER_ISIRCX_PROBE) ? min(ms, probeTimeoutMs) : ms
        let src = DispatchSource.makeTimerSource(queue: sessionQueue)
        src.schedule(deadline: .now() + .milliseconds(Int(effectiveMs)))
        src.setEventHandler { [weak self] in
            guard let self, let s = self.cSession else { return }
            cc_session_fire_timer(s, id)
            // Plan 4a Task 2 trigger 2: the probe timeout. cc_session_fire_timer
            // runs the engine's ccFireIsIrcXTimeout -> ccModeIsIrcXFailure,
            // which (like the 451 path) cancels this very timer via
            // cfg.cancel_timer -- see the cCancelTimer trampoline for trigger
            // 1. Both paths converge on sendLoginIfNeeded's one-shot guard,
            // so calling it again here (belt-and-suspenders, in case a
            // future engine change stops cancelling on timeout) is harmless.
            if id == CC_TIMER_ISIRCX_PROBE, self.probing {
                self.sendLoginIfNeeded()
            }
        }
        timerSource = src
        src.resume()
    }

    private func cancelTimer(id: Int32) {
        timerSource?.cancel()
        timerSource = nil
        // Plan 4a Task 2 trigger 1: the 451 (ERR_NOTREGISTERED) fallback.
        // The engine's ccModeIsIrcXFailure cancels the probe timer the
        // moment it decides plain-IRC login is needed (dequeuing the probe
        // query first) -- `probing` distinguishes this from any unrelated
        // future cancel_timer call.
        if id == CC_TIMER_ISIRCX_PROBE, probing {
            sendLoginIfNeeded()
        }
    }

    // MARK: send (-> NWConnection)

    /// Called (on `sessionQueue`) from the `send` C callback: writes `data`
    /// to the `NWConnection`. Fire-and-forget (matching the original's
    /// unbuffered blocking-socket-equivalent posture) — send failures surface
    /// as a connection-state change via `stateUpdateHandler`, not here.
    private func sendBytes(_ data: Data) {
        connection?.send(content: data, completion: .contentProcessed { _ in })
    }

    // MARK: own_nick / resolve_user (R19 resolvers, answer from Swift state)

    private func resolveUser(nick: String, roomToken: UInt32) -> cc_user_ref {
        if let existing = userRefs[nick] { return existing }
        let ref = nextUserRef
        nextUserRef += 1
        userRefs[nick] = ref
        return ref
    }

    // MARK: Event handling: C union -> ProtocolEvent, state update, re-pairing, yield

    /// Called (on `sessionQueue`) from the `on_event` C callback.
    private func handleEvent(_ ev: cc_proto_event) {
        guard let (event, roomToken) = ProtocolEvent.from(ev, encoding: encoding) else { return }
        applyStateUpdate(event, roomToken: roomToken)

        // --- IRCX DATA->PRIVMSG annotation re-pairing (carry-forward #1) ---
        // The engine is stateless across messages: it emits CC_EV_DATA{nick,
        // annotations} for the "DATA <target> CCUDI1 :#G..." line, then
        // CC_EV_TEXT{nick, has_annotations=false} for the paired plain
        // PRIVMSG that follows. ProtocolSession re-pairs BY NICK: stash the
        // annotations from CC_EV_DATA under that nick; the next
        // text/whisper/action from the SAME nick with has_annotations==false
        // consumes (and clears) the pending slot -- single-use, exactly like
        // the original's m_bbValidUDI flag (state-and-codec.md §3.3).
        // Plain-IRC inline "(#...)" annotations already arrive with
        // has_annotations==true and pass through untouched below.
        switch event {
        case .data(let nick, let annotations):
            pendingUDI[nick] = annotations
            // CC_EV_DATA itself is engine plumbing, not a user-facing event —
            // the paired CC_EV_TEXT (now carrying the re-attached
            // annotations) is what Swift consumers should see. Still emit it
            // so consumers that want the raw feed can see the pairing landed
            // (documented choice: cheap to emit, easy to filter, and useful
            // for debugging the re-pairing itself).
            emit(event)

        case .text(let nick, let ident, let target, let text, let kind, let annotations):
            if annotations == nil, let pending = pendingUDI.removeValue(forKey: nick) {
                emit(.text(nick: nick, ident: ident, target: target, text: text,
                          kind: kind, annotations: pending))
            } else {
                emit(event)
            }

        case .whisper(let nick, let ident, let text, let annotations):
            if annotations == nil, let pending = pendingUDI.removeValue(forKey: nick) {
                emit(.whisper(nick: nick, ident: ident, text: text, annotations: pending))
            } else {
                emit(event)
            }

        case .action(let nick, let text, let annotations):
            if annotations == nil, let pending = pendingUDI.removeValue(forKey: nick) {
                emit(.action(nick: nick, text: text, annotations: pending))
            } else {
                emit(event)
            }

        default:
            emit(event)
        }
    }

    /// Update the canonical state model (state-and-codec.md §1.4) from an
    /// inbound event, BEFORE yielding it. Keeps `rooms`/`_ownNick`/
    /// `connectionStatus` authoritative for the `own_nick`/`resolve_user`
    /// resolvers and for the public accessors.
    private func applyStateUpdate(_ event: ProtocolEvent, roomToken: UInt32) {
        switch event {
        case .loggedIn(let nick):
            // Echo-only update rule: this IS the server's confirmation of our
            // actual nick (the 001 welcome), so (and only so) we adopt it.
            _ownNick = nick
            refillOwnNickBuffer()
            connectionStatus = .connected

        case .serverCaps:
            // Plan 4a Task 2 trigger 3 (the IRCX pivot): the FIRST
            // CC_EV_SERVER_CAPS is the 800 state-0 reply -- the engine has
            // already sent "IRCX" itself (ircsock.cpp's RPL_IRCX handler,
            // unconditional on the parse side); no login yet. A SECOND
            // occurrence is this task's edge-trigger for the 800 state-1
            // reply (see `sendLoginIfNeeded`'s doc comment) -- anon-allowed
            // login proceeds now, matching the verified real-server sequence
            // (docs/superpowers/plans/2026-07-18-plan4-discovery/
            // live-interop.md: "IRCX -> second 800 * 1 ...; plain NICK/USER").
            if sawServerCaps {
                sendLoginIfNeeded()
            } else {
                sawServerCaps = true
            }

        case .nickChanged(let oldNick, let newNick, let isSelf):
            if isSelf {
                _ownNick = newNick   // echo-only rule: server-confirmed rename
                refillOwnNickBuffer()
            }
            for key in rooms.keys {
                if var member = rooms[key]!.members.removeValue(forKey: oldNick) {
                    member.nick = newNick
                    rooms[key]!.members[newNick] = member
                }
            }
            if let ref = userRefs.removeValue(forKey: oldNick) {
                userRefs[newNick] = ref
            }

        case .selfJoined(let channel):
            // The room_token<->channel mapping is registered proactively in
            // `join()` BEFORE the wire JOIN is even sent (see that method's
            // doc comment for why it can't be done reactively here) — by the
            // time this event arrives, `rooms[channel]` already exists with
            // its token set. Guard against a server confirming a join we
            // never registered (shouldn't happen; defensive only) by falling
            // back to `roomToken` off the event itself if the local lookup
            // is somehow empty.
            var state = rooms[channel] ?? RoomState(channel: channel, roomToken: roomToken)
            if roomTokenToChannel[roomToken] == nil {
                roomTokenToChannel[roomToken] = channel
            }
            var me = state.members[_ownNick] ?? RoomMember(nick: _ownNick)
            me.departed = false
            state.members[_ownNick] = me
            rooms[channel] = state

        case .selfParted(let channel):
            if var state = rooms[channel] {
                if var me = state.members[_ownNick] {
                    me.departed = true
                    state.members[_ownNick] = me
                }
                rooms[channel] = state
            }

        case .userJoined(let nick, let ident):
            guard let channel = roomTokenToChannel[roomToken] else { break }
            var state = rooms[channel] ?? RoomState(channel: channel, roomToken: roomToken)
            var member = state.members[nick] ?? RoomMember(nick: nick)
            member.ident = ident
            member.departed = false
            state.members[nick] = member
            rooms[channel] = state

        case .userParted(let nick, _), .userQuit(let nick, _):
            // Soft-depart: keep the entry (state-and-codec.md §1.2's
            // documented `UF_DEPARTED`-flag behavior), just mark it departed.
            for key in rooms.keys {
                if var member = rooms[key]!.members[nick] {
                    member.departed = true
                    rooms[key]!.members[nick] = member
                }
            }

        case .kicked(_, let kickee, _, let channel):
            if var state = rooms[channel], var member = state.members[kickee] {
                member.departed = true
                state.members[kickee] = member
                rooms[channel] = state
            }

        case .names(let channel, let nicks):
            var state = rooms[channel] ?? RoomState(channel: channel, roomToken: roomToken)
            for nick in nicks {
                var cleanNick = nick
                // NAMES prefixes op/voice with '@'/'+' (mirrors the original's
                // NAMES-353 handling, state-and-codec.md §1.2 membership row).
                var isOp = false, isVoice = false
                if cleanNick.hasPrefix("@") { isOp = true; cleanNick.removeFirst() }
                else if cleanNick.hasPrefix("+") { isVoice = true; cleanNick.removeFirst() }
                guard !cleanNick.isEmpty else { continue }
                var member = state.members[cleanNick] ?? RoomMember(nick: cleanNick)
                member.isOp = member.isOp || isOp
                member.isVoice = member.isVoice || isVoice
                member.departed = false
                state.members[cleanNick] = member
            }
            rooms[channel] = state

        case .topicChanged(let channel, let topic):
            var state = rooms[channel] ?? RoomState(channel: channel, roomToken: roomToken)
            state.topic = topic
            rooms[channel] = state

        case .channelMode(let channel, let modes, _):
            var state = rooms[channel] ?? RoomState(channel: channel, roomToken: roomToken)
            state.modes = modes
            rooms[channel] = state

        case .userMode(let nick, let modes):
            for key in rooms.keys {
                guard var member = rooms[key]!.members[nick] else { continue }
                if modes.contains("o") { member.isOp = !modes.hasPrefix("-") }
                if modes.contains("v") { member.isVoice = !modes.hasPrefix("-") }
                rooms[key]!.members[nick] = member
            }

        case .roomProp(let key, let value):
            guard let channel = roomTokenToChannel[roomToken] else { break }
            var state = rooms[channel] ?? RoomState(channel: channel, roomToken: roomToken)
            state.clientProps[key] = value
            rooms[channel] = state

        case .appearsAs(let nick, let avatarName, let url):
            for key in rooms.keys {
                guard var member = rooms[key]!.members[nick] else { continue }
                member.avatarName = avatarName
                member.avatarURL = url
                rooms[key]!.members[nick] = member
            }

        case .awayPeer(let nick, _):
            for key in rooms.keys {
                guard var member = rooms[key]!.members[nick] else { continue }
                member.isAway = true
                rooms[key]!.members[nick] = member
            }

        case .disconnectedHint:
            connectionStatus = .disconnected

        default:
            break
        }
    }

    /// Yield an event into the `AsyncStream`. Must run on `sessionQueue`.
    private func emit(_ event: ProtocolEvent) {
        eventContinuation?.yield(event)
    }

    // MARK: C trampolines (must be plain @convention(c): no captured context)
    //
    // Every trampoline recovers `self` via `Unmanaged.fromOpaque(user_data)`
    // (passUnretained at cc_session_config construction time — self outlives
    // the session via the `deinit`-ordered `cc_session_destroy` call, so an
    // unretained pointer is safe and avoids a retain cycle). Each one runs
    // ON sessionQueue already (every call chain that reaches the engine
    // started there), so these touch `self`'s state directly rather than
    // re-dispatching (re-dispatching would break the synchronous,
    // same-thread-reentrancy contract the engine depends on -- see the type's
    // top doc comment).

    private static let cSend: cc_send_fn = { userData, data, len in
        guard let userData, let data else { return }
        let session = Unmanaged<ProtocolSession>.fromOpaque(userData).takeUnretainedValue()
        let bytes = Data(bytes: data, count: len)
        session.sendBytes(bytes)
    }

    private static let cSetTimer: cc_set_timer_fn = { userData, timerId, ms in
        guard let userData else { return }
        let session = Unmanaged<ProtocolSession>.fromOpaque(userData).takeUnretainedValue()
        session.setTimer(id: timerId, ms: ms)
    }

    private static let cCancelTimer: cc_cancel_timer_fn = { userData, timerId in
        guard let userData else { return }
        let session = Unmanaged<ProtocolSession>.fromOpaque(userData).takeUnretainedValue()
        session.cancelTimer(id: timerId)
    }

    private static let cOnEvent: cc_on_event_fn = { userData, ev in
        guard let userData, let ev else { return }
        let session = Unmanaged<ProtocolSession>.fromOpaque(userData).takeUnretainedValue()
        session.handleEvent(ev.pointee)
    }

    private static let cOwnNick: cc_own_nick_fn = { userData in
        guard let userData else { return ownNickFallback }
        let session = Unmanaged<ProtocolSession>.fromOpaque(userData).takeUnretainedValue()
        // Runs synchronously on `sessionQueue` (the engine call that reaches
        // this resolver was itself made from that queue); `ownNickPointer`
        // reads the persistent buffer that `refillOwnNickBuffer()` (also
        // always called on `sessionQueue`, at the point `_ownNick` changes)
        // last wrote. No lock needed: both sides are already serialized.
        return session.ownNickPointer()
    }

    private static let cResolveUser: cc_resolve_user_fn = { userData, nick, roomToken in
        guard let userData, let nick else { return CC_USER_REF_NONE }
        let session = Unmanaged<ProtocolSession>.fromOpaque(userData).takeUnretainedValue()
        let nickStr = WireCodec.decode(nick, encoding: session.encoding)
        return session.resolveUser(nick: nickStr, roomToken: roomToken)
    }

    /// Never-freed empty C string for the (should-never-happen) nil-userData
    /// fallback in `cOwnNick`. `nonisolated(unsafe)` is correct here: the
    /// buffer is allocated once, written once (to a NUL byte), and never
    /// mutated again — there is no data race to guard against, only Swift 6's
    /// blanket "raw pointers aren't Sendable" rule to satisfy.
    private static nonisolated(unsafe) let ownNickFallback: UnsafePointer<CChar> = {
        let buf = UnsafeMutablePointer<CChar>.allocate(capacity: 1)
        buf[0] = 0
        return UnsafePointer(buf)
    }()

    /// Manually-allocated, NUL-terminated C-string buffer holding `_ownNick`
    /// (encoded per `encoding`) — the storage the `own_nick` C callback's
    /// pointer points into. Unlike a Swift `[CChar]`/`Array`, whose backing
    /// store is copy-on-write and may be reallocated by ARC/the optimizer at
    /// any time (making a pointer obtained via `withUnsafeBufferPointer` and
    /// returned past the closure's end formal undefined behavior — the bug
    /// this fixes), a manually `allocate`d buffer has a fixed address for its
    /// lifetime: it is only ever freed by us, on our own schedule, so a
    /// pointer into it stays valid across the synchronous C read (and,
    /// deliberately, across calls in general — same precedent/lifetime
    /// argument as `ownNickFallback` above). Grown (reallocated) only when
    /// the encoded nick no longer fits; never reallocated on a mere read.
    private var ownNickStorage: UnsafeMutablePointer<CChar> = {
        let buf = UnsafeMutablePointer<CChar>.allocate(capacity: 1)
        buf[0] = 0
        return buf
    }()
    private var ownNickStorageCapacity: Int = 1

    /// Re-encodes `_ownNick` into `ownNickStorage`, growing the allocation
    /// first if needed. Must be called on `sessionQueue`, and ONLY at the
    /// point `_ownNick` is mutated (construction, `.loggedIn`,
    /// `.nickChanged(isSelf: true)`) — NOT on every `own_nick` read, which
    /// just returns the already-current buffer via `ownNickPointer()`.
    private func refillOwnNickBuffer() {
        var bytes = WireCodec.encode(_ownNick, encoding: encoding)
        bytes.append(0)
        if bytes.count > ownNickStorageCapacity {
            ownNickStorage.deallocate()
            ownNickStorage = .allocate(capacity: bytes.count)
            ownNickStorageCapacity = bytes.count
        }
        for (i, byte) in bytes.enumerated() {
            ownNickStorage[i] = CChar(bitPattern: byte)
        }
    }

    /// Returns the stable pointer into `ownNickStorage` for the `own_nick` C
    /// callback. Must be called on `sessionQueue` (same queue `_ownNick`'s
    /// mutations and `refillOwnNickBuffer()` run on), matching the engine's
    /// synchronous same-frame read contract.
    private func ownNickPointer() -> UnsafePointer<CChar> {
        UnsafePointer(ownNickStorage)
    }
}

/// A tiny "resume exactly once" latch for `connect()`'s continuation. Marked
/// `@unchecked Sendable` because its actual safety comes from being touched
/// only from `NWConnection.stateUpdateHandler` callbacks, which are all
/// delivered serialized on the same `DispatchQueue` (the one passed to
/// `start(queue:)`) — never because of anything this class does internally.
private final class ResumeOnce: @unchecked Sendable {
    private var resumed = false
    /// Returns `true` the first time it's called, `false` every time after.
    func markResumed() -> Bool {
        if resumed { return false }
        resumed = true
        return true
    }
}

/// Bridges a `[String]` to a `const char* const*` (an array of C string
/// pointers) for the duration of `body`, matching `cc_session_send_whisper`'s
/// `nicks`/`nick_count` parameters.
private func withCStringArray<R>(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>?, Int32) -> R) -> R {
    if strings.isEmpty { return body(nil, 0) }
    var cStrings: [UnsafePointer<CChar>?] = []
    func recurse(_ index: Int) -> R {
        if index == strings.count {
            return cStrings.withUnsafeBufferPointer { buf in
                body(buf.baseAddress, Int32(strings.count))
            }
        }
        return strings[index].withCString { ptr in
            cStrings.append(ptr)
            return recurse(index + 1)
        }
    }
    return recurse(0)
}
