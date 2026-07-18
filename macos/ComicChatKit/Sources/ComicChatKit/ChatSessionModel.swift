import Foundation
import CoreGraphics
import cchat_engine

/// Connection/persona/comic configuration for one `ChatSessionModel` session
/// (Plan 4a Task 9). `characterName`/`backdropName` are bare comicart names
/// (no directory, no extension — e.g. "anna", "field"); `artDir` is resolved
/// against them (`artDir + "/" + name + ".avb"` / `".bgb"`).
public struct ChatConfig: Sendable {
    public var host: String
    public var port: UInt16
    public var nick: String
    public var room: String
    public var encoding: WireEncoding
    public var characterName: String
    public var backdropName: String
    public var artDir: String

    public init(host: String, port: UInt16, nick: String, room: String,
                encoding: WireEncoding = .cp1252, characterName: String = "anna",
                backdropName: String = "field", artDir: String) {
        self.host = host
        self.port = port
        self.nick = nick
        self.room = room
        self.encoding = encoding
        self.characterName = characterName
        self.backdropName = backdropName
        self.artDir = artDir
    }
}

/// One line of a whisper transcript (Plan 4b Task 4's "ONE tabbed box" —
/// `ChatSessionModel.whisperHistories`' per-peer value). `nick` is the line's
/// speaker (the peer for an inbound whisper, our own nick for `isOwn`);
/// `isOwn` distinguishes our own outbound lines (right-aligned/secondary in
/// the UI) from the peer's inbound ones.
public struct WhisperLine: Sendable, Equatable {
    public let nick: String
    public let text: String
    public let isOwn: Bool

    public init(nick: String, text: String, isOwn: Bool) {
        self.nick = nick
        self.text = text
        self.isOwn = isOwn
    }
}

/// The app's headless, testable core (D1 §3.2's binding recommendation: all
/// logic lives in ComicChatKit, only chrome lives in the app). Composes every
/// prior Plan 4a task into one live loop:
///
///   ProtocolSession (Task 1/2/8, login+wire) --events-->
///     ChatSessionModel's consumer Task --enqueueEngineWork-->
///       ProtocolStripBridge (Task 6) --drives--> Strip (Plan 2, Task 5/7)
///         --compose--> CGCanvas (Task 4's real CoreText metrics) --> CGImage
///
/// THREADING (binding, carried over from every composed type's own contract):
/// ONE `DispatchQueue` (`engineQueue`) is injected into `ProtocolSession` and
/// used for ALL strip work (`ProtocolStripBridge`/`Strip`/`CGCanvas.compose`)
/// too — comicchat.h's single-thread contract covers every `cc_*` entry point
/// in the process, not just `cc_session_*` (see `ProtocolSession`'s and
/// `ProtocolStripBridge`'s own top doc comments). The event-consumer `Task`
/// reads `session.events` (an `AsyncStream`, off the engine queue) and hops
/// onto the engine queue via `enqueueEngineWork`/`engineQueue.async` for
/// EVERY touch of engine-adjacent state (`transcript`, the strip/bridge, the
/// announced-back set) — NEVER synchronously from within the event-handling
/// closure itself (that would be a call from inside the `on_event` C-callback
/// stack, which deadlocks `performOnEngineQueue`'s `dispatchPrecondition`;
/// see `EngineInterleaveTests`, the proven pattern this follows). Routing the
/// ENTIRE per-event handler through one `engineQueue.async` (rather than only
/// the strip-touching branches) also gives `transcript`/`announcedBackTo`/
/// the login-continuation a single serialized owner, avoiding a second,
/// independent point of mutation off the engine queue.
public final class ChatSessionModel: @unchecked Sendable {
    public var onStripImage: (@Sendable (CGImage, CGSize) -> Void)?
    public var onMembers: (@Sendable ([String]) -> Void)?
    public var onStatus: (@Sendable (String) -> Void)?
    /// Fired after every emotion-wheel drag (`setEmotion`), typing preview
    /// (`previewTyping`), or (implicitly, via those two) character change,
    /// with the freshly rendered self-pose image (`nil` if the pose image
    /// couldn't be loaded/rendered). Called on the MAIN thread, matching
    /// `onStripImage`/`onMembers`/`onStatus`'s posture.
    public var onSelfPose: (@Sendable (CGImage?) -> Void)?
    /// Fired on the MAIN thread for every whisper line added to
    /// `_whisperHistories` -- both an inbound `.whisper` event (peer = the
    /// sender's nick) AND `sendWhisper`'s own synthetic own-line append (peer
    /// = the addressee) -- see `WhisperBox`'s doc comment for how the app
    /// layer uses this to drive unread badges/live transcript updates.
    public var onWhisper: (@Sendable (String, WhisperLine) -> Void)?

    private let config: ChatConfig
    private let engineQueue = DispatchQueue(label: "com.comicchat.engine")
    private let session: ProtocolSession

    // MARK: Engine-queue-owned state (touched ONLY on `engineQueue`)

    /// The canonical event log — every `ProtocolEvent` this session has seen,
    /// in arrival order, INCLUDING the synthetic self-say events `send(_:)`
    /// injects (see that method's doc comment). This is the transcript
    /// `setViewport`'s reflow replays against (D2 §2.3: "the transcript = the
    /// event log; reflow = destroy strip -> recreate -> re-apply transcript").
    /// Backed by `_transcript` (engine-queue-only storage) + `transcript`
    /// (the public, thread-safe snapshot reader) below.
    private var _transcript: [ProtocolEvent] = []
    private var metricsCanvas: CTMetricsCanvas?
    /// The `CanvasBox` wrapping `metricsCanvas`, registered with the engine
    /// via `cc_set_metrics_canvas` — MUST be kept alive as a property (not
    /// just for the duration of the registration call): `CanvasBox` does NOT
    /// retain itself (its `ctx` is an UNRETAINED `Unmanaged` pointer to
    /// itself, `CanvasBox`'s own doc comment), so once nothing holds it, it
    /// deallocates its heap-allocated `cc_canvas`/`cc_canvas_ops` C structs —
    /// which the engine's file-static metrics-canvas pointer still points at,
    /// producing a bad-access crash the next time ANY strip measures text
    /// (`cc_strip_add_line_cooked` -> `CUnitPanel::LayoutBalloon` ->
    /// `CCanvas::measure_text`, observed via a SIGSEGV in exactly that call
    /// chain during initial development of this type — the fix was
    /// discovering `withExtendedLifetime`'s closure-scoped lifetime extension
    /// ends immediately after `cc_set_metrics_canvas` returns, which is FAR
    /// too short: the registration must outlive every subsequent `addLine`
    /// call for the whole strip's lifetime, not just the registration call
    /// itself).
    private var metricsCanvasBox: CanvasBox?
    private var strip: Strip?
    private var bridge: ProtocolStripBridge?
    private var selfParticipantID: Int32?
    /// Standalone handle (independent of `strip`'s own participant registry)
    /// on the SELF character's `.avb`, used only to render `emitSelfPoseLocked`'s
    /// preview image via `poseImage(_:)`. Lazily created on first use;
    /// survives a `reflowLocked()` reflow (it has nothing to do with the
    /// strip's own participant table) but is NOT reset by reflow -- Task 5's
    /// character-change flow is what would need to reset this when the
    /// self-character actually changes (out of this task's scope).
    private var selfAvatarFile: AvatarFile?
    /// nicks this model has already sent a private reply-announce to (Task
    /// 8's `toNick:` announce) — guards the "first `.appearsAs` from an
    /// unseen nick" rule so a nick's later avatar changes don't re-announce.
    private var announcedBackTo: Set<String> = []
    /// Own-say echo dedup (4a final-review carryover, Plan 4b Task 1): some
    /// IRC servers echo a client's own PRIVMSG back to the sender. `send(_:)`
    /// already renders the own line immediately via a synthetic local
    /// `.text` event (that method's doc comment) — a server echo of the SAME
    /// text must be dropped rather than rendered a second time. Holds exactly
    /// the texts of sends still awaiting a possible echo; `handleLocked`
    /// removes (at most) one matching entry per server-originated `.text`
    /// with our own nick.
    private var pendingLocalEchoes: [String] = []
    /// Engine-queue-local mirror of `session.ownNick`, kept in sync from the
    /// same events `ProtocolSession` itself uses (`.loggedIn`, `.nickChanged`
    /// with `isSelf`). Exists ONLY so `handleLocked`'s echo-dedup check
    /// (below) can compare against the current own nick WITHOUT calling
    /// `session.ownNick` — that accessor does its own `sessionQueue.sync`
    /// internally, and `sessionQueue` IS `engineQueue` (shared by injection,
    /// `ProtocolSession`'s own doc comment), so calling it from `handleLocked`
    /// (already running ON that queue) is a same-queue reentrant `sync`,
    /// which traps — the exact hazard `emitMembers`'s doc comment documents
    /// for `session.room(_:)`, here for `session.ownNick` instead. Seeded
    /// from `config.nick` (the requested nick, correct until any rename).
    private var currentOwnNick: String
    /// Members-ordering guard (4a final-review carryover, Plan 4b Task 1):
    /// `emitMembers()`'s detached `Task` reads `session.room(_:)` off the
    /// engine queue (that method's own doc comment explains why it must be
    /// detached rather than synchronous), so under rapid membership churn
    /// several of these `Task`s can be in flight at once with no guarantee
    /// they complete in spawn order — an earlier-fired-but-slower `Task`
    /// could otherwise deliver its now-stale snapshot to `onMembers` AFTER a
    /// later, more current one already arrived. `membersSeq` (engine-queue-
    /// owned, incremented once per `emitMembers()` call — serialized, so no
    /// two calls ever get the same value) tags each snapshot with its spawn
    /// order; `appliedMembersSeq` (below) is compared against it on the MAIN
    /// thread to drop any snapshot that arrives after a fresher one already
    /// applied.
    private var membersSeq = 0
    /// Main-thread-owned counterpart to `membersSeq` — safe unsynchronized
    /// because it is read and written ONLY inside the `DispatchQueue.main.async`
    /// block in `emitMembers()`, i.e. always on the main thread.
    private var appliedMembersSeq = 0
    /// Current viewport geometry (columns, unit twips, scale). `start()`
    /// seeds panel geometry with `PanelFit`'s 3-column default (see
    /// `setUpStripLocked`'s doc comment) so a strip exists and composes even
    /// before any real viewport arrives via `setViewport`.
    private var currentColumns: Int32 = 3
    private var currentUnitTwips: Int32 = PanelFit.minUnitPanelWidth
    private var currentScale: CGFloat = 2.0
    private var didSetViewport = false
    /// Set once by `shutdown()`; checked at the top of `handleLocked` so any
    /// event still in flight (already `engineQueue.async`-scheduled before
    /// `shutdown()` ran, or arriving from a detached `Task` spawned by an
    /// EARLIER `handleLocked` call — e.g. `.loggedIn`'s `join`/`.selfJoined`'s
    /// `announceAvatar`, both unstructured `Task`s with no lifetime tie to
    /// `consumerTask`) becomes a no-op instead of touching a torn-down
    /// `strip`/`bridge`. Needed because Swift's cooperative `Task` cancellation
    /// does not retroactively stop work already scheduled on `engineQueue`,
    /// and per-process engine state (the metrics-canvas registration, the
    /// avatar registry) is shared with every OTHER test/session in the same
    /// process — a late `recomposeLocked()` racing a DIFFERENT
    /// `ChatSessionModel`'s (or a raw `Strip`'s) engine-global-state setup
    /// corrupts it (observed: `EngineInterleaveTests` intermittently ASSERT
    /// failing/crashing when run right after a `ChatSessionModelTests` test
    /// whose stray post-`shutdown()` engine work was still in flight).
    private var isShutDown = false
    private var consumerTask: Task<Void, Never>?

    // MARK: Whisper (Plan 4b Task 4)

    /// Engine-queue-owned whisper transcripts, keyed by peer nick. Appended
    /// to from `handleLocked`'s `.whisper` case (inbound) and from
    /// `sendWhisper` (own outbound line) -- see `whisperHistories` (the
    /// public, thread-safe snapshot reader) below.
    private var _whisperHistories: [String: [WhisperLine]] = [:]
    /// TEST-ONLY internal seam (reachable via `@testable import`): Task 5
    /// wires this to a real settings toggle. Hard-coded to the spec default
    /// `true` until then -- an inbound whisper is dropped from history (and
    /// surfaces via `onStatus` instead) while this is `false`. Engine-queue
    /// only.
    var _acceptWhispers: Bool = true

    public init(config: ChatConfig) {
        self.config = config
        self.currentOwnNick = config.nick
        self.session = ProtocolSession(host: config.host, port: config.port, nick: config.nick,
                                       encoding: config.encoding, engineQueue: engineQueue)
    }

    /// Thread-safe snapshot of the event log so far (see `_transcript`'s doc
    /// comment for why the engine-queue-owned storage is private).
    public var transcript: [ProtocolEvent] {
        engineQueue.sync { _transcript }
    }

    // MARK: - start()

    /// connect -> (auto-login, handled entirely inside `ProtocolSession`) ->
    /// join -> announce. Builds the strip (metrics canvas, backdrop, self
    /// participant) on the engine queue BEFORE connecting, starts the
    /// event-consumer `Task` before `connect()` so no event is missed between
    /// connection and the first event, then returns once the TCP socket is
    /// up.
    ///
    /// `start()` deliberately does NOT wait for login/join/announce to
    /// complete before returning: the auto-login handshake (probe ->
    /// 451/800-pivot/timeout -> NICK/USER -> 001) runs for an unbounded
    /// amount of async time entirely inside `ProtocolSession` (driven by the
    /// SERVER's replies, which in a live app arrive on their own schedule and
    /// in a test are sent by the caller AFTER `start()` returns — see
    /// `ChatSessionModelTests.liveLoopRendersAndSends`, which calls
    /// `try await model.start()` and only THEN drives the server's
    /// login/join replies). `join` is instead fired from `handleLocked`'s
    /// `.loggedIn` case (below) once the auto-login actually completes,
    /// mirroring how `.selfJoined` there already fires `announceAvatar`
    /// fire-and-forget rather than blocking its caller.
    public func start() async throws {
        try engineQueue.sync {
            try self.setUpStripLocked()
        }
        startEventConsumer()
        try await session.connect()
    }

    /// Engine-queue-only: installs the metrics canvas BEFORE creating the
    /// `Strip` (fonts are measured once per strip -- `cc_set_metrics_canvas`
    /// must be live before `Strip()`'s `cc_strip_create` call), seeds panel
    /// geometry from `PanelFit`'s defaults (3 columns, until a real viewport
    /// arrives via `setViewport`), creates the strip + bridge, sets the
    /// title (so panel 0 exists even before any chat line), sets the
    /// backdrop, and adds+registers the self participant.
    private func setUpStripLocked() throws {
        // Reuse an already-installed metrics canvas across a reflow
        // (`reflowLocked` clears `strip`/`bridge` but does NOT clear
        // `metricsCanvas`/`metricsCanvasBox`): the metrics canvas is a
        // process-global registration independent of any one `Strip`
        // instance (this method's own doc comment), so there is no need to
        // (and, per `metricsCanvasBox`'s doc comment, it would be actively
        // harmful to transiently drop the only reference to) rebuild it on
        // every strip recreation.
        if metricsCanvasBox == nil {
            let metrics = CTMetricsCanvas()
            let box = CanvasBox(metrics)
            cc_set_metrics_canvas(box.handle)
            self.metricsCanvas = metrics
            self.metricsCanvasBox = box
        }

        let newStrip = try Strip()
        try newStrip.setPanelGeometry(unitTwips: currentUnitTwips, panelsPerRow: currentColumns)
        // setTitle BEFORE addParticipant/addLine (Strip.setTitle's own doc
        // comment): builds panel 0 as a title/starring panel immediately, so
        // a session that has joined but not yet seen (or sent) any chat line
        // still has a non-empty strip to compose -- `.selfJoined`'s
        // recompose (below) would otherwise have nothing to show
        // (`recomposeLocked` skips while `panelCount == 0`), exactly the gap
        // `FixtureReplayServerTests` exercises (a fixture with a login/join
        // sequence but no chat message at all).
        try newStrip.setTitle(config.room)
        let resolver = ProtocolStripBridge.AvatarResolver(comicartDir: config.artDir)
        let newBridge = try ProtocolStripBridge(strip: newStrip, resolver: resolver, encoding: config.encoding)
        try newBridge.setBackdrop(config.artDir + "/" + config.backdropName + ".bgb")

        let avbPath = config.artDir + "/" + config.characterName + ".avb"
        let selfID = try newStrip.addParticipant(nick: config.nick, avbPath: avbPath)
        try newStrip.setSelf(selfID)

        self.strip = newStrip
        self.bridge = newBridge
        self.selfParticipantID = selfID
        // Register self's id directly with the bridge (rather than letting
        // the bridge's own `ensureParticipant` assign a SECOND id later): our
        // own JOIN/NAMES typically comes back from the server just like any
        // other member, so a later `.userJoined`/`.text` event carrying OUR
        // OWN nick must resolve to this SAME participant id, not a fresh one.
        newBridge.preRegisterSelfParticipant(nick: config.nick, id: selfID)
    }

    // MARK: - Event consumer

    private func startEventConsumer() {
        consumerTask = Task { [weak self] in
            guard let self else { return }
            for await ev in self.session.events {
                self.enqueueHandle(ev)
            }
        }
    }

    /// Hops onto the engine queue and routes one event. Called from the
    /// event-consumer `Task` (off the engine queue, `fromServer: true` — the
    /// default) and from `send(_:)`'s synthetic self-say (`fromServer:
    /// false`) — both funnel through this single entry point so
    /// `_transcript`/`announcedBackTo`/`loggedInContinuation` have exactly
    /// one serialized owner.
    private func enqueueHandle(_ ev: ProtocolEvent, fromServer: Bool = true) {
        engineQueue.async { [weak self] in
            self?.handleLocked(ev, fromServer: fromServer)
        }
    }

    /// ENGINE QUEUE ONLY. Appends to `_transcript`, then routes per the
    /// report's event-routing table:
    ///   .loggedIn               -> join(config.room) (not part of the
    ///                              brief's strip-routing table, but the
    ///                              earliest-legal moment to join: `join`
    ///                              requires `connectionStatus == .connected`,
    ///                              which `.loggedIn` IS the confirmation of)
    ///   .selfJoined            -> announceAvatar(channel:, name:), recompose
    ///   .appearsAs (unseen nick) -> private reply-announce (toNick:), then
    ///                               bridge.apply + recompose
    ///   .text/.action (strip-relevant) -> bridge.apply + recompose
    ///        (.text only: own-say echo dedup first, see below)
    ///   .whisper                -> Plan 4b Task 4: route to
    ///                              `_whisperHistories`/`onWhisper` (unless
    ///                              `_acceptWhispers == false`, in which case
    ///                              `onStatus` instead), THEN (unconditionally)
    ///                              bridge.apply + recompose -- the existing
    ///                              4a main-strip whisper-balloon rendering,
    ///                              kept as-is.
    ///   .userJoined             -> bridge.apply + recompose, AND recompute
    ///                              sorted member list -> onMembers (final
    ///                              review: a join is both strip-relevant AND
    ///                              membership-relevant)
    ///   .userParted/.userQuit/.kicked/.names/.endOfNames/.nickChanged
    ///                           -> recompute sorted member list -> onMembers
    ///   .statusLine/.error/.disconnectedHint -> onStatus
    ///
    /// - Parameter fromServer: `true` for every event arriving off the wire
    ///   (the event-consumer `Task`'s default); `false` only for `send(_:)`'s
    ///   synthetic self-say. Used by the `.text` case's own-say echo dedup
    ///   below — a synthetic event (`fromServer == false`) never matches the
    ///   `pendingLocalEchoes` check, since it IS the render being kept.
    private func handleLocked(_ ev: ProtocolEvent, fromServer: Bool = true) {
        guard !isShutDown else { return }

        // Own-say echo dedup (4a carryover): some servers echo PRIVMSG back to
        // the sender; our synthetic local echo (send(_:)) already rendered it.
        // Drop exactly one server copy per pending send, BEFORE the transcript
        // append -- reflow (`reflowLocked`, which replays `_transcript`
        // verbatim) must not double-render it either.
        if case .text(let nick, _, _, let text, _, _) = ev,
           fromServer, nick == currentOwnNick,
           let i = pendingLocalEchoes.firstIndex(of: text) {
            pendingLocalEchoes.remove(at: i)
            return
        }
        _transcript.append(ev)

        switch ev {
        case .loggedIn(let nick):
            currentOwnNick = nick   // echo-only rule confirmation, mirrors ProtocolSession's own _ownNick update
            Task { [session, config] in
                try? await session.join(config.room)
            }

        case .selfJoined:
            let name = config.characterName.capitalized
            Task { [session, config] in
                try? await session.announceAvatar(channel: config.room, name: name)
            }
            recomposeLocked()

        case .appearsAs(let nick, _, _):
            if !announcedBackTo.contains(nick) {
                announcedBackTo.insert(nick)
                let name = config.characterName.capitalized
                Task { [session, config] in
                    try? await session.announceAvatar(channel: config.room, toNick: nick, name: name)
                }
            }
            try? bridge?.apply(ev)
            recomposeLocked()

        case .text, .action:
            try? bridge?.apply(ev)
            recomposeLocked()

        case .whisper(let nick, _, let text, _):
            // Plan 4b Task 4: Task-5's acceptWhispers setting doesn't exist
            // yet -- `_acceptWhispers` is a hard-coded-true internal seam
            // until then. When false, the whisper is dropped from
            // `_whisperHistories`/`onWhisper` entirely and surfaces only via
            // `onStatus` (matching the brief's drop-path contract). The
            // main-strip whisper-balloon rendering (`bridge.apply` +
            // `recomposeLocked`) is EXISTING 4a behavior for room-scoped
            // whispers and is unconditionally kept either way -- this task
            // only adds the tabbed-box routing alongside it.
            if _acceptWhispers {
                let line = WhisperLine(nick: nick, text: text, isOwn: false)
                _whisperHistories[nick, default: []].append(line)
                let cb = onWhisper
                DispatchQueue.main.async { cb?(nick, line) }
            } else {
                emitStatus("Whisper from \(nick) blocked (whispers disabled)")
            }
            try? bridge?.apply(ev)
            recomposeLocked()

        case .userJoined:
            try? bridge?.apply(ev)
            recomposeLocked()
            // Final review (Plan 4a): a peer joining mid-session must also
            // refresh the member sidebar -- match every other membership case
            // below (parts/quits/kicks/names/nick-changes) which already call
            // `emitMembers()`. Without this, a joining peer appears in the
            // comic strip but never in the sidebar until some unrelated
            // membership event happens to fire.
            emitMembers()

        case .nickChanged(_, let newNick, let isSelf):
            if isSelf {
                currentOwnNick = newNick   // echo-only rule confirmation, mirrors ProtocolSession's own _ownNick update
            }
            emitMembers()

        case .userParted, .userQuit, .kicked, .names, .endOfNames:
            emitMembers()

        case .statusLine(let text):
            emitStatus(text)
        case .error(let code, let text):
            emitStatus("error \(code): \(text)")
        case .disconnectedHint(let text):
            emitStatus(text)

        default:
            break
        }
    }

    /// ENGINE QUEUE ONLY. Composes the strip built so far onto a fresh
    /// `CGCanvas` at the current viewport scale and hands the image back on
    /// the main thread. Skips if there are no panels yet (an empty strip has
    /// zero size — nothing to compose).
    private func recomposeLocked() {
        guard let bridge, let strip else { return }
        guard strip.panelCount > 0 else { return }
        let (w, h) = strip.size
        guard w > 0, h > 0 else { return }
        let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: currentScale)
        guard (try? bridge.compose(onto: canvas)) != nil else { return }
        guard let image = canvas.makeCGImage() else { return }
        let sizePoints = CGSize(width: CGFloat(w) / 20, height: CGFloat(h) / 20)
        DispatchQueue.main.async { [onStripImage] in
            onStripImage?(image, sizePoints)
        }
    }

    /// Reads the current member list and forwards it to `onMembers`. Called
    /// from `handleLocked` (running ON the engine queue), but dispatched via
    /// a detached `Task` rather than reading `session.room(_:)` directly:
    /// that accessor does its own `sessionQueue.sync` internally
    /// (`ProtocolSession`'s public read API), and `sessionQueue` IS this
    /// model's `engineQueue` (shared by injection, `ProtocolSession`'s own
    /// doc comment) — calling it synchronously from a closure ALREADY
    /// executing on that same serial queue is a same-queue reentrant `sync`,
    /// which traps (SIGTRAP, observed) rather than merely deadlocking. The
    /// `Task` runs on its own (cooperative-pool) context, genuinely off the
    /// engine queue, so `session.room(_:)`'s internal `sync` is safe there.
    private func emitMembers() {
        membersSeq += 1                              // engine queue — serialized
        let seq = membersSeq
        Task { [session, config, onMembers] in
            let members = session.room(config.room)?.members ?? [:]
            let sorted = members.values.filter { !$0.departed }.map(\.nick).sorted()
            DispatchQueue.main.async {
                guard seq > self.appliedMembersSeq else { return }   // stale snapshot — drop
                self.appliedMembersSeq = seq
                onMembers?(sorted)
            }
        }
    }

    private func emitStatus(_ text: String) {
        DispatchQueue.main.async { [onStatus] in
            onStatus?(text)
        }
    }

    // MARK: - emotion wheel / send-mode preview (Plan 4b Task 3)

    /// The wheel drag: sets the SELF participant's emotion (`angle` in
    /// radians, `intensity` in `[0, 1]` — the caller's job to apply the 0.2
    /// center detente, `Strip.setSelfEmotion`'s doc comment) and re-emits the
    /// freshly rendered self-pose image via `onSelfPose`. Fire-and-forget:
    /// hops onto the engine queue and returns immediately, matching every
    /// other engine-touching entry point on this type that isn't already
    /// `async` (`setViewport`).
    public func setEmotion(angle: Double, intensity: Double) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown, let strip = self.strip else { return }
            try? strip.setSelfEmotion(angle: angle, intensity: intensity)
            self.emitSelfPoseLocked()
        }
    }

    /// The typing preview: runs the engine's text->emotion inference against
    /// the SELF participant's avatar (`Strip.previewSelfText`) so the wheel's
    /// live pose preview reflects what `text` WOULD infer, without adding a
    /// line to the strip. Fire-and-forget, same posture as `setEmotion`.
    public func previewTyping(_ text: String) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown, let strip = self.strip else { return }
            try? strip.previewSelfText(text)
            self.emitSelfPoseLocked()
        }
    }

    /// ENGINE QUEUE ONLY. Renders the SELF participant's CURRENT pose
    /// (`strip.selfPoseIndex()`) via a standalone `AvatarFile` handle on the
    /// self character (`selfAvatarFile`, lazily created here) and hands the
    /// image to `onSelfPose` on the main thread.
    ///
    /// INDEX SPACE (critical, comicchat.h): `selfPoseIndex()` returns the
    /// engine's poseID, which is ONE-BASED — `AvatarFile.poseImage(_:)`'s
    /// index space is zero-based over the SAME pose array, so the poseID is
    /// converted via `Int(idx) - 1` before the lookup. This is a DIFFERENT
    /// index space again from `selfAnnotations()`'s pose fields (GetIndices
    /// record indices) — never mix the three. For complex (two-part)
    /// avatars, `selfPoseIndex()` reports the TORSO poseID only, which is an
    /// accepted approximation for this preview.
    private func emitSelfPoseLocked() {
        guard let strip else { return }
        if selfAvatarFile == nil {
            selfAvatarFile = try? AvatarFile(path: config.artDir + "/" + config.characterName + ".avb")
        }
        var image: CGImage? = nil
        if let idx = try? strip.selfPoseIndex(), idx >= 1, let av = selfAvatarFile,
           let art = try? av.poseImage(Int(idx) - 1) {
            image = art.cgImage()
        }
        DispatchQueue.main.async { [onSelfPose] in onSelfPose?(image) }
    }

    /// `Strip.Mode` (the `CC_MODE_*` bitmask) -> the raw SM_* ordinal a
    /// cooked `Annotations.mode` field carries on the wire (Task 2's
    /// verified finding, `defines.h:57-61`): SM_SAY=1, SM_WHISPER=2,
    /// SM_THINK=3, SM_ACTION=5 (SM_SHOUT=4 is never emitted by the
    /// original's BM2SM and has no `Strip.Mode` counterpart here). This is
    /// the INVERSE direction of `ProtocolStripBridge.stripModes(kind:annotations:)`
    /// (SM_* -> CC_MODE_*, for INBOUND annotations) — kept as its own table
    /// rather than reusing that one, since the two ARE inverses but live on
    /// different types for different purposes (outbound send vs. inbound
    /// routing).
    static func smMode(for mode: Strip.Mode) -> Int32 {
        switch mode {
        case .whisper: return 2   // SM_WHISPER
        case .think:   return 3   // SM_THINK
        case .action:  return 5   // SM_ACTION
        default:       return 1   // SM_SAY (.say, and any unrecognized combination)
        }
    }

    // MARK: - send

    /// Sends `text` under `mode` (default `.say`), with COOKED outbound pose
    /// annotations built from the CURRENT wheel/preview state (the original
    /// grabs the bodycam state at send time too — Task 9's `annotations: nil`
    /// MVP scope decision is superseded by this task). `sendComicsData`
    /// (Task 5) will let a user opt out of annotating at all; until that
    /// lands, every send is annotated.
    ///
    /// OWN-SAY RENDERING (Task 9's "No" answer, still honored): the original
    /// renders own says immediately by adding a local history entry at send
    /// time (`bChatSendText` -> local `AddAndExecute`), not by waiting for a
    /// server echo — our loopback/most real IRCds don't echo PRIVMSG back to
    /// the sender at all. This synthesizes a `.text` event locally carrying
    /// the SAME cooked annotations that went out over the wire, and feeds it
    /// through the SAME transcript-append + enqueue path a server-originated
    /// `.text` would take, so the own line appears (posed) without depending
    /// on any echo.
    public func send(_ text: String, mode: Strip.Mode = .say) async throws {
        // `performOnEngineQueue` traps if called while already ON the engine
        // queue (its own doc comment) — `send` is invoked from the UI/main
        // context (ChatWindow's Task { try? await model.send(...) }), never
        // from inside `handleLocked`/the event consumer, so this is legal,
        // same posture as the pre-existing `session.say` call below.
        let ann: Annotations? = session.performOnEngineQueue { [self] in
            guard var a = try? strip?.selfAnnotations() else { return nil }
            a.mode = Self.smMode(for: mode)
            return a
        }
        try await session.say(config.room, text: text, annotations: ann,
                              modes: UInt16(mode.rawValue))
        // Registered BEFORE the synthetic event is enqueued (4a carryover:
        // own-say echo dedup, `handleLocked`'s doc comment) so a server echo
        // of this same text — which can only arrive after `session.say`
        // above has already put the PRIVMSG on the wire — always finds a
        // pending entry to consume, however the two async paths interleave.
        engineQueue.async { [weak self] in self?.pendingLocalEchoes.append(text) }
        let ownNick = session.ownNick
        let synthetic = ProtocolEvent.text(nick: ownNick, ident: "", target: config.room,
                                          text: text, kind: 0, annotations: ann)
        enqueueHandle(synthetic, fromServer: false)
    }

    // MARK: - whisper (Plan 4b Task 4)

    /// Thread-safe snapshot of `_whisperHistories` (see that property's doc
    /// comment for why the engine-queue-owned storage is private) — the
    /// per-peer whisper transcripts the whisper box's `NavigationSplitView`
    /// reads.
    public var whisperHistories: [String: [WhisperLine]] {
        engineQueue.sync { _whisperHistories }
    }

    /// Sends a whisper to `peer` (room-agnostic wire-wise — `cc_session_send_whisper`
    /// actually speaks a plain `PRIVMSG <peer> :...`, see `WhisperRoutingTests`'
    /// doc comment for the verified wire form) with COOKED SM_WHISPER-mode
    /// annotations built from the current wheel/preview state, mirroring
    /// `send(_:mode:)`'s own-render posture: the own line is appended to
    /// `_whisperHistories[peer]` and re-broadcast via `onWhisper` locally,
    /// rather than waiting for a possible server echo.
    ///
    /// DEVIATION FROM THE BRIEF'S SKETCH (noted per the task instructions):
    /// the brief's sketch read `self.session.ownNick` inside the
    /// `engineQueue.async` closure below. `session.ownNick` does its own
    /// `sessionQueue.sync` internally, and `sessionQueue` IS `engineQueue`
    /// (shared by injection, `ProtocolSession`'s own doc comment) — calling
    /// it from a closure already running ON that same serial queue is a
    /// same-queue reentrant `sync`, which traps (the exact hazard
    /// `currentOwnNick`'s own doc comment documents this for `handleLocked`'s
    /// echo-dedup check; the same hazard applies here). This uses
    /// `currentOwnNick` (the engine-queue-local mirror) instead. Note this is
    /// unlike `send(_:mode:)`'s own `let ownNick = session.ownNick` line,
    /// which reads it OFF the engine queue (not inside an `engineQueue.async`
    /// closure) and is therefore legal as written — the trap is specific to
    /// reading it FROM INSIDE an already-on-`engineQueue` closure, which is
    /// exactly what this method's closure is.
    public func sendWhisper(to peer: String, text: String) async throws {
        let ann: Annotations? = session.performOnEngineQueue { [self] in
            guard var a = try? strip?.selfAnnotations() else { return nil }
            a.mode = Self.smMode(for: .whisper)
            a.addressees = [peer]
            return a
        }
        try await session.whisper(to: [peer], text: text, channel: config.room, annotations: ann)
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            let line = WhisperLine(nick: self.currentOwnNick, text: text, isOwn: true)
            self._whisperHistories[peer, default: []].append(line)
            let cb = self.onWhisper
            DispatchQueue.main.async { cb?(peer, line) }
        }
    }

    /// TEST-ONLY (internal, reachable via `@testable import`): hops onto the
    /// engine queue and back, guaranteeing any work already
    /// `engineQueue.async`-scheduled before this call (`setEmotion`/
    /// `previewTyping`'s fire-and-forget dispatch) has completed by the time
    /// `onSettled` runs — the "sentinel enqueueEngineWork hop" settle
    /// technique used throughout this test target (`ChatSessionModelTests`'
    /// polling helpers solve the same problem for server-driven events;
    /// there is no server round-trip to poll for here, since `setEmotion`/
    /// `previewTyping` never touch the wire).
    func settleEngineQueue(_ onSettled: @escaping @Sendable () -> Void) {
        engineQueue.async { onSettled() }
    }

    // MARK: - setViewport (reflow)

    /// Recomputes panel geometry for `widthPoints` (converted to twips) at
    /// `scale`. If the computed geometry is unchanged from the current one,
    /// just recomposes at the new scale (no reflow needed — the same panel
    /// layout, only the output pixel density changed). Otherwise performs a
    /// full reflow (D2 §2.3 / risk 2: "replay is the reflow" — the transcript
    /// IS the event log, so tearing down and re-applying it byte-for-byte
    /// reproduces the exact same strip content, just at new panel geometry):
    /// destroy the strip, recreate one (the metrics canvas installed in
    /// `setUpStripLocked` stays live — it's a process-global registration,
    /// not per-strip), `setPanelGeometry` with the new geometry, re-add the
    /// backdrop and self participant, build a fresh bridge, re-`apply` the
    /// ENTIRE transcript in order, then recompose.
    public func setViewport(widthPoints: CGFloat, scale: CGFloat) {
        let viewportTwips = Int32((widthPoints * 20).rounded())
        let columns = PanelFit.columns(forViewportWidthTwips: viewportTwips)
        let unit = PanelFit.unitPanelTwips(viewportWidthTwips: viewportTwips, columns: columns)

        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            self.currentScale = scale
            if self.didSetViewport, columns == self.currentColumns, unit == self.currentUnitTwips {
                self.recomposeLocked()
                return
            }
            self.didSetViewport = true
            self.currentColumns = columns
            self.currentUnitTwips = unit
            self.reflowLocked()
        }
    }

    /// ENGINE QUEUE ONLY. See `setViewport`'s doc comment for the full
    /// rationale: destroy + recreate the strip at the (already updated)
    /// `currentColumns`/`currentUnitTwips`, re-add backdrop/self, fresh
    /// bridge, re-apply the whole transcript, recompose.
    private func reflowLocked() {
        strip?.close()
        strip = nil
        bridge = nil
        selfParticipantID = nil
        announcedBackTo.removeAll()

        guard (try? setUpStripLocked()) != nil else { return }
        guard let bridge else { return }
        for ev in _transcript {
            try? bridge.apply(ev)
        }
        recomposeLocked()
    }

    // MARK: - shutdown

    /// Cancels the event consumer, disconnects the session, and tears down
    /// the strip. Synchronous (`engineQueue.sync`, not `.async`): by the time
    /// `shutdown()` returns, `isShutDown` is set and the strip/bridge/metrics
    /// box are released, so no work already queued on `engineQueue` (or
    /// spawned by an earlier `handleLocked` as a detached `Task` — see
    /// `isShutDown`'s doc comment) can observe a half-torn-down model or
    /// touch process-global engine state after this call returns. That
    /// matters beyond just this instance: the metrics-canvas/avatar-registry
    /// registrations are shared with every OTHER `Strip`/`ChatSessionModel`
    /// in the process (including a DIFFERENT test's), so a caller that has
    /// called `shutdown()` can safely assume this model will not touch that
    /// shared state again.
    public func shutdown() {
        consumerTask?.cancel()
        consumerTask = nil
        session.disconnect()
        engineQueue.sync {
            self.isShutDown = true
            self.strip?.close()
            self.strip = nil
            self.bridge = nil
            // Final review (Plan 4a): deregister BEFORE releasing the box.
            // `metricsCanvasBox`'s own doc comment explains why: the engine's
            // process-global metrics-canvas pointer points directly at the
            // box's (unretained, non-self-retaining) C structs, so releasing
            // the box first would leave that pointer dangling for however
            // long it takes `metricsCanvasBox = nil` to run -- and any other
            // session/selftest in the process sharing that global registration
            // could measure text against it in that window (bad access,
            // exactly the crash this box's whole design exists to avoid).
            cc_set_metrics_canvas(nil)
            self.metricsCanvasBox = nil
            self.metricsCanvas = nil
        }
    }
}
