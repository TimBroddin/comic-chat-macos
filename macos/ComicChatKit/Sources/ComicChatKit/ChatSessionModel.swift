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
    /// Plan 4b Task 5 (persona plumbing): the USER command's `<user>`/
    /// `<realname>` fields, plumbed straight through to
    /// `ProtocolSession.init`'s own same-named parameters. `nil` (default)
    /// preserves the pre-Task-5 nick-fallback behavior.
    public var userName: String?
    public var realName: String?
    /// Plan 4b Batch C: the free-text reply body a peer's `# GetInfo` probe
    /// receives (`SettingsStore.profileText`'s plumbed-through value). Empty
    /// string (default) is a legal, honest reply — see
    /// `ChatSessionModel`'s `.infoRequest` case for the exact wire behavior.
    public var profileText: String
    /// Gates `send(_:mode:)`'s outbound cooked pose annotations: `false` ->
    /// `annotations: nil` (peers then run text inference — the original's
    /// ComicsData toggle semantics). `true` (default) matches every prior
    /// task's always-annotated behavior.
    public var sendComicsData: Bool
    /// Seeds `ChatSessionModel`'s internal `_acceptWhispers` test seam
    /// (Task 4) from a real setting. `true` by default, matching that seam's
    /// existing hard-coded default.
    public var acceptWhispers: Bool
    /// Plan 4b Task 6: gates the `.appearsAs`-triggered avatar auto-download
    /// (an unknown name arriving WITH a URL). `true` by default (D4 §4's
    /// documented default; matches `SettingsStore.autoDownloadAvatars`'s own
    /// "never set" default).
    public var autoDownloadAvatars: Bool
    /// Quick-wins batch item 3 (original `UnitsWide`): a user-forced
    /// panels-per-row override, `0` (default) meaning "automatic" —
    /// `setViewport`'s existing `PanelFit.columns(forViewportWidthTwips:)`
    /// derivation, unchanged. When `> 0`, `setViewport` uses this value as
    /// `columns` directly instead (unit width is still computed from the
    /// viewport via `PanelFit.unitPanelTwips(viewportWidthTwips:columns:)`,
    /// just fed this forced column count rather than the auto-fit one).
    public var panelsPerRow: Int
    /// Auto-reconnect (spec §7): gates the model's backoff-reconnect loop on an
    /// UNEXPECTED network drop. `true` (default) matches a real live session's
    /// desired resilience; `false` disables it for the replay-fixture dev path
    /// (a `FixtureReplayServer` serves exactly one connection and closes it —
    /// reconnecting there would just re-run the fixture or dial a dead port,
    /// neither of which is meaningful, so `AppState` sets this `false` when it
    /// launches with `--replay-fixture`). Does NOT affect user-initiated
    /// disconnect, nick-rejected teardown, or a failed FIRST connect (those are
    /// never reconnected regardless — see `ChatSessionModel`'s reconnect
    /// trigger, which fires only from a `.disconnectedHint` on a
    /// previously-established session).
    public var autoReconnect: Bool

    public init(host: String, port: UInt16, nick: String, room: String,
                encoding: WireEncoding = .cp1252, characterName: String = "anna",
                backdropName: String = "field", artDir: String,
                userName: String? = nil, realName: String? = nil,
                profileText: String = "",
                sendComicsData: Bool = true, acceptWhispers: Bool = true,
                autoDownloadAvatars: Bool = true, panelsPerRow: Int = 0,
                autoReconnect: Bool = true) {
        self.host = host
        self.port = port
        self.nick = nick
        self.room = room
        self.encoding = encoding
        self.characterName = characterName
        self.backdropName = backdropName
        self.artDir = artDir
        self.userName = userName
        self.realName = realName
        self.profileText = profileText
        self.sendComicsData = sendComicsData
        self.acceptWhispers = acceptWhispers
        self.autoDownloadAvatars = autoDownloadAvatars
        self.panelsPerRow = panelsPerRow
        self.autoReconnect = autoReconnect
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

/// Fired from `handleLocked` (engine-queue detection — Plan 4b Batch B) for
/// two notification-worthy inbound events: a room `.text`/`.action` whose
/// text contains our own nick (case-folded `contains`, NOT true whole-word
/// matching — a simple substring test, documented deviation: "Tim" also
/// matches inside "Timothy") and an inbound whisper (either wire shape —
/// `.whisper` or a plain-IRC whisper-shaped `.text`, both already detected by
/// the SAME machinery `whisperBoxRoutingLocked`'s call sites use). Fires ONLY
/// for `fromServer` events (never our own synthetic self-say/echo) and NEVER
/// for an ignored nick — mirrors `.text`/`.action`'s own ignore-doctrine guard
/// (`ignoredNicks`, `handleLocked`'s doc comment) so a silenced nick can't
/// still buzz a notification. The APP layer (`AppState`) turns this into a
/// `UNUserNotificationCenter` post — this Kit stays UserNotifications-free
/// (UN requires a proper app bundle; `swift test` has none — see the type's
/// own callback-testability doc, tested here via loopback: the callback
/// itself, not the OS notification).
public struct NotificationEvent: Sendable {
    public enum Kind: Sendable, Equatable {
        case mention(room: String)
        case whisper
    }
    public let kind: Kind
    public let fromNick: String
    public let text: String

    public init(kind: Kind, fromNick: String, text: String) {
        self.kind = kind
        self.fromNick = fromNick
        self.text = text
    }
}

/// One joined room's state (Plan 4b Task 7: true multi-room). Each room keeps
/// its OWN canonical event-log transcript (the transcript doctrine, unchanged
/// — just per-room now); the ONE live strip is rebuilt from whichever room is
/// active. Background rooms cache their last composed image (`lastImage`) so a
/// tab switch away and back is instant, and count `unread` MESSAGES (only
/// `.text`/`.action`/whisper-carried content — NOT joins/parts/names).
///
/// Live-fix 1 (crypthome.com, live-reproduced): `rooms`/`roomOrder` are keyed
/// by `ChatSessionModel.roomKey(_:)` — a case-folded canonical form — NOT the
/// raw channel string, since IRC channel names are case-insensitive
/// (RFC 1459 §2.3.1) but our own bookkeeping used to be byte-exact (and, pre
/// live-fix 6, so was the ENGINE's own channel<->room_token table — see
/// `roomKey`'s own doc comment below for the current state of both layers),
/// so a server that echoes `JOIN #crypt` back as `#Crypt` (a real, observed
/// behavior) created a SECOND room box for the same channel. `displayName`
/// carries the presentation-form name shown in the tab bar/strip title/wire
/// calls — seeded from whatever casing FIRST created the box (typically our
/// own locally-typed room name) and updated to the SERVER's casing the
/// moment a `.selfJoined` confirm (or any other authoritative server-cased
/// channel payload) arrives for it, so the UI ends up showing the server's
/// canonical form, matching the original 1998 client's own display
/// convention (it always showed whatever casing the server used).
struct RoomBox {
    var displayName: String
    var transcript: [ProtocolEvent] = []
    var lastImage: CGImage?
    var lastSizePoints: CGSize = .zero
    var unread: Int = 0
    /// Quick-wins batch item 6: this room's current topic, updated from
    /// `handleLocked`'s `.topicChanged` case (the model already sees that
    /// event — no need to read `session.room(_:).topic` on-queue, which
    /// would trap the same way `session.ownNick`/`session.room(_:)` do
    /// elsewhere on this type, per `currentOwnNick`'s doc comment). Empty
    /// until a `.topicChanged` has actually arrived for this room.
    var topic: String = ""
}

/// A room's tab-bar summary (Plan 4b Task 7). `Identifiable` by `name` so
/// the rooms sidebar's `ForEach` can key on it directly.
public struct RoomInfo: Sendable, Equatable, Identifiable {
    public var id: String { name }
    public let name: String
    public let unread: Int
    public let isActive: Bool
    /// Quick-wins batch item 6: the room's current topic (from `.topicChanged`,
    /// tracked model-side in `RoomBox.topic` — see that property's doc comment
    /// for why NOT `session.room(_:).topic` directly), empty until a
    /// `.topicChanged` has actually arrived for this room. Drives the sidebar
    /// row's subtitle.
    public let topic: String

    public init(name: String, unread: Int, isActive: Bool, topic: String = "") {
        self.name = name
        self.unread = unread
        self.isActive = isActive
        self.topic = topic
    }
}

/// One row of a LIST browser (Plan 4b Task 8 — `CRoomList`'s LIST browser,
/// D1 §1.6/backlog item 5), built from the accumulated `.roomListBegin` ->
/// `.roomListItem`×N -> `.roomListEnd` sequence. `Identifiable` by `name`
/// (room names are unique per server) so `RoomListWindow`'s `Table` can key
/// on it directly, matching `RoomInfo`/`MemberRow`'s own posture.
public struct RoomListItem: Sendable, Equatable, Identifiable {
    public var id: String { name }
    public let name: String
    public let users: Int32
    public let topic: String

    public init(name: String, users: Int32, topic: String) {
        self.name = name
        self.users = users
        self.topic = topic
    }
}

/// One member-list row (Plan 4b Task 8: `onMembers`'s payload widens from
/// `[String]` to `[MemberRow]`) — `nick`/`isOp` (for the op badge + Kick/Ban
/// gating) plus `avatarName` (for the member-row icon thumbnail, resolved via
/// the model's art dir at the view layer). `Identifiable` by `nick` so
/// `ChatWindow`'s `List` can key on it directly, matching `RoomInfo`'s own
/// `Identifiable`-by-name posture.
public struct MemberRow: Sendable, Equatable, Identifiable {
    public var id: String { nick }
    public let nick: String
    public let isOp: Bool
    public let avatarName: String
    /// Quick-wins batch item 4, clear-fix in Batch B: mirrors
    /// `ProtocolSession.RoomMember.isAway`, set/cleared from an inbound
    /// `.awayPeer` event. Batch B finding: the wire-level CTCP marker this
    /// rides already carries BOTH directions (a peer going away sends
    /// `\x01AWAY <msg>\x01`; RETURNING sends the same marker with an EMPTY
    /// message, `\x01AWAY\x01` — `protsupp.cpp`'s ported `ccPayloadAwayPeer`
    /// parse already reproduces this grammar faithfully) — `ProtocolSession`'s
    /// `.awayPeer` handler now reads the empty-vs-non-empty message to flip
    /// this both ways (previously it only ever set `true`, silently dropping
    /// the "back" signal already sitting in the event payload). Display-only
    /// here either way — the actual flag lives on `RoomMember`.
    public let isAway: Bool
    /// Quick-wins batch item 5 (original CUserInfo m_bIgnored): whether this
    /// nick is in `ChatSessionModel.ignoredNicks` — drives the member grid's
    /// slashed-eye badge. Display-only annotation; the actual filtering
    /// happens in `handleLocked`/`rebuildStripLocked`, not here.
    public let isIgnored: Bool

    public init(nick: String, isOp: Bool, avatarName: String, isAway: Bool = false, isIgnored: Bool = false) {
        self.nick = nick
        self.isOp = isOp
        self.avatarName = avatarName
        self.isAway = isAway
        self.isIgnored = isIgnored
    }
}

/// The auto-reconnect state machine's public state (spec §7), delivered via
/// `ChatSessionModel.onReconnectStateChanged`. `Equatable` so the app layer can
/// cheaply ignore no-op redeliveries.
public enum ReconnectState: Sendable, Equatable {
    /// A live connection is up (the initial connect succeeded, or a reconnect
    /// landed). The steady state — the UI shows no reconnect indicator.
    case connected
    /// A backoff-reconnect loop is in progress after an unexpected drop.
    /// `attempt` is 1-based (the Nth attempt), `of` the cap — the UI shows
    /// "reconnecting… (attempt N of M)".
    case reconnecting(attempt: Int, of: Int)
    /// Every reconnect attempt failed (the cap was hit). Terminal until a
    /// manual reconnect (`AppState.connect()`) supersedes it — the UI shows a
    /// "couldn't reconnect" line and lets the user retry.
    case gaveUp
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
    /// Plan 4b Batch C: this port's own CTCP VERSION reply text, sent via
    /// `session.sendVersionReply` for a `.versionRequest` event. NOT the
    /// original's runtime-built "Microsoft Chat 2.5 (3.0) (comics mode)"
    /// (`GetVersionString`/`IDS_COMICS_MODE`, protsupp.cpp:1126-1138 — see
    /// `cc_session_send_version_reply`'s comicchat.h doc comment) — this is a
    /// different, honest client identifying itself as what it actually is, a
    /// static constant rather than a fabricated impersonation of the
    /// original's exact string.
    public static let versionReplyText = "Microsoft Comic Chat for macOS (open-source port) - comicchat 2.5 engine"

    public var onStripImage: (@Sendable (CGImage, CGSize) -> Void)?
    /// Plan 4b Task 8: widened from `[String]` to `[MemberRow]` — same
    /// snapshot+seq-guard delivery as before (`emitMembers`'s doc comment),
    /// just a richer row (`nick`/`isOp`/`avatarName`) so the sidebar can show
    /// an op badge + avatar thumbnail without a second round-trip.
    public var onMembers: (@Sendable ([MemberRow]) -> Void)?
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
    /// Fired on the MAIN thread whenever the set of joined rooms, the active
    /// room, or any room's unread count changes (Plan 4b Task 7). Drives the
    /// app's `RoomTabBar`. The `[RoomInfo]` is in join order (initial room
    /// first), each carrying its current unread count and whether it's the
    /// active room.
    public var onRoomsChanged: (@Sendable ([RoomInfo]) -> Void)?
    /// Fired ONCE per `requestRoomList()` round-trip (Plan 4b Task 8), on the
    /// MAIN thread, with the full accumulated `[RoomListItem]` — see
    /// `roomListAccum`'s doc comment for the accumulate-then-fire-on-end
    /// mechanics.
    public var onRoomList: (@Sendable ([RoomListItem]) -> Void)?
    /// Fired on the MAIN thread for a `getInfo(_:)` WHO reply (Plan 4b Task
    /// 8): `(nick, formatted)` — there is no `cc_session_whois` builder in the
    /// outbound surface (comicchat.h:600 has `who` only), so this is backed by
    /// `.whoResult` (WHO-by-nick), not a whois.
    public var onUserInfo: (@Sendable (String, String) -> Void)?
    /// Fired on the MAIN thread whenever the ACTIVE room's own-membership op
    /// status changes (Plan 4b Task 8) — derived inside `emitMembers`'s
    /// existing snapshot read, same delivery posture as `onMembers`. Drives
    /// `AppState.selfIsOp`, which gates the Kick/Ban context-menu items.
    public var onSelfOp: (@Sendable (Bool) -> Void)?
    /// Fired on the MAIN thread for every inbound `.sound` event (nick, file)
    /// — Plan 4b Task 9. Session-level playback callback: fires regardless of
    /// which transcript the event was recorded to (a channel-borne `.sound`
    /// still appends to that room's transcript above `handleLocked`'s switch,
    /// like any other channel-scoped event — only PLAYBACK is uniformly
    /// session-level). The app layer (`AppState`) owns the actual
    /// `AVAudioPlayer` + `SoundLibrary` resolution; this Kit stays
    /// AVFoundation-free.
    public var onSound: (@Sendable (String, String) -> Void)?
    /// Live-fix 3 (crypthome.com, live-reproduced: `432 Anonymous :Reserved
    /// name` dead-ended the session with only a transient status line).
    /// Fired on the MAIN thread from the `.nickRejected` case — `(badNick,
    /// humanText)`, where `humanText` is a ready-to-show sentence (not just
    /// the raw wire reason) so the app layer can present it directly (an
    /// alert, per the app-side wiring). The session is NOT torn down here —
    /// the app layer owns disconnecting/reopening the connect sheet (mirrors
    /// `onStatus`/every other "surface it, app decides what to do" callback
    /// on this type).
    public var onNickRejected: (@Sendable (String, String) -> Void)?
    /// Live-fix 4 (Tim's request: "show the MOTD like the original client";
    /// also fixes error-notes being clobbered by the next status line).
    /// Fired on the MAIN thread with ONE formatted line for each of
    /// `.motd`/`.statusLine`/`.error`/`.disconnectedHint` — the running
    /// server-messages console (`ServerConsoleWindow`) accumulates these
    /// separately from `onStatus` (which only ever shows the LATEST line,
    /// clobbering whatever came before it). Fires IN ADDITION to the
    /// existing `onStatus`/`emitStatus` calls those same cases already make
    /// — this is an additional accumulating view onto the same information,
    /// not a replacement.
    public var onServerMessage: (@Sendable (String) -> Void)?
    /// Auto-reconnect (spec §7): fired on the MAIN thread whenever the reconnect
    /// state machine changes — `.reconnecting(attempt:of:)` while a backoff
    /// loop is running, `.connected` once a reconnect (or the initial connect)
    /// succeeds, `.gaveUp` after the attempt cap is hit. The app layer
    /// (`AppState`) uses this to drive a non-modal indicator (a status line +
    /// a "reconnecting…" sidebar header). Distinct from `onStatus`/
    /// `onServerMessage` (which carry the human-readable text lines this fires
    /// ALONGSIDE): this is the structured signal the UI keys its indicator off,
    /// so it doesn't have to string-match status text.
    public var onReconnectStateChanged: (@Sendable (ReconnectState) -> Void)?
    /// Fired on the MAIN thread (Plan 4b Batch B) for a mention (a room
    /// `.text`/`.action` whose text contains our own nick) or an inbound
    /// whisper (either wire shape) — see `NotificationEvent`'s own doc comment
    /// for the exact detection rule and the `fromServer`/ignore guards. The
    /// app layer (`AppState`) turns this into a `UNUserNotificationCenter`
    /// post, gated on the app being inactive — this Kit stays
    /// UserNotifications-free (that framework needs a real app bundle,
    /// unavailable under `swift test`).
    public var onNotificationEvent: (@Sendable (NotificationEvent) -> Void)?

    /// `var` (Plan 4b Task 5): `changeCharacter` updates the model's OWN
    /// notion of `config.characterName` so a subsequent `reflowLocked()` (or
    /// any other reader of `config`) sees the character actually in effect —
    /// engine-queue only, like every other piece of this type's mutable
    /// state (see `setUpStripLocked`'s own `config.characterName`/
    /// `config.backdropName` reads, which run on the engine queue via
    /// `start()`/`reflowLocked()`).
    private var config: ChatConfig
    /// The character `config.characterName` held at `init` time — set ONCE
    /// and never mutated afterward (Plan 4b Task 5 fix round 1, transcript-
    /// doctrine finding). `setUpStripLocked` uses THIS (not the possibly-
    /// since-changed `config.characterName`) to seed the self participant
    /// during a REFLOW: `reflowLocked` re-`apply`s the WHOLE `_transcript` in
    /// order, which (as of this fix) now contains a `.appearsAs` entry at the
    /// exact position any `changeCharacter` call happened — seeding with the
    /// CURRENT (post-switch) character instead would render every pre-switch
    /// panel with the wrong avatar (the panel is built before replay ever
    /// reaches the `.appearsAs` entry that's supposed to introduce it), which
    /// is the bug this fix closes. A fresh (never-reflowed) session has no
    /// `.appearsAs` self-switch in its transcript yet, so seeding with
    /// `initialCharacterName` there is simply seeding with the same value
    /// `config.characterName` already holds — behaviorally identical to
    /// before this fix.
    private let initialCharacterName: String
    private let engineQueue = DispatchQueue(label: "com.comicchat.engine")
    /// `var` (auto-reconnect, spec §7): a dropped `ProtocolSession`'s
    /// `cc_session` C state is stale and cannot be revived in place, so a
    /// reconnect creates a FRESH `ProtocolSession` (same config) and rebinds
    /// this reference to it. Every `session.` reference on this type reads this
    /// `var` at call time, and every `Task { [session] in … }` closure captures
    /// whichever session was current when it was created — a stale-session
    /// capture's outbound call simply fails harmlessly (`.notConnected`), never
    /// touching the live one. All rebinds happen on `engineQueue` (via
    /// `reconnectAttemptLocked`), the same queue every other `session`-touching
    /// path funnels through, so no reader ever observes a half-swapped
    /// reference. Restructured from `let` carefully: the event-consumer `Task`
    /// (`startEventConsumer`) and the `currentOwnNick` mirror are both re-bound
    /// as part of each rebuild (see `reconnectAttemptLocked`).
    private var session: ProtocolSession

    // MARK: Engine-queue-owned state (touched ONLY on `engineQueue`)

    /// Per-room state (Plan 4b Task 7: true multi-room), keyed by
    /// `roomKey(_:)` (live-fix 1 — see that method's doc comment for why this
    /// is a case-folded canonical form rather than the raw channel string),
    /// engine-queue-owned. Each room's `transcript` is the canonical event log
    /// for THAT room — every `ProtocolEvent` scoped to it, in arrival order,
    /// INCLUDING the synthetic self-say events `send(_:)` injects and the
    /// synthetic self-character-switch `.appearsAs` (which is appended to
    /// EVERY room's transcript — clarification 1). The ACTIVE room's
    /// transcript is what `setViewport`'s reflow replays against (D2 §2.3:
    /// "the transcript = the event log; reflow = destroy strip -> recreate ->
    /// re-apply transcript"). `roomOrder` preserves join order for the tab bar
    /// (also canonical keys — resolve `rooms[key]!.displayName` for anything
    /// user-visible).
    private var rooms: [String: RoomBox] = [:]
    /// Join order (initial room first), canonical keys — the order
    /// `RoomTabBar` shows tabs (via `roomInfosLocked()`, which resolves each
    /// key's `displayName`).
    private var roomOrder: [String] = []
    /// The CANONICAL KEY (`roomKey(_:)`) of the room whose transcript
    /// currently owns the ONE live strip. Seeded from `roomKey(config.room)`
    /// in `start()`. `setActiveRoom` swaps it (destroy strip -> rebuild from
    /// the new room's transcript -> recompose). Always look up
    /// `rooms[activeRoom]?.displayName` for anything user-visible (the strip
    /// title, wire calls) — `activeRoom` itself is a lookup key, not
    /// necessarily what the server or user typed.
    private var activeRoom: String = ""

    /// Live-fix 1 (crypthome.com, live-reproduced — `JOIN #crypt` echoed back
    /// as `#Crypt` created a permanently-empty second tab): canonicalizes a
    /// channel name for internal room-bookkeeping keys (`rooms`/`roomOrder`/
    /// `activeRoom`), matching IRC's case-insensitive channel-name rule
    /// (RFC 1459 §2.3.1: channels fold ASCII letters AND `[]\^` <->`{}|~`).
    ///
    /// Plan 4b live-fix 6, Fix 1 update: the C engine's own room-token table
    /// (`ccSessionRoomTokenForChannel`, `engine/ircsock.cpp`; mirrored by
    /// `ccSessionIsJoinedChannel`, `bridge/cc_session.cpp`) is now ALSO
    /// case-insensitive (`stricmp` — a plain ASCII fold, matching the
    /// original's `LookupDoc`, chatdoc.cpp:2021-2031), closing a fidelity
    /// gap where inbound events on a differently-cased channel (peer
    /// messages/joins arriving before or between join-confirms, or naming a
    /// THIRD casing the join-confirm never used) were stranded
    /// session-scoped. The two layers do NOT need to agree on the exact fold
    /// (`stricmp`'s plain ASCII vs. this method's RFC-1459-extended
    /// `[]\^`<->`{}|~` fold) — this method re-folds whatever casing the
    /// engine hands back through `roomKey` before ever comparing it to
    /// `activeRoom` (see `handleLocked`'s `scopedKey = scopedRoom.map(roomKey)`),
    /// so any case-insensitive engine fold is sufficient; only THIS type's
    /// own fold needs to be the RFC-1459-correct one for keys/UI-visible
    /// comparisons. The engine's token table still keys off whatever string
    /// Swift last registered/renamed via `cc_session_register_room`/
    /// `cc_session_update_room_channel` (`ProtocolSession.join(_:)`/the
    /// `.selfJoined` rename path) — this method's own fold remains the
    /// authority for Swift-side bookkeeping identity.
    private func roomKey(_ channel: String) -> String {
        var folded = channel.lowercased()
        // RFC 1459 §2.3.1's extended fold: `[]\^` <-> `{}|~` are the
        // lowercase/uppercase pairs for the four punctuation characters IRC
        // permits in a nick/channel name (a nick-casing rule channels
        // inherit). `String.lowercased()` above already folds ASCII letters;
        // this closes the remaining four characters case-insensitivity would
        // otherwise miss (e.g. a server that echoes `#a-team[mac]` back as
        // `#A-Team{MAC}` must still resolve to the same room).
        let foldMap: [Character: Character] = ["[": "{", "]": "}", "\\": "|", "^": "~"]
        folded = String(folded.map { foldMap[$0] ?? $0 })
        return folded
    }
    /// Session-scoped event log (Plan 4b Task 7, clarification 2): whisper/
    /// status/login/error events (token 0 — `channel == nil`) that belong to
    /// no single room. Kept for Task 10 (save) / Task 11 (text view), which
    /// consume per-room transcript + relevant session events.
    ///
    /// NOTE (Task 9 wording correction): `.sound` is NOT always session-scoped
    /// like this list's other members — a channel-borne `.sound` (the engine
    /// tokens the whole PRIVMSG payload switch, same as `.text`/`.action`)
    /// routes to THAT ROOM's transcript below, exactly like any other
    /// channel-scoped event; only a session-scoped `.sound` (no room token)
    /// lands here. What IS uniformly session-level is PLAYBACK — `onSound`
    /// fires regardless of which transcript the event was recorded to (see
    /// `handleLocked`'s `.sound` case).
    private var sessionTranscript: [ProtocolEvent] = []
    /// Convenience read of the ACTIVE room's transcript (the `transcript`
    /// public accessor's backing). Per-room appends go directly to
    /// `rooms[room].transcript` in `handleLocked`; `rebuildStripLocked` replays
    /// a specific room's transcript by name — so this is a read-only view.
    private var _transcript: [ProtocolEvent] {
        rooms[activeRoom]?.transcript ?? []
    }
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
    /// nicks this model has already sent a private reply-announce to (Task
    /// 8's `toNick:` announce) — guards the "first `.appearsAs` from an
    /// unseen nick" rule so a nick's later avatar changes don't re-announce.
    private var announcedBackTo: Set<String> = []
    /// Plan 4b Batch C: last-reply timestamp per nick, shared by BOTH
    /// `.versionRequest` and `.infoRequest` (one dict, not two — a peer
    /// hammering us with alternating VERSION/GetInfo probes is still one
    /// "nick spamming probes" case, not two independent budgets). A
    /// deliberate MODERN guard the original never had (its own
    /// ReplyVersion/GetInfo-reply branches only gated on ignore/flood
    /// flags, never a probe-specific cooldown) — added per the task brief's
    /// explicit ask ("one reply per nick per 10s"), engine-queue-local like
    /// `announcedBackTo` above since both are read/written only from
    /// `handleLocked` (ENGINE QUEUE ONLY).
    private var lastProbeReplyAt: [String: Date] = [:]
    /// The rate-limit window `lastProbeReplyAt` enforces (Plan 4b Batch C).
    private static let probeReplyCooldown: TimeInterval = 10
    /// Own-say echo dedup (4a final-review carryover, Plan 4b Task 1): some
    /// IRC servers echo a client's own PRIVMSG back to the sender. `send(_:)`
    /// already renders the own line immediately via a synthetic local
    /// `.text` event (that method's doc comment) — a server echo of the SAME
    /// text must be dropped rather than rendered a second time. Holds exactly
    /// the texts of sends still awaiting a possible echo; `handleLocked`
    /// removes (at most) one matching entry per server-originated `.text`
    /// with our own nick.
    private var pendingLocalEchoes: [String] = []
    /// The `.sound` sibling of `pendingLocalEchoes` above (outbound-sound
    /// task). `sendSound` already renders/plays the own sound immediately via
    /// a synthetic local `.sound` event (see that method's doc comment) — a
    /// server echo of the SAME sound line (round-tripping back through this
    /// engine's own `ccPrepareSound` parser, same as any other self-sent
    /// PRIVMSG some servers echo) must be dropped rather than played/appended
    /// a second time. Keyed on `file` alone (not `file+text`): unlike `send`'s
    /// free-form message text, sound's `text` argument is near-always empty
    /// in this UI (the picker never asks for one, matching the original's
    /// dialog default), so two back-to-back sends of the SAME file with
    /// different text would be a rare, deliberately-distinct case anyway —
    /// `file` is the wire-identifying field (`ccPrepareSound`'s `outFile`) and
    /// is what actually drives playback, so it's the correct dedup key.
    private var pendingLocalSoundEchoes: [String] = []
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

    // MARK: Auto-reconnect (spec §7) — engine-queue-owned

    /// The in-flight backoff-reconnect `Task`, or `nil` when no reconnect loop
    /// is running. Spawned by `handleLocked`'s `.disconnectedHint` case on an
    /// UNEXPECTED drop; cancelled by `shutdown()` (which calls `cancelReconnect()`
    /// as its first step) and directly by `cancelReconnect()` itself — the
    /// app layer's disconnect-before-connect path (`AppState.disconnect()` ->
    /// `ChatSessionModel.shutdown()`) is what actually runs before a manual
    /// reconnect (a manual reconnect/disconnect always wins). Engine-queue-owned
    /// like every other piece of this section's state.
    private var reconnectTask: Task<Void, Never>?
    /// `true` while a reconnect loop is active — guards `.disconnectedHint`
    /// against spawning a SECOND overlapping loop (a fresh session that fails
    /// to even connect emits its own `.disconnectedHint`, which must feed the
    /// existing loop's next attempt rather than starting a rival one).
    private var isReconnecting = false
    /// Max backoff-reconnect attempts before giving up (spec §7's "after N
    /// failed attempts"). 8 attempts across the capped-exponential schedule
    /// (2,4,8,16,32,60,60,60s) spans ~4.5 minutes of retrying — enough to ride
    /// out a transient drop without hammering a genuinely-dead server forever.
    private let maxReconnectAttempts = 8

    /// ENGINE QUEUE ONLY. `true` when this model should auto-reconnect on an
    /// unexpected drop: the config opted in AND we're not shutting down AND a
    /// live session was previously established (a fresh session that never
    /// logged in has `currentColumns`-style seed state but no rooms confirmed —
    /// however the reconnect trigger is `.disconnectedHint`, which the initial
    /// `connect()` failure surfaces via a THROW from `start()`, not an event,
    /// so this predicate needn't special-case first-connect: `.disconnectedHint`
    /// only ever reaches `handleLocked` from a session that got past
    /// `connect()`). Replay-fixture sessions opt out via `config.autoReconnect`.
    private var shouldAutoReconnect: Bool {
        config.autoReconnect && !isShutDown
    }

    /// Engine-queue-owned in-flight guard (Plan 4b Task 6 fix round 1):
    /// avatar NAMEs (not nicks — the download destination is name-keyed,
    /// same key `downloadAvatarIfNeededLocked` fetches by) currently being
    /// fetched by a detached download `Task`. Without this, N `.appearsAs`
    /// announces for the same unresolved name arriving before the first
    /// download lands would each pass `downloadAvatarIfNeededLocked`'s
    /// `bridge.resolvesName` check (still unresolved — the first download
    /// hasn't landed yet) and spawn N redundant concurrent fetches of the
    /// same URL. Inserted into synchronously (engine queue, before the
    /// detached `Task` is spawned) and removed on EVERY exit of that `Task`
    /// (success or thrown error) via an `engineQueue.async` hop back —
    /// touched ONLY on `engineQueue`, like every other piece of state in
    /// this section.
    private var inFlightAvatarDownloads: Set<String> = []

    // MARK: Ignore (quick-wins batch item 5, original CUserInfo m_bIgnored)

    /// Engine-queue-owned, session-scoped (not per-room — the original's
    /// per-user ignore flag is global to the user, not scoped to a channel)
    /// set of ignored nicks, keyed by a case-folded form (`ignoreKey(_:)`)
    /// so "Ignore Bob" also silences "BOB"/"bob" — the same case-insensitive
    /// posture nick comparisons use elsewhere on this type (e.g.
    /// `caseInsensitiveCompare` in `handleLocked`'s `.appearsAs` case).
    ///
    /// DOCTRINE (binding, load-bearing — read before touching either
    /// apply site below): ignore is a VIEW-side filter over the log, NOT a
    /// drop-at-ingest rule. Events from an ignored nick are still appended to
    /// `_transcript`/`sessionTranscript` above in `handleLocked` (the log
    /// stays canonical — an unignore later must be able to render the
    /// skipped history, and `.text`/`.action`/`.whisper`/`.sound`'s transcript
    /// bookkeeping/whisper-box routing/unread counting are all UNCHANGED by
    /// ignore). The filter applies ONLY at the two RENDER-ROUTING sites that
    /// turn a transcript entry into a strip panel: `handleLocked`'s own
    /// live `applyToBridgeLocked`/`recomposeLocked` calls (the `.text`/
    /// `.action` cases route through `handleRoomMessageStripEffect`, guarded
    /// below) AND `rebuildStripLocked`'s replay loop (a reflow re-derives the
    /// ENTIRE strip from the transcript from scratch — without the SAME guard
    /// there, an ignored nick's already-transcript-recorded lines would
    /// silently resurrect on the very next viewport resize/tab switch, since
    /// replay has no memory of what live rendering chose to skip). Both sites
    /// must agree, or ignore becomes a flaky "sometimes filtered" feature
    /// instead of a real view.
    private var ignoredNicks: Set<String> = []

    /// Case-fold for `ignoredNicks` keys — plain ASCII lowercase, matching
    /// the `caseInsensitiveCompare` posture used for nick comparisons
    /// elsewhere on this type (`roomKey(_:)`'s own RFC-1459 `[]\^`<->`{}|~`
    /// extended fold is a CHANNEL-name rule, not applicable to a bare nick).
    private func ignoreKey(_ nick: String) -> String { nick.lowercased() }

    /// Toggles whether `nick` is ignored (quick-wins batch item 5's member
    /// context-menu action). Fire-and-forget: hops onto the engine queue,
    /// mutates `ignoredNicks`, then reflows the ACTIVE room so the change is
    /// visible immediately — an unignore must re-render the nick's history
    /// (the log kept it, per the doctrine above), and an ignore must retract
    /// any of the nick's panels already showing on the live strip. Reusing
    /// `rebuildStripLocked` (the same "replay is the reflow" machinery
    /// `setViewport`/`setActiveRoom` already use) guarantees the two apply
    /// sites (live + replay) are re-derived from the SAME transcript with the
    /// SAME guard, rather than trying to surgically patch the already-composed
    /// strip.
    public func setIgnored(_ nick: String, _ ignored: Bool) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            let key = self.ignoreKey(nick)
            if ignored {
                self.ignoredNicks.insert(key)
            } else {
                self.ignoredNicks.remove(key)
            }
            guard !self.activeRoom.isEmpty else { return }
            self.rebuildStripLocked(for: self.activeRoom, resetAnnounce: false)
        }
    }

    /// Thread-safe snapshot check — `MemberRow.isIgnored`'s source
    /// (`emitMembers`'s detached `Task` cannot touch engine-queue state
    /// directly, so the ignored-set membership is captured on-queue at
    /// snapshot time, same posture as `announcedAvatarNames`/`ownCharacterName`
    /// in that method).
    public var ignoredNicksSnapshot: Set<String> {
        engineQueue.sync { ignoredNicks }
    }

    // MARK: Room list (Plan 4b Task 8)

    /// Engine-queue-owned accumulator for the current `.roomListBegin` ->
    /// `.roomListItem`×N -> `.roomListEnd` round-trip (`CC_EV_ROOM_LIST_ITEM`
    /// is emitted once per LIST reply line, `ircsock.cpp`'s RPL_LIST/322
    /// handler) — `nil` between round-trips (no LIST in flight) so a stray
    /// `.roomListItem`/`.roomListEnd` with no preceding `.roomListBegin`
    /// (shouldn't happen; defensive only) is dropped rather than firing
    /// `onRoomList` with a garbage partial list. `.roomListBegin` resets it to
    /// `[]`; each `.roomListItem` appends; `.roomListEnd` fires `onRoomList`
    /// once with the full array and resets to `nil`.
    private var roomListAccum: [RoomListItem]?
    /// Set the moment `requestRoomList()` puts the wire LIST out (BEFORE any
    /// server reply — unlike `roomListAccum`, which only becomes non-nil once
    /// `.roomListBegin` actually arrives), cleared on `.roomListEnd`. Guards
    /// `requestRoomList()` itself against issuing a SECOND wire LIST while an
    /// earlier one is still in flight (self-review fix): gating on
    /// `roomListAccum != nil` alone has a window between "LIST sent" and
    /// "`.roomListBegin` received" where a second call would see `nil` and
    /// send its own LIST too — two overlapping round-trips sharing the one
    /// accumulator could then interleave and splice/corrupt the result.
    private var roomListRequestInFlight = false

    // MARK: Whisper (Plan 4b Task 4)

    /// Engine-queue-owned whisper transcripts, keyed by peer nick. Appended
    /// to from `handleLocked`'s `.whisper` case (inbound) and from
    /// `sendWhisper` (own outbound line) -- see `whisperHistories` (the
    /// public, thread-safe snapshot reader) below.
    private var _whisperHistories: [String: [WhisperLine]] = [:]
    /// Internal seam (reachable via `@testable import` for
    /// `WhisperRoutingTests`' drop test) now wired to `config.acceptWhispers`
    /// at `init` (Task 5) -- an inbound whisper is dropped from history (and
    /// surfaces via `onStatus` instead) while this is `false`. Kept as a
    /// separate property (rather than reading `config.acceptWhispers`
    /// directly at the point of use) so a test can still flip it after
    /// construction without needing a full `ChatConfig` rebuild. Engine-queue
    /// only. Default `true` matches `ChatConfig.acceptWhispers`'s own default.
    var _acceptWhispers: Bool = true

    public init(config: ChatConfig) {
        self.config = config
        self.initialCharacterName = config.characterName
        self.currentOwnNick = config.nick
        self.session = ProtocolSession(host: config.host, port: config.port, nick: config.nick,
                                       encoding: config.encoding, engineQueue: engineQueue,
                                       userName: config.userName, realName: config.realName)
        // Task 4's internal test-only seam, now wired to a real setting
        // (Task 5) — `_acceptWhispers` keeps its `true` default when
        // `config.acceptWhispers` is left at ITS default, so
        // `WhisperRoutingTests`' drop test (which constructs `ChatConfig`
        // without this parameter) is unaffected.
        self._acceptWhispers = config.acceptWhispers
    }

    /// Plan 4b Task 6: where downloaded custom avatars land —
    /// `~/Library/Application Support/Comic Chat/Characters`. Created on
    /// first use (idempotent — `withIntermediateDirectories: true`); a
    /// `computed` property (not cached) since it's cheap path arithmetic and
    /// callers (the resolver's `extraDirs`, `downloadAvatarIfNeededLocked`)
    /// each want the directory to exist by the time they use it.
    public static var userCharactersDir: String {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory() + "/Library/Application Support")
        let dir = base.appendingPathComponent("Comic Chat").appendingPathComponent("Characters")
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.path
    }

    /// Resolves `avatarName` (a bare comicart name, as carried by
    /// `MemberRow.avatarName`) to a concrete `.avb` path, using the SAME
    /// search order `setUpStripLocked`'s `AvatarResolver` uses for the live
    /// strip: `userCharactersDir` (downloaded/custom avatars) BEFORE
    /// `config.artDir` (the bundled set) — D1 §4.3, so a member wearing a
    /// custom avatar that shadows a bundled name of the same nick resolves
    /// to the SAME file here as it does on the strip. Plan 4b Task 8
    /// (self-review fix): `AppState.resolveMemberIcon`'s member-row icon
    /// lookup previously hardcoded `artDir` only, silently diverging from the
    /// strip's own resolution for exactly this case. Returns `nil` for an
    /// empty name or a name that resolves to no file on disk anywhere
    /// (mirrors `AvatarResolver.resolvesName`'s "no cycled-default fallback"
    /// contract — an unresolved icon should show NO icon, not a random one).
    /// Thread-safe / does not require the engine queue: pure filesystem
    /// lookups against immutable `config.artDir` and the fixed
    /// `userCharactersDir` path, no strip/engine state touched.
    public func resolveAvatarPath(_ avatarName: String) -> String? {
        guard !avatarName.isEmpty else { return nil }
        let bare = avatarName.lowercased().hasSuffix(".avb") ? avatarName : "\(avatarName.lowercased()).avb"
        for dir in [Self.userCharactersDir, config.artDir] {
            let candidate = (dir as NSString).appendingPathComponent(bare)
            if FileManager.default.fileExists(atPath: candidate) { return candidate }
        }
        return nil
    }

    /// Thread-safe snapshot of the ACTIVE room's event log so far (Plan 4b
    /// Task 7: per-room transcripts — this returns the active room's, matching
    /// the single-room callers' pre-Task-7 expectations; use
    /// `transcript(for:)` for a specific room).
    public var transcript: [ProtocolEvent] {
        engineQueue.sync { _transcript }
    }

    /// Thread-safe snapshot of `room`'s event log (Plan 4b Task 7), or `[]` if
    /// that room isn't joined. Task 10/11 read this per-room. `room` is
    /// case-folded via `roomKey(_:)` (live-fix 1) so a caller can pass either
    /// the display-cased name (e.g. from `RoomInfo.name`) or any other casing
    /// and still resolve the same room.
    public func transcript(for room: String) -> [ProtocolEvent] {
        engineQueue.sync { rooms[roomKey(room)]?.transcript ?? [] }
    }

    /// Thread-safe snapshot of the session-scoped event log (Plan 4b Task 7,
    /// clarification 2) — whisper/status/login events that belong to no
    /// single room. A channel-borne `.sound` is NOT included here (Task 9
    /// wording correction — see `sessionTranscript`'s doc comment); it lands
    /// in that room's transcript like any other channel-scoped event. Task
    /// 10/11 consume this alongside a room's transcript.
    public var sessionEvents: [ProtocolEvent] {
        engineQueue.sync { sessionTranscript }
    }

    /// Thread-safe snapshot of the joined rooms in tab order (Plan 4b Task 7).
    public var roomInfos: [RoomInfo] {
        engineQueue.sync { roomInfosLocked() }
    }

    /// Thread-safe snapshot of the active room's DISPLAY name (Plan 4b Task
    /// 7; live-fix 1 — `activeRoom` itself is now a case-folded lookup key,
    /// not necessarily what the server or user typed, so this resolves
    /// `rooms[activeRoom]?.displayName` rather than returning the key
    /// directly. Falls back to the raw key if the active room somehow has no
    /// box (shouldn't happen — defensive only).
    public var currentRoom: String {
        engineQueue.sync { rooms[activeRoom]?.displayName ?? activeRoom }
    }

    /// Thread-safe snapshot of our own current nick (final-review Important
    /// #4a): `AppState.recomputeTranscriptText`'s text-view filter needs this
    /// to call the shared `ProtocolEvent.isWhisperShapedText` predicate the
    /// same way `handleLocked` does internally via `currentOwnNick` — that
    /// property itself is engine-queue-owned/private (its own doc comment
    /// explains why a bare read isn't safe), so this is the thread-safe
    /// read-through, same `engineQueue.sync` posture as `currentRoom`.
    public var ownNick: String {
        engineQueue.sync { currentOwnNick }
    }

    /// Thread-safe snapshot of the strip's current panel count (`Strip.panelCount`,
    /// 0 if no strip has been built yet — e.g. before `start()`). Exposed for
    /// callers/tests that need to observe a recompose actually having
    /// happened (e.g. after `changeCharacter`/`changeBackdrop` + a send) —
    /// same `engineQueue.sync` read-through pattern as `transcript`.
    public var panelCount: Int32 {
        engineQueue.sync { strip?.panelCount ?? 0 }
    }

    /// Thread-safe snapshot of the strip's current panel geometry
    /// (`Strip.panelGeometry`), `nil` if no strip has been built yet. Quick-wins
    /// batch item 3: lets a test observe `setViewport`'s resolved `perRow`
    /// directly (whether auto-fit or `config.panelsPerRow`-forced) rather than
    /// re-deriving it from `PanelFit` itself — same `engineQueue.sync`
    /// read-through posture as `panelCount`.
    public var panelGeometry: (unitW: Int32, unitH: Int32, perRow: Int32, hInter: Int32, vInter: Int32)? {
        engineQueue.sync { strip?.panelGeometry }
    }

    /// Plan 4b Task 10: the config snapshot `conversationFile()` needs — read
    /// through one `engineQueue.sync` (matching `transcript`/`currentRoom`'s
    /// own thread-safe-read posture, since `config` is engine-queue-owned:
    /// `changeCharacter`/`changeBackdrop` mutate it ON the engine queue).
    /// `characterName` here is `initialCharacterName` (the character this
    /// session's self participant was actually SEEDED with), NOT
    /// `config.characterName` — see `ConversationFile.characterName`'s doc
    /// comment for why a save must record the seed character, not whatever a
    /// mid-session `changeCharacter` has since moved `config.characterName`
    /// to (that switch is already replay-visible as a `.appearsAs` transcript
    /// entry; seeding a reopen with the post-switch character instead would
    /// repaint every pre-switch panel with the wrong avatar).
    public var saveConfigSnapshot: (host: String, nick: String, characterName: String,
                                     backdropName: String, encoding: WireEncoding, artDir: String) {
        engineQueue.sync {
            (config.host, config.nick, initialCharacterName, config.backdropName,
             config.encoding, config.artDir)
        }
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
            // Seed the initial room (Plan 4b Task 7) BEFORE building the strip:
            // `activeRoom`/`rooms[activeRoom]` must exist so `_transcript`
            // (now the active room's transcript) and the strip title resolve.
            // Live-fix 1: `activeRoom`/`rooms` key on `roomKey(config.room)`
            // (case-folded); `displayName` starts as the raw, locally-typed
            // `config.room` and is updated to the server's own casing the
            // moment `.selfJoined` confirms the join (`handleLocked`'s
            // `.selfJoined` case, via `ensureRoomBoxLocked`).
            let key = self.roomKey(self.config.room)
            self.activeRoom = key
            if self.rooms[key] == nil {
                self.rooms[key] = RoomBox(displayName: self.config.room)
                self.roomOrder = [key]
            }
            try self.setUpStripLocked(isReflow: false)
        }
        startEventConsumer()
        emitRooms()
        try await session.connect()
    }

    /// Engine-queue-only: installs the metrics canvas BEFORE creating the
    /// `Strip` (fonts are measured once per strip -- `cc_set_metrics_canvas`
    /// must be live before `Strip()`'s `cc_strip_create` call), seeds panel
    /// geometry from `PanelFit`'s defaults (3 columns, until a real viewport
    /// arrives via `setViewport`), creates the strip + bridge, sets the
    /// title (so panel 0 exists even before any chat line), sets the
    /// backdrop, and adds+registers the self participant.
    ///
    /// - Parameter isReflow: `false` for the one `start()` call site (a fresh
    ///   session — no transcript exists yet, so there is nothing to replay
    ///   forward from); `true` for `reflowLocked`'s call site. Selects which
    ///   character seeds the self participant: a fresh session seeds
    ///   `config.characterName` (the character it was actually started with —
    ///   equivalent to `initialCharacterName` at this point, since nothing has
    ///   changed it yet); a reflow seeds `initialCharacterName` UNCONDITIONALLY,
    ///   even if `changeCharacter` has since moved `config.characterName`
    ///   forward, because `reflowLocked` immediately re-`apply`s the whole
    ///   `_transcript` afterward — which (Plan 4b Task 5 fix round 1) now
    ///   contains a `.appearsAs` entry at the exact position any character
    ///   switch happened. Seeding with the POST-switch character here would
    ///   have every pre-switch panel painted with the wrong avatar before
    ///   replay ever reaches the entry that's supposed to introduce it —
    ///   exactly the transcript-doctrine violation this fix closes. See
    ///   `initialCharacterName`'s own doc comment.
    private func setUpStripLocked(isReflow: Bool) throws {
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
        // Title = the ACTIVE room's DISPLAY name (Plan 4b Task 7; live-fix 1:
        // `activeRoom` is now a case-folded lookup key, not necessarily what
        // the server/user typed — resolve `displayName` for anything shown to
        // the user). On a fresh `start()`, `activeRoom` was just seeded to
        // `roomKey(config.room)`; on a reflow or a `setActiveRoom` rebuild, it
        // is the room whose transcript is about to be replayed — so the
        // strip's starring panel names the right room.
        try newStrip.setTitle(rooms[activeRoom]?.displayName ?? activeRoom)
        // extraDirs (Plan 4b Task 6): the user characters dir is searched
        // BEFORE comicartDir (D1 §4.3) — a downloaded avatar shadows a
        // same-named bundled one, and (the reflow-coherence payoff) a
        // reflow's transcript replay re-resolves the announced name against
        // the downloaded file naturally, with no special-casing needed here.
        let resolver = ProtocolStripBridge.AvatarResolver(
            comicartDir: config.artDir,
            extraDirs: [Self.userCharactersDir])
        let newBridge = try ProtocolStripBridge(strip: newStrip, resolver: resolver, encoding: config.encoding)
        try newBridge.setBackdrop(config.artDir + "/" + config.backdropName + ".bgb")

        let seedCharacterName = isReflow ? initialCharacterName : config.characterName
        let avbPath = config.artDir + "/" + seedCharacterName + ".avb"
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

        // Live-fix 7: emit the neutral self pose immediately so the pose pane
        // shows the standing avatar from launch (and re-shows it after a
        // reflow), rather than staying blank until the first wheel drag /
        // typing preview. `setSelf(selfID)` above means `cc_strip_self_preview`
        // can already resolve the self avatar; the avatar's freshly-loaded
        // `m_body` is its neutral pose.
        emitSelfPoseLocked()
    }

    // MARK: - Event consumer

    /// Starts (or, on a reconnect rebind, RE-starts) the event-consumer `Task`
    /// bound to the CURRENT `session`. Auto-reconnect (spec §7): each reconnect
    /// creates a fresh `ProtocolSession` with its own `events` stream, so the
    /// consumer must be re-pointed at the new stream. The `session` reference is
    /// re-read here (not captured) so a rebind takes effect; the OLD session's
    /// stream finishes when its `ProtocolSession` is torn down in
    /// `reconnectAttemptLocked`, ending the old `for await` loop naturally.
    /// Cancels any prior consumer first so two loops never run at once.
    private func startEventConsumer() {
        consumerTask?.cancel()
        let currentSession = session
        consumerTask = Task { [weak self] in
            guard let self else { return }
            for await scoped in currentSession.events {
                self.enqueueHandle(scoped.event, channel: scoped.channel)
            }
        }
    }

    /// Hops onto the engine queue and routes one event. Called from the
    /// event-consumer `Task` (off the engine queue, `fromServer: true` — the
    /// default) and from `send(_:)`'s synthetic self-say (`fromServer:
    /// false`) — both funnel through this single entry point so
    /// `_transcript`/`announcedBackTo`/`loggedInContinuation` have exactly
    /// one serialized owner.
    private func enqueueHandle(_ ev: ProtocolEvent, channel: String? = nil, fromServer: Bool = true) {
        engineQueue.async { [weak self] in
            self?.handleLocked(ev, channel: channel, fromServer: fromServer)
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
    ///   .text (strip-relevant)  -> own-say echo dedup first (see below),
    ///                              THEN (Plan 4b Task 4 fix, §8 Topology A
    ///                              plain-IRC interop finding) if the PRIVMSG
    ///                              targets our own nick or carries cooked
    ///                              SM_WHISPER-mode annotations, ALSO route
    ///                              to `whisperBoxRoutingLocked` -- a plain
    ///                              IRC whisper classifies `CC_EV_TEXT`, not
    ///                              `CC_EV_WHISPER` (that helper's doc
    ///                              comment). THEN (unconditionally)
    ///                              bridge.apply + recompose.
    ///   .action (strip-relevant) -> bridge.apply + recompose
    ///   .whisper                -> Plan 4b Task 4: route to
    ///                              `whisperBoxRoutingLocked` (the IRCX
    ///                              WHISPER verb path), THEN (unconditionally)
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
    ///   .sound                  -> onSound (session-level playback callback,
    ///                              Task 9); transcript placement follows the
    ///                              room token like any other event — NOT
    ///                              forced session-scoped (see
    ///                              `sessionTranscript`'s doc comment)
    ///
    /// - Parameter channel: the room this event is scoped to (Plan 4b Task 7),
    ///   `nil` for session-scoped events (whisper/status/login — see
    ///   `ScopedEvent`'s doc comment; a `.sound` MAY also be session-scoped if
    ///   it carries no room token, but a channel-borne one is not forced here
    ///   the way this list's other members are). A channel-scoped event
    ///   appends to THAT room's transcript and drives the live strip ONLY when
    ///   it is the active room; otherwise it bumps that room's unread (messages only)
    ///   and refreshes the tab bar. A `nil` channel routes to the SESSION
    ///   transcript and behaves exactly as pre-Task-7 (no strip effect unless
    ///   it's one of the room-agnostic strip events — none are).
    /// - Parameter fromServer: `true` for every event arriving off the wire
    ///   (the event-consumer `Task`'s default); `false` only for `send(_:)`'s
    ///   synthetic self-say / `changeCharacter`'s synthetic self-`.appearsAs`.
    ///   Used by the `.text` case's own-say echo dedup below — a synthetic
    ///   event (`fromServer == false`) never matches the `pendingLocalEchoes`
    ///   check, since it IS the render being kept.
    private func handleLocked(_ ev: ProtocolEvent, channel: String? = nil, fromServer: Bool = true) {
        guard !isShutDown else { return }

        // Own-say echo dedup (4a carryover): some servers echo PRIVMSG back to
        // the sender; our synthetic local echo (send(_:)) already rendered it.
        // Drop exactly one server copy per pending send, BEFORE the transcript
        // append -- reflow (`rebuildStripLocked`, which replays a room's
        // transcript verbatim) must not double-render it either.
        if case .text(let nick, _, _, let text, _, _) = ev,
           fromServer, nick == currentOwnNick,
           let i = pendingLocalEchoes.firstIndex(of: text) {
            pendingLocalEchoes.remove(at: i)
            return
        }

        // Own-sound echo dedup (outbound-sound task, same shape as the
        // own-say dedup just above): `sendSound` already rendered/played the
        // own line via a synthetic local `.sound` event; drop exactly one
        // server copy per pending send, BEFORE the transcript append, same
        // reflow-safety reasoning as the `.text` case.
        if case .sound(let nick, let file, _) = ev,
           fromServer, nick == currentOwnNick,
           let i = pendingLocalSoundEchoes.firstIndex(of: file) {
            pendingLocalSoundEchoes.remove(at: i)
            return
        }

        // --- transcript routing (Plan 4b Task 7) ---------------------------
        // Channel-scoped -> that room's transcript (create the box if the
        // event beat the `.selfJoined` that would have — defensive; normal
        // flow registers the room in `joinRoom`/`.selfJoined`). Session-scoped
        // (`channel == nil`) -> the session transcript. The strip-driving
        // switch below reads `isActiveRoom` to decide live-render vs unread.
        //
        // `scopedRoom` prefers the token-resolved `channel` but falls back to
        // an event's OWN payload channel for the events that carry one
        // (`.selfJoined`/`.selfParted`). This closes a token-registration race:
        // `.selfJoined` is what CONFIRMS a join, but the wire JOIN echo can
        // arrive before the async `session.join(...)` (fired from `.loggedIn`)
        // has registered the room's token — so the token-resolved channel is
        // nil, yet the event payload names the channel unambiguously. Using the
        // payload channel keeps `.selfJoined` in its room's transcript
        // regardless of that timing (verified: FixtureReplayServerTests, whose
        // fixture sends the JOIN echo immediately after 001).
        //
        // Live-fix 1: `scopedRoom` is the DISPLAY-cased channel string exactly
        // as it arrived on the wire (or from the event's own payload) — NOT a
        // lookup key. `scopedKey` is its case-folded form (`roomKey(_:)`),
        // used for every `rooms[...]` dictionary touch below; `scopedRoom`
        // itself is kept around only for wire calls that need the actual
        // channel string (`session.announceAvatar`, etc.) and, via
        // `ensureRoomBoxLocked`, to update a room's `displayName` to the
        // server's own casing.
        //
        // ORDER FLIPPED from the token-resolved-first shape the doc comment
        // above describes (live-fix 1 addendum): `intrinsicChannel(of: ev)`
        // is preferred FIRST for `.selfJoined`/`.selfParted` specifically.
        // `channel` (the token-resolved string) is `cc_session_room_channel`'s
        // return value, which is whatever string THIS SIDE originally passed
        // to `cc_session_register_room` (i.e. `join(_:)`'s caller-supplied
        // name, frozen at registration time — the engine's own channel table
        // never updates it). The event's OWN payload channel, in contrast, is
        // the ACTUAL wire bytes the server just sent (`ircsock.cpp`'s JOIN
        // handler assigns `ev.u.self_joined.channel = pParse->args[1]`
        // verbatim) — which is exactly the server's authoritative casing this
        // fix needs to update `displayName` from. Using the token-resolved
        // channel here (the pre-fix/pre-addendum shape) would silently keep
        // showing whatever casing WE originally typed forever, since it never
        // changes once registered — defeating the fix for the very case it
        // targets (server echoes back a DIFFERENT case than what we joined
        // with). Every other event type is unaffected: `intrinsicChannel`
        // returns `nil` for anything but `.selfJoined`/`.selfParted`, so the
        // `??` falls through to the token-resolved `channel` exactly as
        // before for every other case.
        let scopedRoom = intrinsicChannel(of: ev) ?? channel
        let scopedKey = scopedRoom.map(roomKey)
        let isActiveRoom = (scopedKey != nil && scopedKey == activeRoom)
        // `.selfParted` for a room we already dropped (via `leaveRoom`, which
        // removes the box BEFORE the server's part-confirm echo lands) must
        // NOT re-create the box (which would re-add a phantom tab). It routes
        // to the session transcript instead and the `.selfParted` case below
        // is a no-op for an already-gone room.
        let isPartOfDepartedRoom: Bool = {
            if case .selfParted(let ch) = ev { return rooms[roomKey(ch)] == nil }
            return false
        }()
        if let room = scopedRoom, let key = scopedKey, !isPartOfDepartedRoom {
            ensureRoomBoxLocked(room)
            rooms[key]!.transcript.append(ev)
        } else {
            sessionTranscript.append(ev)
        }

        switch ev {
        case .loggedIn(let nick):
            currentOwnNick = nick   // echo-only rule confirmation, mirrors ProtocolSession's own _ownNick update
            // Auto-reconnect (spec §7): a login on a session we were
            // RECONNECTING through means the reconnect succeeded — clear the
            // loop and notify. `finishReconnectLocked` is a no-op when no
            // reconnect was in flight (the ordinary first login), so this is
            // safe to call unconditionally here.
            let wasReconnecting = isReconnecting
            finishReconnectLocked()
            if wasReconnecting {
                // RECONNECT login: re-join EVERY room the user had open, in the
                // original join order (`roomOrder`), against the FRESH session.
                // The strips/transcripts persist (the boxes live on this model,
                // untouched by the reconnect — see `reconnectAttemptLocked`),
                // so each room's `.selfJoined` re-confirm appends seamlessly to
                // its existing transcript, and the room-token re-registration
                // on the fresh session (via `session.join`) re-maps back to the
                // same case-folded `roomKey`. Snapshot the display names on the
                // engine queue (here) and fire the joins fire-and-forget, same
                // shape as the single initial join below.
                let displayRooms = roomOrder.compactMap { rooms[$0]?.displayName }
                Task { [session] in
                    for room in displayRooms {
                        try? await session.join(room)
                    }
                }
            } else {
                // INITIAL login: join the one configured room.
                Task { [session, config] in
                    try? await session.join(config.room)
                }
            }

        case .selfJoined(let joinedChannel):
            // The server confirmed a join. Register/activate is already done
            // for the INITIAL room (`start()`) and for a `joinRoom` (below);
            // this just ensures the box exists and announces our avatar IN
            // THAT CHANNEL (clarification 1: the per-CRoomInfo announce goes
            // to the joined channel, not a hard-coded `config.room`).
            // `ensureRoomBoxLocked` also updates `displayName` to the
            // server's own casing here (live-fix 1) — this IS the join
            // confirm, the authoritative moment the server's casing is known.
            ensureRoomBoxLocked(joinedChannel)
            let joinedKey = roomKey(joinedChannel)
            let name = config.characterName.capitalized
            Task { [session] in
                try? await session.announceAvatar(channel: joinedChannel, name: name)
            }
            if joinedKey == activeRoom { recomposeLocked() }
            emitRooms()

        case .selfParted(let partedChannel):
            // A server-confirmed self-part. If `leaveRoom` already dropped the
            // box (the common case — a user-initiated leave), this is a no-op
            // (guarded by `isPartOfDepartedRoom` above, which routed the event
            // to the session transcript and never re-created the box). If the
            // room is STILL joined, the part was server-initiated (a forced
            // part / self-kick the app didn't drive) — drop it the same way
            // `leaveRoom` does: remove the box/tab, and if it was active,
            // fall back to another room or clear the strip.
            let partedKey = roomKey(partedChannel)
            if rooms[partedKey] != nil {
                let wasActive = (partedKey == activeRoom)
                rooms.removeValue(forKey: partedKey)
                roomOrder.removeAll { $0 == partedKey }
                if wasActive {
                    if let fallback = roomOrder.first {
                        rebuildStripLocked(for: fallback, resetAnnounce: false)
                        rooms[fallback]?.unread = 0
                        emitMembers(for: fallback)
                    } else {
                        strip?.close(); strip = nil; bridge = nil
                        selfParticipantID = nil; activeRoom = ""
                    }
                }
                emitRooms()
            }

        case .appearsAs(let nick, let avatarName, let url):
            // A character-appearance announce. Two wire shapes reach here
            // (final-review Important #1, `ircsock.cpp:923-928` — the engine
            // explicitly delegates this fan-out decision to Swift):
            //   - CHANNEL-scoped (clarification 4): the common case, a
            //     "# Appears as" riding a room PRIVMSG/DATA — `scopedRoom` is
            //     non-nil, it already lives in THAT room's transcript
            //     (appended by the routing block above) and drives the live
            //     strip only when that room is active. Left as-is here.
            //   - PRIVATE (token 0, `scopedRoom == nil`): the 1998 client's
            //     STANDARD response to our own channel-wide announce is a
            //     private "# Appears as" reply-announce, which arrives as a
            //     bare PRIVMSG to our own nick (see `AnnounceTests
            //     .privateReplyAnnounce`'s wire form / `handleLocked`'s
            //     transcript-routing doc comment on the token-registration
            //     race) — that has NO channel token, so the routing block
            //     above filed it under `sessionTranscript`, which is NEVER
            //     replayed by `rebuildStripLocked`. Pre-multi-room (single
            //     transcript) this same event applied unconditionally; the
            //     multi-room split silently dropped it. Fixed here: fan it
            //     out to EVERY joined room's transcript (so a later
            //     `setActiveRoom` rebuild of ANY room still re-avatars this
            //     peer) and apply it to the live strip regardless of which
            //     room is active (the peer's avatar update is not really
            //     "about" any one room — it has none).
            //   KNOWN LESSER VARIANT (recorded debt, not fixed here): a
            //   CHANNEL-scoped `.appearsAs` for a nick who is ALSO a member
            //   of some OTHER joined room only updates that other room's
            //   transcript once THAT room independently sees its own
            //   `.appearsAs`/roster refresh for the same nick — i.e. the
            //   avatar doesn't fan out across rooms just because the nick is
            //   present in more than one. This is the same root cause (a
            //   peer's avatar identity is session-wide, not room-scoped) but
            //   deliberately left alone: fixing it would mean cross-
            //   referencing room membership on every channel-scoped
            //   `.appearsAs`, a bigger change than this finding's minimal fix
            //   calls for.
            let isPrivateAnnounce = (scopedRoom == nil)
            if isPrivateAnnounce {
                appendToAllRoomTranscriptsLocked(ev)
            }
            // The reply-announce greets a PEER whose avatar we're seeing for
            // the first time; it must NEVER fire for our own synthetic
            // (`!fromServer`) or a server-echoed announce of our OWN nick.
            // `announcedBackTo` stays session-level (a nick is greeted once
            // across the whole session).
            let isOwnAnnounce = !fromServer || nick.caseInsensitiveCompare(currentOwnNick) == .orderedSame
            if !isOwnAnnounce, !announcedBackTo.contains(nick) {
                announcedBackTo.insert(nick)
                let name = config.characterName.capitalized
                // Live-fix 1: `activeRoom` is a case-folded LOOKUP KEY, not a
                // real channel string — sending it straight to the wire (the
                // pre-fix shape) would announce into a channel that may not
                // even exist in that exact casing. Resolve the active room's
                // DISPLAY name (the actual, server-known channel string)
                // instead; `scopedRoom` (the wire-cased channel this event
                // itself arrived on) is preferred when available.
                let announceChannel = scopedRoom ?? rooms[activeRoom]?.displayName ?? ""
                // Final-review minor: the resolved channel can be "" (no room
                // joined yet, or the last room just left) — a reply-announce
                // with an empty channel has nowhere sensible to go on the
                // wire, so skip it rather than sending a malformed announce.
                if !announceChannel.isEmpty {
                    Task { [session] in
                        try? await session.announceAvatar(channel: announceChannel, toNick: nick, name: name)
                    }
                }
            }
            if isActiveRoom || isPrivateAnnounce { applyToBridgeLocked(ev); recomposeLocked() }
            // Plan 4b Task 6 (D4 §4): a PEER's announce naming art we don't
            // have locally AND carrying a fetchable URL enters the
            // auto-download path. Fires once regardless of room — the in-flight
            // guard dedupes (clarification 4); the resolved avatar is picked up
            // by whichever room's transcript replay next resolves the name.
            if !isOwnAnnounce {
                downloadAvatarIfNeededLocked(nick: nick, avatarName: avatarName, url: url)
            }

        case .text(let nick, _, _, let text, _, _):
            // Plan 4b Task 4 fix (§8 Topology A / plain-IRC interop finding):
            // a private whisper on plain IRC arrives as a bare `PRIVMSG
            // <ourNick> :text` -- the engine classifies this `CC_EV_TEXT`
            // (never `CC_EV_WHISPER`; see `whisperBoxRoutingLocked`'s doc
            // comment for the verified ircsock.cpp citations), so it must
            // ALSO be routed to the whisper box, alongside strip rendering.
            // Whispers stay SESSION-level (clarification 2) — the whisper-box
            // routing runs regardless of which room is active. Detected via
            // `ProtocolEvent.isWhisperShapedText` (final-review Important
            // #4a: factored into a shared predicate so `AppState`'s text-view
            // filter uses the EXACT same rule rather than a re-derived copy).
            // Runs AFTER the own-echo dedup guard above (which already
            // `return`ed for a matching echo).
            let isWhisper = ProtocolEvent.isWhisperShapedText(ev, ownNick: currentOwnNick)
            if isWhisper {
                whisperBoxRoutingLocked(nick: nick, text: text, fromServer: fromServer)
            } else {
                // Mention notification (Batch B): only for a genuine channel
                // message (not a whisper-shaped one — that already fired the
                // `.whisper` notification kind above), from the server, and
                // not from an ignored nick.
                notifyIfMentionLocked(nick: nick, text: text, room: scopedRoom,
                                      fromServer: fromServer)
            }
            // Strip/unread routing: a plain-IRC whisper is session-scoped
            // (`channel == nil`), so it never bumps a room's unread or strip;
            // a genuine channel message drives the active strip or bumps the
            // background room's unread. An OWN message (`!fromServer` — the
            // synthetic self-say into a background room) renders but never
            // bumps unread (you don't have unread from yourself).
            //
            // Quick-wins batch item 5 (ignore, doctrine comment on
            // `ignoredNicks`): an ignored nick's `.text` still appended to the
            // transcript above (the log stays canonical) but is skipped HERE,
            // at render-routing — no strip panel, no unread bump. Never
            // applies to our OWN nick (`ignoredNicks` can't contain
            // `currentOwnNick` via the UI, but this guard costs nothing and
            // documents the invariant explicitly).
            if !ignoredNicks.contains(ignoreKey(nick)) {
                handleRoomMessageStripEffect(ev, isActiveRoom: isActiveRoom, room: scopedKey,
                                             isMessage: !isWhisper && fromServer)
            }

        case .action(let nick, let text, _):
            if !ignoredNicks.contains(ignoreKey(nick)) {
                handleRoomMessageStripEffect(ev, isActiveRoom: isActiveRoom, room: scopedKey,
                                             isMessage: fromServer)
                // Mention notification (Batch B): an ACTION line ("/me ...")
                // can carry our nick too — same detection, same guards.
                notifyIfMentionLocked(nick: nick, text: text, room: scopedRoom,
                                      fromServer: fromServer)
            }

        case .whisper(let nick, _, let text, _):
            // Plan 4b Task 4: the IRCX WHISPER verb path -- see
            // `whisperBoxRoutingLocked`'s doc comment for both wire forms.
            // Whisper-box routing is session-level (clarification 2).
            //
            // FINAL-REVIEW COORDINATOR RULING (Important #4b, binding):
            // session-scoped (PRIVATE) whispers render in the whisper box
            // ONLY -- the strip `bridge.apply` for a session-scoped whisper
            // (`channel == nil`) is REMOVED. Pre-this-fix, a session-scoped
            // whisper balloon rendered on the strip ONCE (via this branch's
            // old `channel == nil` clause) and then vanished the next time
            // ANY reflow replayed that room's transcript -- a session-scoped
            // event is never IN a room's transcript (it lives in
            // `sessionTranscript`, `handleLocked`'s routing block above), so
            // `rebuildStripLocked`'s replay simply never re-applies it. That
            // is worse than either fully-consistent behavior (balloon
            // forever vs. never) and contradicts the original, which renders
            // private whispers box-only (never on the main strip at all --
            // `whisprbx.cpp`'s dedicated whisper-window rendering path, no
            // `CUnitPanel`/strip involvement). ROOM-scoped WHISPER events
            // (an IRCX `WHISPER <chan> ...` naming a real channel) are
            // UNCHANGED: they keep rendering their strip balloon when that
            // room is active, exactly like before -- `isActiveRoom` already
            // requires `scopedRoom != nil`, so this reduces to "only apply
            // when room-scoped and active," dropping the old `|| channel ==
            // nil` fallback entirely. A whisper does NOT bump unread either
            // way.
            whisperBoxRoutingLocked(nick: nick, text: text, fromServer: fromServer)
            // Quick-wins batch item 5 (ignore): the whisper BOX still shows
            // the line unconditionally (`whisperBoxRoutingLocked` above,
            // unaffected — ignore is a STRIP-rendering filter, not a
            // whisper-box one; a whisper box is an explicit private
            // conversation the user opened, distinct from the ambient strip
            // an ignore is meant to declutter). Only the room-scoped strip
            // balloon is skipped for an ignored nick.
            if isActiveRoom, !ignoredNicks.contains(ignoreKey(nick)) {
                applyToBridgeLocked(ev); recomposeLocked()
            }

        case .userJoined:
            // A peer joining is strip-relevant (renders a JOIN panel) AND
            // membership-relevant, both scoped to the event's room. Live only
            // when that room is active; a background room defers rendering to
            // its next activation (transcript already appended). A join does
            // NOT bump unread (clarification 3).
            if isActiveRoom { applyToBridgeLocked(ev); recomposeLocked() }
            if let key = scopedKey { emitMembers(for: key) }

        case .nickChanged(_, let newNick, let isSelf):
            if isSelf {
                currentOwnNick = newNick   // echo-only rule confirmation, mirrors ProtocolSession's own _ownNick update
            }
            // A nick change is server-wide (session-scoped) — refresh the
            // active room's member list (the sidebar shows the active room).
            emitMembers(for: activeRoom)

        case .userParted, .kicked, .names, .endOfNames:
            emitMembers(for: scopedKey ?? activeRoom)

        case .userQuit:
            // Server-wide: the peer left every room at once — refresh the
            // active room's sidebar.
            emitMembers(for: activeRoom)

        // Quick-wins batch item 6: track the room's topic model-side (the
        // simplest path per the brief — reading `session.room(_:).topic`
        // on-queue would trap, same hazard `currentOwnNick`'s doc comment
        // documents for `session.ownNick`). The transcript append above
        // already recorded this event; this just mirrors it into the
        // `RoomBox` for `roomInfosLocked()`'s snapshot. `scopedKey` is nil
        // only if the token-resolution race left this session-scoped (should
        // not happen for a real topic reply — defensive no-op either way).
        case .topicChanged(_, let topic):
            if let key = scopedKey {
                rooms[key]?.topic = topic
                emitRooms()
            }

        case .statusLine(let text):
            emitStatus(text)
            emitServerMessage(text)
        case .error(let code, let text):
            emitStatus("error \(code): \(text)")
            emitServerMessage("error \(code): \(text)")
            // Final-review minor: if the server ERRORS a LIST request instead
            // of ever sending `.roomListBegin`/`.roomListEnd`,
            // `roomListRequestInFlight` (set true the moment the wire LIST
            // went out, `requestRoomList()`'s own doc comment) would
            // otherwise never clear — permanently wedging every subsequent
            // `requestRoomList()` call into a silent no-op for the rest of
            // the session. Clearing it here unconditionally on ANY `.error`
            // is a coarser trigger than "only a LIST-caused error" (this
            // event carries no correlation back to which request caused it),
            // but a spurious clear is harmless (worst case: an unrelated
            // error lets a still-in-flight LIST send an early second wire
            // LIST, the same class of redundant-request the in-flight guard
            // exists to avoid, not a correctness break) whereas never
            // clearing is a permanent wedge — the asymmetry favors clearing.
            roomListRequestInFlight = false
        case .disconnectedHint(let text):
            emitStatus(text)
            emitServerMessage(text)
            // Auto-reconnect (spec §7): an UNEXPECTED drop (user-initiated
            // disconnect never reaches here — `ProtocolSession.disconnect()`
            // sets a flag that suppresses this event) kicks off (or feeds) the
            // backoff-reconnect loop. `maybeStartReconnectLocked` no-ops when
            // reconnect is disabled/shutting down, and coalesces a second
            // `.disconnectedHint` from a failed reconnect attempt into the
            // already-running loop rather than starting a rival one.
            maybeStartReconnectLocked()

        // Live-fix 4: the original client showed MOTD/LUSER text in its
        // status window — this port never surfaced `.motd` anywhere
        // (`default: break`, pre-fix). `luser`/`motd` are two SEPARATE
        // accumulated text blocks off the wire (see `ProtocolEvent.motd`'s
        // originating LUSERS/MOTD reply sequence, `ircsock.cpp`); join
        // whichever are non-empty into one console line rather than firing
        // twice for what's conceptually one server greeting.
        case .motd(let luser, let motd):
            let parts = [luser, motd].filter { !$0.isEmpty }
            guard !parts.isEmpty else { break }
            emitServerMessage(parts.joined(separator: "\n"))

        // Live-fix 3 (crypthome.com, live-reproduced: `432 Anonymous
        // :Reserved name` dead-ended the session with only a transient
        // status line — nothing routed the app to a clear next step). Fires
        // BOTH the existing status-line surface (`emitStatus`/
        // `emitServerMessage`, matching every other error-shaped event above)
        // AND the dedicated `onNickRejected` callback the app layer uses to
        // alert + reopen the connect sheet — `handleLocked` itself does NOT
        // disconnect; the session is left exactly as the engine/server left
        // it (typically already unusable — the app layer's job is deciding
        // what "surface it" means for the user, mirroring every other
        // session-scoped notice on this type).
        case .nickRejected(_, let badNick):
            let text = "Nickname '\(badNick)' was rejected by the server (reserved/in use). Choose another."
            emitStatus(text)
            emitServerMessage(text)
            let cb = onNickRejected
            DispatchQueue.main.async { cb?(badNick, text) }

        // MARK: Room list (Plan 4b Task 8) — accumulate begin -> items -> end,
        // fire `onRoomList` once on end. Session-scoped (LIST is not
        // per-channel), so these fall to `sessionTranscript` above like every
        // other `channel == nil` event; no strip effect.
        case .roomListBegin:
            roomListAccum = []
        case .roomListItem(let name, let users, let topic):
            roomListAccum?.append(RoomListItem(name: name, users: users, topic: topic))
        case .roomListEnd:
            let items = roomListAccum ?? []
            roomListAccum = nil
            roomListRequestInFlight = false
            let cb = onRoomList
            DispatchQueue.main.async { cb?(items) }

        // MARK: WHO / Get Info (Plan 4b Task 8) — `getInfo(_:)` is backed by
        // `who(_:)` + this event (there is no `cc_session_whois` builder in
        // the outbound surface; comicchat.h:600 has `who` only).
        case .whoResult(let nick, let user, let host, let whoChannel, _):
            let formatted = "\(nick) (\(user)@\(host)) — \(whoChannel)"
            let cb = onUserInfo
            DispatchQueue.main.async { cb?(nick, formatted) }

        // MARK: Sounds (Plan 4b Task 9) — the transcript append above already
        // happened (channel-scoped or session-scoped, following the wire's
        // room token like any other event; see `sessionTranscript`'s doc
        // comment). Playback itself is session-level regardless: fire
        // `onSound` unconditionally here. No unread bump (a sound is not a
        // message the user "missed" the way a channel text/action is), and no
        // `bridge.apply` (sounds render no strip panel — audio-only).
        case .sound(let nick, let file, _):
            // Quick-wins batch item 5 (ignore): a `.sound` renders no strip
            // panel at all (audio-only — no `bridge.apply` above either), so
            // its only "render" effect IS this playback callback; skipping it
            // for an ignored nick is this event kind's equivalent of the
            // `.text`/`.action`/`.whisper` strip-routing guards above. The
            // transcript append already happened unconditionally (the log
            // stays canonical, matching the ignore doctrine).
            guard !ignoredNicks.contains(ignoreKey(nick)) else { break }
            let cb = onSound
            DispatchQueue.main.async { cb?(nick, file) }

        // Away-return (Batch B READ-FIRST finding): `ProtocolSession`'s own
        // `.awayPeer` handler already updates `RoomMember.isAway` for every
        // room the nick is a member of (both directions, since that fix —
        // see `ProtocolSession.handleEvent`'s `.awayPeer` case doc comment
        // for the wire-grammar citation), but NOTHING previously re-derived
        // the member list afterward — this case was missing entirely
        // (falling through to `default: break`), so `onMembers` never
        // re-fired and `MemberRow.isAway` stayed stuck at whatever it was
        // when the sidebar was last built. `.awayPeer` carries no channel
        // token of its own (it rides a plain PRIVMSG, room-scoped or not
        // depending on target) — refresh the ACTIVE room's sidebar, same
        // "session-wide peer state change -> refresh what's visible" posture
        // `.nickChanged`/`.userQuit` already use above for other session-wide
        // peer facts.
        case .awayPeer:
            emitMembers(for: activeRoom)

        // Plan 4b Batch C: answer the two peer probes Tim opted back into
        // (VERSION/GetInfo — see `comicchat.h`'s `CC_EV_VERSION_REQUEST`/
        // `CC_EV_INFO_REQUEST` doc comments for the un-suppression
        // citation). Both follow the SAME shape as `.appearsAs`'s own
        // reply-announce just above: resolve a channel to send the private
        // reply through (`ccSessionSelectRoom`, which every outbound builder
        // needs a room_token for, requires SOME registered room even for a
        // reply that is itself a private PRIVMSG/NOTICE to one nick — there
        // is no session-wide "no room" send path), fire-and-forget via a
        // detached `Task`, and rate-limit per nick (see `lastProbeReplyAt`'s
        // own doc comment for why one shared dict covers both cases).
        case .versionRequest(let fromNick):
            replyToProbeIfDueLocked(fromNick, scopedRoom: scopedRoom) { channel in
                Task { [session] in
                    try? await session.sendVersionReply(channel: channel, toNick: fromNick,
                                                        versionText: Self.versionReplyText)
                }
                self.emitStatus("Sent version info to \(fromNick)")
            }

        case .infoRequest(let fromNick):
            replyToProbeIfDueLocked(fromNick, scopedRoom: scopedRoom) { channel in
                let profile = self.config.profileText
                Task { [session] in
                    try? await session.sendInfoReply(channel: channel, toNick: fromNick, profileText: profile)
                }
                self.emitStatus("Sent profile info to \(fromNick)")
            }

        default:
            break
        }
    }

    /// ENGINE QUEUE ONLY (Plan 4b Batch C). Shared plumbing for
    /// `.versionRequest`/`.infoRequest`: resolves a channel to reply through
    /// (`scopedRoom` if this probe arrived room-scoped, else the active
    /// room's display name — same fallback `.appearsAs`'s reply-announce
    /// uses just above) and enforces the per-nick 10s cooldown
    /// (`lastProbeReplyAt`). Skips silently (no reply, no status line) if
    /// there's no channel to send through OR the nick replied to within the
    /// last `probeReplyCooldown` seconds — a deliberate modern anti-spam
    /// guard the original never had (see `lastProbeReplyAt`'s doc comment).
    private func replyToProbeIfDueLocked(_ fromNick: String, scopedRoom: String? = nil,
                                         _ send: (String) -> Void) {
        let now = Date()
        if let last = lastProbeReplyAt[fromNick], now.timeIntervalSince(last) < Self.probeReplyCooldown {
            return
        }
        let channel = scopedRoom ?? rooms[activeRoom]?.displayName ?? ""
        guard !channel.isEmpty else { return }
        lastProbeReplyAt[fromNick] = now
        send(channel)
    }

    /// ENGINE QUEUE ONLY. A channel-scoped strip message (`.text`/`.action`):
    /// if it's the active room, apply it to the live strip and recompose; if a
    /// background room, bump that room's unread (only when `isMessage`) and
    /// refresh the tab bar. The transcript append already happened in
    /// `handleLocked`. `room == nil` (session-scoped, e.g. a plain-IRC
    /// whisper) does nothing here — it never touches the strip or unread.
    private func handleRoomMessageStripEffect(_ ev: ProtocolEvent, isActiveRoom: Bool, room: String?, isMessage: Bool) {
        if isActiveRoom {
            applyToBridgeLocked(ev)
            recomposeLocked()
        } else if let room, rooms[room] != nil {
            if isMessage {
                rooms[room]!.unread += 1
                emitRooms()
            }
        }
    }

    /// ENGINE QUEUE ONLY (final-review Important #1). Appends `ev` to EVERY
    /// currently-joined room's transcript — used for a PRIVATE `.appearsAs`
    /// reply-announce (token 0, `scopedRoom == nil`), which the routing block
    /// in `handleLocked` files under `sessionTranscript` (never replayed by
    /// `rebuildStripLocked`). Without this fan-out, a peer's avatar update
    /// delivered this way survives only until the next `setActiveRoom`/
    /// `setViewport` reflow of whichever room happened to be active when it
    /// arrived, then is silently lost on every other room (and on that same
    /// room too, once its transcript is replayed from scratch and this event
    /// isn't in it). Mirrors `changeCharacter`'s own "record the switch in
    /// every room's transcript" posture (Plan 4b Task 7 clarification 1) —
    /// same shape, opposite direction (a PEER's avatar rather than our own).
    private func appendToAllRoomTranscriptsLocked(_ ev: ProtocolEvent) {
        for room in roomOrder {
            rooms[room]?.transcript.append(ev)
        }
    }

    /// ENGINE QUEUE ONLY. Ensures a `RoomBox` and its tab-order slot exist for
    /// `room` (idempotent, keyed via `roomKey(room)` — live-fix 1). Used
    /// defensively wherever an event might reference a room the normal join
    /// flow hasn't registered yet.
    ///
    /// Live-fix 1: `room` is the DISPLAY-cased channel string as it arrived
    /// (from the server, from a local join request, ...). If a box already
    /// exists under this key (e.g. seeded locally by `start()`/`joinRoom`
    /// with our own typed casing), this ALSO updates its `displayName` to
    /// `room` — the freshest-seen casing wins, which in practice means the
    /// server's own `.selfJoined` confirm (the authoritative source) always
    /// ends up overwriting whatever casing we originally typed, matching the
    /// original 1998 client's own display convention.
    ///
    /// Final-review Important #2: this is the SINGLE chokepoint every room
    /// box is actually created through after `start()`'s own initial-room
    /// seeding (`joinRoom`'s eager registration, `.selfJoined`'s confirm, and
    /// `handleLocked`'s defensive token-race path all call this) — so it is
    /// also the right place to seed a JOIN-AFTER-CHARACTER-SWITCH fix:
    /// `changeCharacter` only appends its synthetic `.appearsAs` to rooms that
    /// already exist at switch time (`self.roomOrder` at that moment) — a room
    /// joined LATER has no switch recorded in its transcript at all, so its
    /// first-ever rebuild (`rebuildStripLocked`, e.g. the very first
    /// `setActiveRoom` into it) seeds the self participant from
    /// `initialCharacterName` (`setUpStripLocked`'s `isReflow` seeding) and
    /// then replays a transcript with NO switch entry to move it forward —
    /// self renders as the character we STARTED the session with, not the one
    /// we're actually currently wearing. Seeding the brand-new box's
    /// transcript with the same synthetic `.appearsAs` shape `changeCharacter`
    /// uses, BEFORE any real event lands in it, closes the gap: the room's
    /// very first replay already carries the switch at position 0.
    /// `config.characterName` is read here (not `initialCharacterName`) since
    /// this only needs to fire when they've actually diverged (a switch
    /// happened at some point before this room existed); if they're still
    /// equal, seeding would be a no-op anyway (replaying the same avatar the
    /// fresh self-participant is already seeded with), so the guard is a
    /// direct translation of "only seed when there's something to seed."
    private func ensureRoomBoxLocked(_ room: String) {
        let key = roomKey(room)
        if rooms[key] == nil {
            var box = RoomBox(displayName: room)
            if config.characterName != initialCharacterName {
                box.transcript.append(.appearsAs(nick: currentOwnNick,
                                                  avatarName: config.characterName.capitalized, url: ""))
            }
            rooms[key] = box
            roomOrder.append(key)
        } else {
            rooms[key]!.displayName = room
        }
    }

    /// The channel an event carries in its OWN payload, for the join-lifecycle
    /// events where that channel IS definitionally the room and is needed as a
    /// fallback when the token-resolved channel is momentarily nil (the
    /// token-registration race — see `handleLocked`'s `scopedRoom`). Only
    /// `.selfJoined`/`.selfParted` qualify: their payload channel is
    /// unambiguously a room. Message events (`.text` target may be a nick;
    /// `.userJoined` carries no channel) are NOT included — those rely on the
    /// authoritative token-resolved channel.
    private func intrinsicChannel(of ev: ProtocolEvent) -> String? {
        switch ev {
        case .selfJoined(let channel): return channel
        case .selfParted(let channel): return channel
        default: return nil
        }
    }

    /// ENGINE QUEUE ONLY. Routes one inbound whisper line to the tabbed
    /// whisper box: appends to `_whisperHistories[nick]` and fires `onWhisper`
    /// (unless `_acceptWhispers == false`, in which case the line is dropped
    /// from history entirely and surfaces only via `onStatus` instead).
    /// Shared by BOTH wire forms an inbound whisper can arrive as
    /// (`handleLocked`'s own doc comment table has the per-event-case
    /// routing):
    ///   - the IRCX `WHISPER <chan> <targetlist> :<text>` verb, which the
    ///     engine classifies `CC_EV_WHISPER` -> `.whisper` (ircsock.cpp's
    ///     `cmdidWhisper` handler, :1135-1176);
    ///   - a plain-IRC `PRIVMSG <ourNick> :<text>` (no channel prefix), which
    ///     the engine classifies `CC_EV_TEXT` -> `.text` -- `cmdidPrivMsg`
    ///     (ircsock.cpp:907-954) never sets `MT_WHISPER` on the PRIVMSG path,
    ///     so this is the wire form §8 Topology A's plain-IRC-only servers
    ///     actually produce, and the ONLY reason this method takes the
    ///     already-destructured `nick`/`text` rather than a `ProtocolEvent`
    ///     itself (the two cases carry different case shapes).
    ///
    /// - Parameter fromServer: Batch B — passed straight through from
    ///   `handleLocked`'s own parameter (both call sites are reached from
    ///   `handleLocked`'s `.text`/`.whisper` cases, which run for both real
    ///   wire events and synthetic ones); gates the `onNotificationEvent`
    ///   fire below so a hypothetical future synthetic whisper-shaped event
    ///   never buzzes a notification for something we said ourselves.
    private func whisperBoxRoutingLocked(nick: String, text: String, fromServer: Bool = true) {
        if _acceptWhispers {
            let line = WhisperLine(nick: nick, text: text, isOwn: false)
            _whisperHistories[nick, default: []].append(line)
            let cb = onWhisper
            DispatchQueue.main.async { cb?(nick, line) }
            // Notification (Batch B): a whisper is always notification-worthy
            // (it's addressed to us specifically) — same fromServer/ignore
            // guards as the mention path (`notifyIfMentionLocked`'s doc
            // comment), applied inline here since a whisper has no separate
            // "room" concept to check a mention against.
            if fromServer, !ignoredNicks.contains(ignoreKey(nick)) {
                let ncb = onNotificationEvent
                let event = NotificationEvent(kind: .whisper, fromNick: nick, text: text)
                DispatchQueue.main.async { ncb?(event) }
            }
        } else {
            emitStatus("Whisper from \(nick) blocked (whispers disabled)")
        }
    }

    /// ENGINE QUEUE ONLY (Batch B). Fires `onNotificationEvent` with
    /// `.mention(room:)` when `text` contains our own current nick,
    /// case-folded — a simple `contains`, NOT a true whole-word match (a
    /// documented deviation: a nick like "Tim" also matches inside "Timothy";
    /// acceptable per the brief's "whole-word-ish... simple case-folded
    /// contains is acceptable" allowance). Guarded the same way every other
    /// notification-worthy path on this type is: only for `fromServer` events
    /// (never our own synthetic self-say/echo) and never for an ignored nick
    /// (mirrors `.text`/`.action`'s own `ignoredNicks` guard in `handleLocked`,
    /// though callers already skip calling this for an ignored nick too — the
    /// check is repeated here so this method is safe to call unconditionally
    /// from any future call site). Never fires for our OWN nick speaking (a
    /// self-say containing our own nick is not a "mention" in any useful
    /// sense, and `fromServer` alone doesn't rule that out for a server that
    /// echoes PRIVMSGs back — the own-echo dedup upstream already drops the
    /// echo before this is ever reached, but the nick-equality check is kept
    /// as a defensive belt-and-suspenders guard).
    private func notifyIfMentionLocked(nick: String, text: String, room: String?, fromServer: Bool) {
        guard fromServer, !ignoredNicks.contains(ignoreKey(nick)) else { return }
        guard nick.caseInsensitiveCompare(currentOwnNick) != .orderedSame else { return }
        guard !currentOwnNick.isEmpty,
              text.range(of: currentOwnNick, options: [.caseInsensitive]) != nil else { return }
        let roomName = room.map { rooms[roomKey($0)]?.displayName ?? $0 } ?? ""
        let cb = onNotificationEvent
        let event = NotificationEvent(kind: .mention(room: roomName), fromNick: nick, text: text)
        DispatchQueue.main.async { cb?(event) }
    }

    /// ENGINE QUEUE ONLY. Plan 4b Task 6 (D4 §4): kicks off a peer avatar
    /// auto-download when ALL of these hold:
    ///   - `config.autoDownloadAvatars` is on;
    ///   - `avatarName` did not resolve to real local art (`bridge.resolvesName`
    ///     — "did this name resolve to real art vs a cycled default", NOT
    ///     just "is the name non-empty": a name that already resolves has
    ///     nothing to fetch, whether that's because it's genuinely known art
    ///     or a bare name `resolve(avatarName:)` would cycle a default for
    ///     either way, downloading would be pointless or wrong);
    ///   - `url` is a well-formed http/https URL (the wire carries `"?"` for
    ///     a DEFERRED url and `""` for none — see `URL`'s parse below; a
    ///     scheme check on TOP of parse success is required because
    ///     `URL(string:)` happily parses `"?"` as a query-only relative
    ///     reference with no scheme).
    ///
    /// Fetch runs in a DETACHED `Task`, deliberately OFF the engine queue
    /// (`URLSession` is async; the brief's threading note: "never block the
    /// engine queue on network"). On success, the apply hops BACK onto the
    /// engine queue (`engineQueue.async`) to re-avatar the nick's existing
    /// participant — `bridge.participantIDs[nick]` -> `strip.setParticipantAvatar`
    /// -> `recomposeLocked()` — guarded by `isShutDown` and the participant
    /// still existing (a peer may have parted while the download was in
    /// flight). Failure is a SILENT status-line note, matching the original's
    /// auto (non-interactive) download path (chat.cpp:2242-2244 — the
    /// interactive path pops a dialog via `AvatarTransferError`, but the
    /// auto-on-appearsAs path this task mirrors never does).
    private func downloadAvatarIfNeededLocked(nick: String, avatarName: String, url: String) {
        guard config.autoDownloadAvatars else { return }
        guard let bridge, !bridge.resolvesName(avatarName) else { return }
        guard let parsed = URL(string: url),
              let scheme = parsed.scheme?.lowercased(), scheme == "http" || scheme == "https" else {
            return
        }
        // In-flight guard (fix round 1): a second `.appearsAs` for the same
        // NAME arriving while the first download is still in flight bails
        // out here instead of spawning a redundant fetch of the same URL.
        guard !inFlightAvatarDownloads.contains(avatarName) else { return }
        inFlightAvatarDownloads.insert(avatarName)

        let downloader = AvatarDownloader()
        let dir = URL(fileURLWithPath: Self.userCharactersDir)
        Task.detached { [weak self] in
            defer {
                self?.engineQueue.async { [weak self] in
                    guard let self, !self.isShutDown else { return }
                    self.inFlightAvatarDownloads.remove(avatarName)
                }
            }
            guard let self else { return }
            do {
                let downloadedPath = try await downloader.fetch(name: avatarName, url: parsed, into: dir)
                self.engineQueue.async { [weak self] in
                    guard let self, !self.isShutDown else { return }
                    guard let bridge = self.bridge, let strip = self.strip,
                          let id = bridge.participantIDs[nick] else { return }
                    // 4a-carryover comment (per task brief): the engine's
                    // `s->avatars` vector accumulates old+new `CAvatarX*`
                    // per participant on switch (avatario.cpp/panel.cpp's
                    // ChangeAvatar path) -- bookkeeping-only today (no
                    // per-session cap on switches), a guard owed if avatar
                    // switches become frequent (e.g. repeated re-downloads
                    // of the same nick's avatar across a long session).
                    guard (try? strip.setParticipantAvatar(id, avbPath: downloadedPath.path)) != nil else { return }
                    self.recomposeLocked()
                }
            } catch {
                self.emitStatus("Avatar download for \(nick) failed: \(error)")
            }
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
        // Plan 4b Task 7: cache the last composed image so `setActiveRoom` can
        // stash it into the outgoing room's box (an instant switch-back).
        lastComposedImage = image
        lastComposedSizePoints = sizePoints
        DispatchQueue.main.async { [onStripImage] in
            onStripImage?(image, sizePoints)
        }
    }

    /// The image/size the LAST `recomposeLocked` produced for the CURRENT
    /// active strip (Plan 4b Task 7). Stashed into the outgoing room's box on a
    /// `setActiveRoom` swap. Engine-queue-owned.
    private var lastComposedImage: CGImage?
    private var lastComposedSizePoints: CGSize = .zero

    /// Reads `room`'s member list and forwards it to `onMembers`. Called from
    /// `handleLocked` (running ON the engine queue), but dispatched via a
    /// detached `Task` rather than reading `session.room(_:)` directly: that
    /// accessor does its own `sessionQueue.sync` internally (`ProtocolSession`'s
    /// public read API), and `sessionQueue` IS this model's `engineQueue`
    /// (shared by injection, `ProtocolSession`'s own doc comment) — calling it
    /// synchronously from a closure ALREADY executing on that same serial queue
    /// is a same-queue reentrant `sync`, which traps (SIGTRAP, observed) rather
    /// than merely deadlocking. The `Task` runs on its own (cooperative-pool)
    /// context, genuinely off the engine queue, so `session.room(_:)`'s
    /// internal `sync` is safe there.
    ///
    /// Plan 4b Task 7: `room` is the event's own channel — the sidebar shows
    /// whichever room is active, so a membership event for a BACKGROUND room
    /// still refreshes ITS `session.room(room)` snapshot but only the active
    /// room's snapshot actually shows (the app re-reads on `setActiveRoom`).
    /// Emitting for the event's room keeps `ProtocolSession`'s own per-room
    /// member table the single source of truth and lets a caller filter.
    ///
    /// Plan 4b Task 8: the snapshot now builds `[MemberRow]` (nick/isOp/
    /// avatarName) rather than a bare sorted `[String]` — same snapshot, same
    /// seq-guard ordering protection, richer row. Also derives OUR OWN op
    /// status from the same snapshot and fires it via `onSelfOp` (the
    /// brief's suggested extension point — "simplest: extend `emitMembers`
    /// to also push `selfIsOp` through a new `onSelfOp`"). `ownNick` is
    /// captured here (engine-queue-local `currentOwnNick`, NOT
    /// `session.ownNick` — that accessor's own `sessionQueue.sync` would be a
    /// same-queue reentrant call from here, the exact hazard this property's
    /// own doc comment documents) so the detached `Task` doesn't need to
    /// touch engine-queue state itself.
    private func emitMembers(for room: String) {
        membersSeq += 1                              // engine queue — serialized
        let seq = membersSeq
        let activeAtEmit = activeRoom
        let ownNick = currentOwnNick
        // Live-fix 1: `room` is a case-folded LOOKUP KEY (every call site now
        // passes `scopedKey`/`activeRoom`, not the raw wire-cased channel) —
        // but `ProtocolSession.room(_:)` is keyed by whatever DISPLAY string
        // `session.join(...)` was actually called with (`ProtocolSession`'s
        // own `rooms` dict has no folding of its own). Resolve the display
        // name here, on the engine queue, before handing it to the detached
        // `Task` below (which must not touch `self`'s engine-queue state).
        let displayRoom = rooms[room]?.displayName ?? room
        // Live-fix (Tim's screenshot report): gray placeholder rows because
        // `member.avatarName` (`RoomMember`, `ProtocolSession`'s room-scoped
        // state) reads empty in two known cases: (1) OUR OWN row, always —
        // our announce is outbound-only (`session.announceAvatar`), and
        // nothing server-side ever echoes it back into `RoomState`, so
        // `ProtocolSession`'s `.appearsAs` case (which only fires from an
        // INBOUND event) never sees our own nick to update; (2) a PEER whose
        // `.appearsAs` reply-announce arrives before their room membership is
        // otherwise established (`ProtocolSession`'s `.appearsAs` handler
        // updates `rooms[key].members[nick].avatarName` for every room where
        // `nick` is ALREADY a member — it does not itself gate on channel vs.
        // private scope — but a member entry has to exist first; ordering
        // races or a not-yet-membered nick leave it empty). Snapshotting
        // `bridge?.announcedAvatarNames` (peers, nick-keyed — the SAME map
        // `.appearsAs` writes into via `ProtocolStripBridge.apply`,
        // regardless of scope, so it's populated even when `RoomState` isn't
        // yet) and `config.characterName` (self) HERE, ON THE ENGINE QUEUE,
        // lets the row-building below fill an empty `avatarName` without the
        // detached `Task` ever touching engine-queue state itself (the same
        // hazard this method's own doc comment already documents for
        // `session`). RoomState's own value wins when non-empty — it's the
        // room-scoped, authoritative source whenever it IS populated.
        let announcedAvatarNames = bridge?.announcedAvatarNames ?? [:]
        let ownCharacterName = config.characterName
        // Quick-wins batch item 5: snapshotted here, on the engine queue
        // (same posture as `announcedAvatarNames`/`ownCharacterName` above —
        // the detached `Task` below must not touch engine-queue state
        // itself), so `MemberRow.isIgnored` can be derived without a second
        // `engineQueue.sync` round-trip from inside that `Task`.
        let ignoredSnapshot = ignoredNicks
        let foldIgnore = { (nick: String) in ignoredSnapshot.contains(nick.lowercased()) }
        Task { [session, onMembers, onSelfOp] in
            // Only the ACTIVE room's membership drives the one sidebar — a
            // background room's churn updates `session.room(room)` (read on
            // its next activation) but must not overwrite the visible list.
            guard room == activeAtEmit else { return }
            let members = session.room(displayRoom)?.members ?? [:]
            let present = members.values.filter { !$0.departed }.sorted { $0.nick < $1.nick }
            let rows = present.map { member -> MemberRow in
                var avatarName = member.avatarName
                if avatarName.isEmpty {
                    if member.nick.caseInsensitiveCompare(ownNick) == .orderedSame {
                        avatarName = ownCharacterName
                    } else {
                        avatarName = announcedAvatarNames[member.nick] ?? ""
                    }
                }
                return MemberRow(nick: member.nick, isOp: member.isOp, avatarName: avatarName,
                                 isAway: member.isAway, isIgnored: foldIgnore(member.nick))
            }
            let selfIsOp = members[ownNick]?.isOp == true
            DispatchQueue.main.async {
                guard seq > self.appliedMembersSeq else { return }   // stale snapshot — drop
                self.appliedMembersSeq = seq
                onMembers?(rows)
                onSelfOp?(selfIsOp)
            }
        }
    }

    /// ENGINE QUEUE ONLY. Builds the current `[RoomInfo]` (tab order, unread,
    /// active flag). Reads `roomOrder`/`rooms`/`activeRoom` — all engine-queue
    /// state. Live-fix 1: `roomOrder`'s entries are case-folded KEYS; `name`
    /// resolves each box's `displayName` (the server-cased/user-facing form)
    /// so the rooms sidebar/every other UI consumer of `RoomInfo.name` shows the
    /// server's own casing, never the lowercased lookup key.
    private func roomInfosLocked() -> [RoomInfo] {
        roomOrder.compactMap { key in
            guard let box = rooms[key] else { return nil }
            return RoomInfo(name: box.displayName, unread: box.unread, isActive: key == activeRoom,
                            topic: box.topic)
        }
    }

    /// ENGINE QUEUE ONLY. Fires `onRoomsChanged` with the current tab-bar
    /// snapshot on the main thread (Plan 4b Task 7).
    private func emitRooms() {
        let infos = roomInfosLocked()
        DispatchQueue.main.async { [onRoomsChanged] in
            onRoomsChanged?(infos)
        }
    }

    private func emitStatus(_ text: String) {
        DispatchQueue.main.async { [onStatus] in
            onStatus?(text)
        }
    }

    /// Live-fix 4: fires `onServerMessage` on the MAIN thread with one
    /// already-formatted console line. Separate from `emitStatus` — `onStatus`
    /// only ever shows the LATEST line (the app's `statusLine` is a single
    /// `String`, overwritten each call), which is exactly the "error-notes
    /// clobbered by the next status line" bug this fix addresses; the server
    /// console instead ACCUMULATES every line via this callback.
    private func emitServerMessage(_ text: String) {
        DispatchQueue.main.async { [onServerMessage] in
            onServerMessage?(text)
        }
    }

    /// ENGINE QUEUE ONLY. Applies `ev` to the live strip `bridge`, surfacing a
    /// failure via `onStatus` instead of swallowing it silently. Every strip
    /// `apply` in `handleLocked`/`rebuildStripLocked` was previously a bare
    /// `try? bridge?.apply(ev)` — a throw (e.g. an avatar that fails to load)
    /// vanished with no trace, so a whole room's strip could freeze invisibly
    /// (the Plan 4b live break: an unresolvable announced avatar threw on every
    /// event needing that peer as a participant, and nothing said so). The
    /// engine's own single-line diagnostics stay swallowed by design; this only
    /// adds ONE status line at the failure site so the NEXT such break is
    /// visible rather than silent. Error handling is otherwise unchanged: the
    /// throw is still absorbed (a failed line must not abort the surrounding
    /// event loop), and `nil` bridge is still a no-op.
    private func applyToBridgeLocked(_ ev: ProtocolEvent) {
        guard let bridge else { return }
        do {
            try bridge.apply(ev)
        } catch {
            emitStatus("Strip render skipped an event: \(error)")
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

    // MARK: - comic hit-testing (Plan 4b): click-to-talk-to + balloon tooltips

    /// Reverse-map a strip PARTICIPANT id (as returned by
    /// `Strip.hitTestAvatar` / `cc_strip_hit_test_avatar`) to the nick the
    /// bridge assigned it. Engine-queue-owned read of the bridge's
    /// `participantIDs` map (nick -> id), reversed. Returns `nil` for an
    /// unknown id (including 0) or before a strip/bridge exists. ENGINE QUEUE
    /// ONLY — call from within an `engineQueue` block (e.g. `hitTestNick`
    /// below), never from the main thread directly (`participantIDs` is
    /// engine-queue-owned like every other bridge/strip touch on this type).
    private func participantNickLocked(for id: Int32) -> String? {
        guard id > 0, let bridge else { return nil }
        for (nick, pid) in bridge.participantIDs where pid == id {
            return nick
        }
        return nil
    }

    /// Thread-safe reverse-map of a strip PARTICIPANT id to its nick (the
    /// bridge's `participantIDs` map reversed) — `nil` for an unknown id (incl.
    /// 0) or before a strip/bridge exists. Same `engineQueue.sync` read-through
    /// posture as `transcript`/`currentRoom`; exposed for the app/tests (the
    /// on-queue callers use `participantNickLocked` directly to avoid a
    /// same-queue reentrant `sync`).
    public func participantNick(for id: Int32) -> String? {
        engineQueue.sync { participantNickLocked(for: id) }
    }

    /// Hit-test a page-twips point against the composed strip's avatars and
    /// deliver the nick of the avatar there (or `nil` for no avatar) on the
    /// MAIN thread. The single engine-queue hop the view's click handler needs:
    /// `cc_strip_hit_test_avatar` is a `cc_*` call (engine queue only), and the
    /// result (a nick) is what the UI toggles in its talk-to selection. `x`/`y`
    /// are PAGE TWIPS, y-up (the same space `Strip.size`/`compose` use) — the
    /// caller (`ComicStripView`) converts the AppKit view point + the twips
    /// y-flip before calling. `completion` always runs on the main thread,
    /// exactly once.
    public func hitTestNick(atTwips x: Int32, _ y: Int32,
                            completion: @escaping @Sendable (String?) -> Void) {
        engineQueue.async { [weak self] in
            var nick: String? = nil
            if let self, !self.isShutDown, let strip = self.strip,
               let id = strip.hitTestAvatar(xTwips: x, yTwips: y) {
                nick = self.participantNickLocked(for: id)
            }
            DispatchQueue.main.async { completion(nick) }
        }
    }

    /// Hit-test a page-twips point against the composed strip's balloons and
    /// deliver the balloon's displayed text (or `nil` for no balloon there) on
    /// the MAIN thread — the balloon-text tooltip. Same engine-queue-hop +
    /// page-twips y-up contract as `hitTestNick`. `completion` always runs on
    /// the main thread, exactly once.
    public func hitTestBalloonText(atTwips x: Int32, _ y: Int32,
                                   completion: @escaping @Sendable (String?) -> Void) {
        engineQueue.async { [weak self] in
            var text: String? = nil
            if let self, !self.isShutDown, let strip = self.strip {
                text = strip.hitTestBalloon(xTwips: x, yTwips: y, encoding: self.config.encoding)
            }
            DispatchQueue.main.async { completion(text) }
        }
    }

    /// Plan 4b Batch D (panel export/copy): the single panel at `index`
    /// (0-based), composed alone onto a fresh `CGCanvas` sized to the unit
    /// panel box (`Strip.panelGeometry`'s `unitW`/`unitH` — panels are
    /// uniform unit panels; see `Strip.composePanel`'s doc comment). `scale`
    /// mirrors `recomposeLocked`'s CGCanvas supersampling factor. `nil` when
    /// no strip exists yet, `index` is out of range, or the compose fails —
    /// the caller (the strip view's context menu) then shows/exports
    /// nothing rather than crashing.
    ///
    /// ENGINE-QUEUE SYNC HOP: unlike `hitTestNick`/`hitTestBalloonText`
    /// (async + a completion callback), this is a synchronous
    /// `engineQueue.sync` read-through — the same posture `panelCount`/
    /// `panelGeometry` already use — because the call site (a right-click
    /// context-menu action) needs the image immediately to build the
    /// pasteboard/save-panel payload, not via a later callback.
    public func panelImage(at index: Int32, scale: CGFloat = 2.0) -> CGImage? {
        engineQueue.sync {
            guard let strip, !isShutDown else { return nil }
            let geo = strip.panelGeometry
            guard geo.unitW > 0, geo.unitH > 0 else { return nil }
            let canvas = CGCanvas(widthTwips: geo.unitW, heightTwips: geo.unitH, scale: scale)
            guard (try? strip.composePanel(index, onto: canvas)) != nil else { return nil }
            return canvas.makeCGImage()
        }
    }

    // MARK: - character / backdrop switching (Plan 4b Task 5)

    /// Switches the SELF participant's avatar to `name` (a bare comicart name,
    /// no directory/extension — same convention as `ChatConfig.characterName`).
    ///
    /// Plan 4b Task 5 fix round 1 (reviewer-confirmed transcript-doctrine
    /// finding): a character change IS wire-visible, as a channel-wide
    /// `"# Appears as <name>"` announce — and that shape already has a full
    /// existing handler, `.appearsAs`, which BOTH re-avatars the target
    /// participant (`ProtocolStripBridge.apply`'s `.appearsAs` case ->
    /// `Strip.setParticipantAvatar`, Task 4a-6) AND lands in `_transcript`.
    /// So rather than calling `strip.setParticipantAvatar` directly (the
    /// pre-fix shape, which mutated the strip live but recorded NOTHING in
    /// the event log — invisible to `reflowLocked`'s transcript replay, so a
    /// viewport resize after a switch silently rewrote every PRE-switch panel
    /// with the post-switch avatar), this now SYNTHESIZES a `.appearsAs` for
    /// our own nick and routes it through the exact same `handleLocked` path
    /// a peer's avatar announcement takes:
    ///   1. update `config.characterName` FIRST (before constructing/handling
    ///      the synthetic event) — `handleLocked`'s `.appearsAs` case reads
    ///      `config.characterName` for the reply-announce name (irrelevant
    ///      here, guarded off below). Live-fix 7 note: `emitSelfPoseLocked`
    ///      no longer keeps a separate self-avatar file to rebuild — it reads
    ///      the strip's own live self avatar (`cc_strip_self_preview`), which
    ///      the `.appearsAs`-driven `setParticipantAvatar` re-points below — so
    ///      the wheel preview follows the switch automatically with no per-
    ///      character-switch reset;
    ///   2. synthesize `.appearsAs(nick: currentOwnNick, avatarName:
    ///      name.capitalized, url: "")` and hand it to `handleLocked` DIRECTLY
    ///      (not `enqueueHandle`, which would re-hop `engineQueue.async` —
    ///      unnecessary and slower, since `changeCharacter` is ALREADY running
    ///      on the engine queue here, same "already on-queue" posture
    ///      `sendWhisper`'s trailing `engineQueue.async` block documents for
    ///      the opposite case). `handleLocked`'s `.appearsAs` case appends to
    ///      `_transcript` (recording the switch at its correct reflow
    ///      position) and calls `bridge.apply(ev)`, which resolves
    ///      `avatarName` through the SAME `AvatarResolver` a peer's switch
    ///      uses and calls `Strip.setParticipantAvatar` — so the actual
    ///      engine-side effect is identical to the pre-fix direct call,
    ///      FUTURE-PANELS-ONLY semantics, original-faithful (existing panels
    ///      keep the OLD avatar exactly like a peer's `.appearsAs` switch —
    ///      histent.cpp:368-413's documented behavior);
    ///   3. calls `emitSelfPoseLocked()` so the wheel's live preview updates
    ///      immediately to the new character's current pose;
    ///   4. fires `session.announceAvatar` fire-and-forget (a detached
    ///      `Task`, same shape as `.selfJoined`'s own announce in
    ///      `handleLocked` — SetMyAvatar's announce-on-change, avatar.cpp:
    ///      585-599) — the REAL wire announce, unchanged from before this fix.
    ///
    /// `fromServer: false` on the synthetic (mirrors `send(_:)`'s own
    /// synthetic self-say) — this, together with the nick equaling
    /// `currentOwnNick`, is exactly what `handleLocked`'s `.appearsAs` case
    /// now checks to SKIP the private reply-announce branch (that branch
    /// exists to greet a PEER appearing for the first time; our own
    /// synthetic must never trigger a reply-announce to ourselves — see that
    /// case's own doc comment).
    ///
    /// KNOWN DEVIATION from the pre-fix shape: the OLD code guarded
    /// `config.characterName`'s mutation behind `strip.setParticipantAvatar`'s
    /// own throw (a bad/missing `.avb` path left every bit of state untouched).
    /// Routing through `handleLocked` -> `bridge?.apply(ev)` (which swallows the
    /// throw via `try?`, matching every other case in that switch) drops that
    /// guard: `config` is now mutated, and the wire announce still fires,
    /// even if the underlying avatar file fails to resolve/load. Accepted:
    /// every real caller (`CharacterPickerView.select`, the one production
    /// call site) only ever passes a name from `buildCatalog(artDir:)`'s
    /// curated, filesystem-verified catalog, so this path is unreachable in
    /// practice — and the alternative (duplicating a pre-check here before
    /// synthesizing the event, just to preserve a guard no real caller can
    /// trigger) would reintroduce the two-code-paths-for-one-avatar-switch
    /// split this fix exists to collapse.
    ///
    /// Fire-and-forget: hops onto the engine queue and returns immediately,
    /// same posture as `setEmotion`/`previewTyping`/`setViewport`.
    /// Plan 4b Task 7 (multi-room, clarification 1): the self character-switch
    /// is appended to EVERY joined room's transcript (each room's replay needs
    /// the `.appearsAs` positionally so a later `setActiveRoom` rebuild
    /// re-avatars the self participant correctly), applied ONCE to the current
    /// live strip (the active room), and the wire announce goes to EVERY joined
    /// room (the original's per-CRoomInfo announce). The synthetic is appended
    /// DIRECTLY to each room's transcript rather than routed through
    /// `handleLocked` per-room (which would recompose N times and re-run the
    /// reply-announce guard N times) — its only strip effect is the single
    /// `bridge.apply` on the active strip, done here.
    public func changeCharacter(_ name: String) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown, self.strip != nil,
                  self.selfParticipantID != nil else { return }
            self.config.characterName = name
            let synthetic = ProtocolEvent.appearsAs(nick: self.currentOwnNick,
                                                    avatarName: name.capitalized, url: "")
            // Record the switch in every room's transcript at its correct
            // reflow position (clarification 1).
            for room in self.roomOrder {
                self.rooms[room]?.transcript.append(synthetic)
            }
            // Apply to the live strip (the active room) once.
            self.applyToBridgeLocked(synthetic)
            self.recomposeLocked()
            self.emitSelfPoseLocked()
            // The REAL wire announce to every joined room.
            let session = self.session
            let announceName = name.capitalized
            let joinedRooms = self.roomOrder
            Task {
                for room in joinedRooms {
                    try? await session.announceAvatar(channel: room, name: announceName)
                }
            }
        }
    }

    /// Switches the strip's backdrop to `name` (a bare comicart name). No
    /// reflow: `bridge.setBackdrop`/`Strip.setBackdrop`'s contract is that
    /// SUBSEQUENT panels inherit the new backdrop (comicchat.h's
    /// `set_backdrop` doc comment) — existing panels keep the old one,
    /// exactly like `changeCharacter`'s future-panels-only avatar switch.
    /// Also updates `config.backdropName` (so a later `reflowLocked()`
    /// re-applies the NEW backdrop, matching `changeCharacter`'s same
    /// `config` update). Fire-and-forget, same posture as `changeCharacter`.
    ///
    /// KNOWN DEVIATION (documented, not fixed — Plan 4b Task 5 fix round 1
    /// coordinator ruling): unlike `changeCharacter` (fixed this round to
    /// record the switch as a transcript `.appearsAs` entry, replayed at its
    /// correct position on reflow), a mid-session backdrop change is NOT
    /// recorded in `_transcript` at all — `reflowLocked` seeds the WHOLE
    /// reflowed strip from `config.backdropName` (the CURRENT backdrop,
    /// mutated above) via `setUpStripLocked`'s `bridge.setBackdrop` call,
    /// which means every panel — including ones authored before this switch
    /// — gets rebuilt with the CURRENT backdrop after a reflow. The original
    /// tracked this positionally too (`ChangeBackDropEntry`, replayed on
    /// `HM_RELOAD`, histent.cpp:557-577 in this repo's copy of the source) —
    /// so this IS the same class of doctrine gap `changeCharacter` had. It is
    /// deliberately NOT fixed here: unlike an avatar switch, there is no
    /// existing wire-visible `ProtocolEvent` case a backdrop change could
    /// piggyback on the way `changeCharacter` piggybacks on `.appearsAs` —
    /// the live wire form is IRCX `PROP <chan> bk <name>[.<url>]`
    /// (comicchat.h's own `set_backdrop` grammar note), which this port has
    /// not yet pinned a `ProtocolEvent` case for. Fixing this properly needs
    /// that event added first, which needs the IRCX PROP `bk` grammar pinned
    /// against a real capture — deferred to Plan 4b §8 (live acceptance),
    /// where a real server's backdrop-change wire form can be captured and
    /// verified rather than guessed at here.
    public func changeBackdrop(_ name: String) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown, let bridge = self.bridge else { return }
            let bgbPath = self.config.artDir + "/" + name + ".bgb"
            guard (try? bridge.setBackdrop(bgbPath)) != nil else { return }
            self.config.backdropName = name
        }
    }

    /// ENGINE QUEUE ONLY. Renders the SELF participant's CURRENT posed body
    /// (head + torso + masks) via `Strip.selfPreviewImage(...)` — the engine's
    /// own `CBody::DrawBody` path, the original bodycam pane's draw — and hands
    /// the image to `onSelfPose` on the main thread.
    ///
    /// Live-fix 7: this REPLACES the old `selfPoseIndex()` ->
    /// `AvatarFile.poseImage(_:)` single-record preview, which drew ONE pose
    /// record and therefore rendered only the TORSO for a complex (two-part)
    /// avatar — a headless body (Tim's live report). `selfPreviewImage`
    /// composites the head and torso planes exactly as the engine does when it
    /// draws the avatar in a panel, so both parts appear. It also removes the
    /// standalone `selfAvatarFile` handle and its per-character-switch reset
    /// choreography: the preview now reads the strip's OWN live self avatar
    /// (`cc_strip_self_preview` resolves it via the current avatar id), so a
    /// `changeCharacter` switch is reflected automatically with no separate
    /// file to reopen or index-space to reconcile.
    ///
    /// The twips bounds define the aspect box the engine fits the body into
    /// (portrait, matching a standing figure); the pose pane in the UI
    /// aspect-fits the resulting image again, so the exact numbers only set the
    /// intrinsic aspect ratio, not the on-screen size. A nil result (no self
    /// set yet) clears the preview, exactly as the old guard did.
    private func emitSelfPoseLocked() {
        guard let strip else { return }
        // Portrait aspect box for a full standing body; the engine's GetBodyBox
        // aspect-fits + centers + bottom-pins the body within it.
        let image = strip.selfPreviewImage(widthTwips: 2400, heightTwips: 3600)
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

    // MARK: - room ops (Plan 4b Task 8)
    //
    // Thin pass-throughs over `ProtocolSession`'s own 6 wrappers — `AppState`
    // (the app layer) talks to `ChatSessionModel`, never `ProtocolSession`
    // directly (same posture as `send`/`sendWhisper` wrapping `session.say`/
    // `session.whisper`), so these exist purely to keep that boundary
    // consistent rather than adding any behavior of their own.

    /// Creates (and, per the engine's implicit-join-on-create semantics,
    /// joins) a room — the Create Room… sheet's `createRoom` half (paired
    /// with `goToRoom` to open the tab locally, `AppState.createRoom`'s own
    /// doc comment).
    public func createRoom(_ channel: String, modes: String? = nil, maxUsers: UInt32 = 0, key: String? = nil) async throws {
        try await session.createRoom(channel, modes: modes, maxUsers: maxUsers, key: key)
    }

    public func kick(_ channel: String, nick: String, reason: String? = nil) async throws {
        try await session.kick(channel, nick: nick, reason: reason)
    }

    public func invite(_ channel: String, nick: String) async throws {
        try await session.invite(channel, nick: nick)
    }

    /// Quick-wins batch item 6 (Room menu's "Set Topic…"): thin pass-through
    /// over `ProtocolSession.setTopic` — the server's `.topicChanged` confirm
    /// is what actually updates `RoomBox.topic` (`handleLocked`'s case above),
    /// not this call directly, matching every other room-op's "fire the wire
    /// command, the event confirms it" shape on this type.
    public func setTopic(_ channel: String, topic: String) async throws {
        try await session.setTopic(channel, topic: topic)
    }

    public func ban(_ channel: String, pattern: String, banning: Bool) async throws {
        try await session.ban(channel, pattern: pattern, banning: banning)
    }

    public func setRoomMode(_ channel: String, mode: UInt32, maxUsers: UInt32, password: String? = nil) async throws {
        try await session.setRoomMode(channel, mode: mode, maxUsers: maxUsers, password: password)
    }

    /// Session-scoped Away toggle (Plan 4b Task 8: the Room menu's Away
    /// item) — no room token, mirrors `ProtocolSession.setAway`'s own
    /// session-scoped shape.
    public func setAway(_ isAway: Bool, message: String? = nil) async throws {
        try await session.setAway(isAway, message: message)
    }

    // MARK: - room list / Get Info (Plan 4b Task 8)

    /// Requests the server's room list (`CRoomList`'s LIST browser, D1
    /// §1.6/backlog item 5). The result arrives asynchronously via
    /// `onRoomList` once the accumulated `.roomListBegin` -> `.roomListItem`×N
    /// -> `.roomListEnd` sequence completes (`handleLocked`'s `roomListAccum`
    /// case) — this method only fires the wire LIST, it does not itself
    /// return the items (mirroring every other fire-the-query/consume-the-
    /// event-later shape on this type, e.g. `who`/`.whoResult` below).
    ///
    /// Plan 4b Task 8 self-review fix: a second call while a LIST is already
    /// in flight (`roomListRequestInFlight` — set here BEFORE the wire send,
    /// cleared by `.roomListEnd`) is a silent no-op rather than sending a
    /// second wire LIST. Without this, `RoomListWindow`'s Refresh button (or
    /// its `.task`+Refresh racing on window (re)open) could issue two
    /// back-to-back LIST queries whose `.roomListBegin`/`.roomListItem`/
    /// `.roomListEnd` replies interleave against the SAME shared
    /// `roomListAccum` (no per-request identity) — the second `.roomListBegin`
    /// would reset the accumulator mid-way through the first round-trip,
    /// producing a spliced/corrupted item list on whichever `.roomListEnd`
    /// arrives next. Gating on a flag set BEFORE the send (not merely on
    /// `roomListAccum != nil`, which only becomes true once `.roomListBegin`
    /// is actually RECEIVED) closes the window where two rapid calls could
    /// both race past an accumulator-only check before either reply lands.
    public func requestRoomList() async throws {
        let alreadyInFlight = engineQueue.sync { () -> Bool in
            if roomListRequestInFlight { return true }
            roomListRequestInFlight = true
            return false
        }
        guard !alreadyInFlight else { return }
        try await session.list()
    }

    /// Get Info (Plan 4b Task 8): queries WHO for `nick` — there is NO
    /// `cc_session_whois` builder in the outbound surface (comicchat.h:600
    /// has `who` only), so this is implemented over the EXISTING
    /// `ProtocolSession.who(_:)` + the `.whoResult` event
    /// (`handleLocked`'s case above formats and fires `onUserInfo`).
    public func getInfo(_ nick: String) async throws {
        try await session.who(nick)
    }

    // MARK: - send

    /// Sends `text` under `mode` (default `.say`), with COOKED outbound pose
    /// annotations built from the CURRENT wheel/preview state (the original
    /// grabs the bodycam state at send time too — Task 9's `annotations: nil`
    /// MVP scope decision is superseded by this task), UNLESS
    /// `config.sendComicsData == false` (Task 5's opt-out: peers then run
    /// text inference instead — the original's ComicsData toggle semantics).
    /// The wheel/preview themselves are unaffected either way (they read
    /// `strip.selfAnnotations()`/pose state directly, never this method's
    /// `ann` value) — only what goes out over the wire (and what the own-say
    /// local render carries, kept consistent with the wire per this method's
    /// own OWN-SAY RENDERING note below) is gated.
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
    /// - Parameter room: the room to send into (Plan 4b Task 7). `nil`
    ///   (default) targets the ACTIVE room — the compose bar always sends into
    ///   the tab it's showing, and the app leaves this `nil` since the active
    ///   room IS the shown tab. A caller MAY pass an explicit room to send into
    ///   a background room without switching to it.
    /// - Parameter addressees: Plan 4b Task 8 (D1 §1.5): the member-list
    ///   selection, appended so existing callers (which never passed this) are
    ///   unaffected. Threaded into `ann.addressees` when comics-data annotations
    ///   are being sent at all (`sendComicsData` gate above, unchanged) — the
    ///   wire `T<nick>` list rides free via the annotation encoder
    ///   (`Annotations.toCAnnotations`'s existing clip-at-5, D1 §2.1). Passed
    ///   through as given rather than pre-clipping here: the encoder is the
    ///   single source of truth for the 5-addressee wire limit.
    public func send(_ text: String, mode: Strip.Mode = .say, room: String? = nil,
                     addressees: [String] = []) async throws {
        // `performOnEngineQueue` traps if called while already ON the engine
        // queue (its own doc comment) — `send` is invoked from the UI/main
        // context (ChatWindow's Task { try? await model.send(...) }), never
        // from inside `handleLocked`/the event consumer, so this is legal,
        // same posture as the pre-existing `session.say` call below.
        //
        // `config`/`activeRoom` READ HAZARD (Plan 4b Task 5/7): both are
        // engine-queue-owned (`changeCharacter`/`changeBackdrop`/
        // `setActiveRoom` mutate them ON the engine queue) — so a bare
        // off-queue read here would race those writes. All needed values are
        // read inside this SAME `performOnEngineQueue` call.
        //
        // Live-fix 1: `room` (a caller-supplied name, e.g. from `RoomInfo
        // .name`/any other casing) is resolved to that room's DISPLAY name
        // via `roomKey(_:)` -> `rooms[key]?.displayName` — `targetRoom` must
        // be the actual wire-cased channel string (`session.say` and the
        // synthetic event's `channel:` both need this, not a lookup key). A
        // room the caller names that isn't actually joined falls back to the
        // raw caller string (matches the pre-fix behavior of just using
        // whatever was passed — `session.say` itself will fail with
        // `.commandFailed` for an unknown channel, same as before).
        let (sendComicsData, targetRoom): (Bool, String) = session.performOnEngineQueue { [self] in
            let resolvedRoom = room.map { rooms[roomKey($0)]?.displayName ?? $0 }
                ?? (rooms[activeRoom]?.displayName ?? activeRoom)
            return (config.sendComicsData, resolvedRoom)
        }
        let ann: Annotations? = session.performOnEngineQueue { [self] in
            guard sendComicsData else { return nil }
            guard var a = try? strip?.selfAnnotations() else { return nil }
            a.mode = Self.smMode(for: mode)
            a.addressees = addressees
            return a
        }
        try await session.say(targetRoom, text: text, annotations: ann,
                              modes: UInt16(mode.rawValue))
        // Registered BEFORE the synthetic event is enqueued (4a carryover:
        // own-say echo dedup, `handleLocked`'s doc comment) so a server echo
        // of this same text — which can only arrive after `session.say`
        // above has already put the PRIVMSG on the wire — always finds a
        // pending entry to consume, however the two async paths interleave.
        engineQueue.async { [weak self] in self?.pendingLocalEchoes.append(text) }
        let ownNick = session.ownNick
        let synthetic = ProtocolEvent.text(nick: ownNick, ident: "", target: targetRoom,
                                          text: text, kind: 0, annotations: ann)
        // Route the synthetic through the target room's channel (Plan 4b Task
        // 7) so it lands in that room's transcript and renders on the live
        // strip when that room is active (own-say is never counted unread —
        // `handleLocked`'s message-vs-active gate handles a background target).
        enqueueHandle(synthetic, channel: targetRoom, fromServer: false)
    }

    // MARK: - Send Sound (outbound-sound task)

    /// Send Sound: sends `\x01SOUND "<file>" <text>\x01` to the ACTIVE room
    /// (`session.sendSound`, `cc_session_send_sound`) and renders/plays the
    /// own line the SAME WAY an inbound `.sound` event does — the original
    /// plays the sound locally too (sounddlg.cpp:416-434's dialog is
    /// "browse, preview, and send" — sending always includes local playback,
    /// not just an announce) — by enqueueing a synthetic `.sound` event
    /// through the SAME `handleLocked` path an inbound `CC_EV_SOUND` takes,
    /// so `onSound`'s callback (and therefore `AppState.playSound`'s
    /// settings-gated `AVAudioPlayer`) fires identically for both directions;
    /// there is no separate/parallel playback call here.
    ///
    /// `text` mirrors `send`'s free-form message parameter but defaults to
    /// `""` — the picker UI (a simple "browse your sounds folder and send"
    /// popover, no message-composition field) never supplies one, matching
    /// the original dialog's own "just pick a sound" common path; a caller
    /// that DOES have accompanying text may still pass it.
    public func sendSound(file: String, text: String = "") async throws {
        // Same `config`/`activeRoom` read-hazard fix as `send(_:mode:)`'s own
        // doc comment (Plan 4b Task 5/7: both are engine-queue-owned).
        let targetRoom: String = session.performOnEngineQueue { [self] in
            rooms[activeRoom]?.displayName ?? activeRoom
        }
        try await session.sendSound(targetRoom, file: file, text: text)
        // Registered BEFORE the synthetic event is enqueued -- same ordering
        // reasoning as `send(_:mode:)`'s own doc comment: a server echo of
        // this same sound line can only arrive after `session.sendSound`
        // above has already put the PRIVMSG on the wire, so the pending
        // entry always exists first regardless of how the two async paths
        // interleave.
        engineQueue.async { [weak self] in self?.pendingLocalSoundEchoes.append(file) }
        let ownNick = session.ownNick
        let synthetic = ProtocolEvent.sound(nick: ownNick, file: file, text: text)
        // Route through the target room's channel (Plan 4b Task 7, same
        // posture as `send(_:mode:)`) so it lands in that room's transcript
        // and — via `handleLocked`'s `.sound` case — fires `onSound`
        // regardless of which room is currently active (playback is
        // session-level, matching the inbound case's own posture).
        enqueueHandle(synthetic, channel: targetRoom, fromServer: false)
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
        // Same `config` read-hazard fix as `send(_:mode:)`'s own doc comment
        // (Plan 4b Task 5/7: `config`/`activeRoom` are engine-queue-owned) —
        // read inside this SAME `performOnEngineQueue` call. Plan 4b Task 7:
        // the whisper's room context is the ACTIVE room (the tab the user is
        // whispering FROM) — the wire form is still a plain PRIVMSG to `peer`
        // (`cc_session_send_whisper`), the room only scopes the token/
        // annotations context.
        // Live-fix 1: `activeRoom` is a case-folded LOOKUP KEY — resolve the
        // active room's DISPLAY name for the wire call (`session.whisper`'s
        // `channel:` resolves a token via `ProtocolSession`'s own
        // `rooms[channel]`, keyed by whatever display string `session.join`
        // was actually called with).
        let (ann, room): (Annotations?, String) = session.performOnEngineQueue { [self] in
            let displayRoom = rooms[activeRoom]?.displayName ?? activeRoom
            guard var a = try? strip?.selfAnnotations() else { return (nil, displayRoom) }
            a.mode = Self.smMode(for: .whisper)
            a.addressees = [peer]
            return (a, displayRoom)
        }
        try await session.whisper(to: [peer], text: text, channel: room, annotations: ann)
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
    /// Quick-wins batch item 3: when `config.panelsPerRow > 0`, `setViewport`
    /// uses it as `columns` directly instead of `PanelFit`'s auto-fit scan —
    /// this closure recomputes `columns`/`unit` from the CURRENT
    /// `config.panelsPerRow` each time it runs (rather than capturing a value
    /// at call time), matching `setPanelsPerRow`'s own doc comment: a live
    /// setting change re-runs exactly this geometry derivation at the
    /// CURRENT width, so the two entry points (a real resize vs. a settings
    /// change) share one source of truth for "what geometry follows from the
    /// current width + override".
    public func setViewport(widthPoints: CGFloat, scale: CGFloat) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            self.lastViewportWidthPoints = widthPoints
            self.applyViewportLocked(widthPoints: widthPoints, scale: scale)
        }
    }

    /// ENGINE QUEUE ONLY. The shared geometry-derive-then-reflow-if-changed
    /// body `setViewport`/`setPanelsPerRow` both drive — factored out
    /// (quick-wins batch item 3) so a live panels-per-row change re-runs
    /// EXACTLY this logic rather than a hand-duplicated copy that could drift.
    /// When `config.panelsPerRow > 0`, it is used as `columns` directly
    /// instead of `PanelFit`'s auto-fit scan (unit width is still derived from
    /// the viewport via `PanelFit.unitPanelTwips(viewportWidthTwips:columns:)`,
    /// just fed this forced column count).
    private func applyViewportLocked(widthPoints: CGFloat, scale: CGFloat) {
        let viewportTwips = Int32((widthPoints * 20).rounded())
        let columns = config.panelsPerRow > 0
            ? Int32(config.panelsPerRow)
            : PanelFit.columns(forViewportWidthTwips: viewportTwips)
        let unit = PanelFit.unitPanelTwips(viewportWidthTwips: viewportTwips, columns: columns)
        currentScale = scale
        if didSetViewport, columns == currentColumns, unit == currentUnitTwips {
            recomposeLocked()
            return
        }
        didSetViewport = true
        currentColumns = columns
        currentUnitTwips = unit
        // Reflow the ACTIVE room (Plan 4b Task 7): a viewport change only
        // affects the one live strip, which the active room owns.
        rebuildStripLocked(for: activeRoom, resetAnnounce: true)
    }

    /// The last width `setViewport` was called with (points), engine-queue-
    /// owned — `setPanelsPerRow` replays a geometry recompute at THIS width
    /// (a settings change carries no width of its own; the last real viewport
    /// call is the only width this model knows). `0` until the first
    /// `setViewport` call (matches `didSetViewport`'s own "nothing to redo
    /// yet" gate below).
    private var lastViewportWidthPoints: CGFloat = 0

    /// Quick-wins batch item 3 (live change, Task 3's "AppState observes the
    /// setting change… update config… re-run the setViewport logic" plumbing):
    /// updates `config.panelsPerRow` and, if a viewport has already been
    /// established, immediately re-derives columns/unit at the LAST KNOWN
    /// width via `applyViewportLocked` and reflows if the geometry actually
    /// changed — same "no-op if unchanged, else full reflow" shape
    /// `setViewport` itself has, since both now call the identical helper. A
    /// no-op before any `setViewport` has run (`didSetViewport == false` —
    /// there is no live strip/width yet for a settings change to affect; the
    /// NEW override still takes effect on the next real `setViewport` call,
    /// e.g. the window's own layout pass).
    public func setPanelsPerRow(_ perRow: Int) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            self.config.panelsPerRow = perRow
            guard self.didSetViewport, self.lastViewportWidthPoints > 0 else { return }
            self.applyViewportLocked(widthPoints: self.lastViewportWidthPoints, scale: self.currentScale)
        }
    }

    /// Plan 4b Batch C: updates `config.profileText` live (no reflow needed —
    /// unlike `setPanelsPerRow` above, this value is only ever READ at
    /// `.infoRequest` reply time, `handleLocked`'s own case, never affects
    /// the strip). Lets the Persona tab's profile `TextEditor` write straight
    /// through to an already-connected session, same "Settings edit applies
    /// immediately" posture as `changeCharacter`/`changeBackdrop`.
    public func setProfileText(_ text: String) {
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            self.config.profileText = text
        }
    }

    /// ENGINE QUEUE ONLY. Destroy + recreate the ONE live strip and replay
    /// `room`'s transcript into it (Plan 4b Task 7: the generalized reflow —
    /// `setViewport` calls it for the active room on a geometry change,
    /// `setActiveRoom` calls it for the room being switched to). See
    /// `setViewport`'s doc comment for the full rationale: "replay is the
    /// reflow" — the room's transcript IS its event log, so tearing down and
    /// re-applying it reproduces that room's exact strip content at the current
    /// panel geometry. The metrics canvas installed in `setUpStripLocked` stays
    /// live (a process-global registration, not per-strip).
    ///
    /// ONE STRIP AT A TIME (engine UB territory): the teardown-then-create
    /// order here mirrors the proven single-room reflow exactly — `strip?.close()`
    /// FIRST, null out `strip`/`bridge`, THEN `setUpStripLocked` creates the
    /// new one. At no point do two strips coexist.
    ///
    /// - Parameter resetAnnounce: clears `announcedBackTo` (the pre-Task-7
    ///   viewport-reflow behavior — a resize re-greets peers on their next live
    ///   announce). `setActiveRoom` passes `false`: a tab switch must NOT
    ///   re-greet already-greeted peers (`announcedBackTo` is session-scoped).
    private func rebuildStripLocked(for room: String, resetAnnounce: Bool) {
        strip?.close()
        strip = nil
        bridge = nil
        selfParticipantID = nil
        if resetAnnounce { announcedBackTo.removeAll() }

        // `setUpStripLocked` seeds the title from `activeRoom`, so ensure it
        // points at the room being rebuilt (the caller sets this too, but keep
        // it robust — `activeRoom` is the single source of truth for the title
        // and the `_transcript` computed property this replays).
        activeRoom = room
        guard (try? setUpStripLocked(isReflow: true)) != nil else { return }
        guard bridge != nil else { return }
        for ev in rooms[room]?.transcript ?? [] {
            // Quick-wins batch item 5 (ignore doctrine, `ignoredNicks`'s own
            // doc comment): a reflow re-derives the ENTIRE strip from the
            // transcript from scratch, so the SAME ignore guard `handleLocked`
            // applies live (`.text`/`.action`/`.whisper`/`.sound`) must also
            // apply HERE — otherwise an ignored nick's already-transcript-
            // recorded lines would resurrect on the very next viewport
            // resize/tab switch, since replay has no memory of what live
            // rendering chose to skip. `.sound` needs no entry here (it was
            // never applied to the strip in the first place — audio-only,
            // no bridge.apply either live or replayed).
            if isEventFromIgnoredNickLocked(ev) { continue }
            applyToBridgeLocked(ev)
        }
        recomposeLocked()
    }

    /// ENGINE QUEUE ONLY. Whether `ev` is one of the ignore-filterable strip
    /// events (`.text`/`.action`/`.whisper`) authored by a currently-ignored
    /// nick — the shared predicate `rebuildStripLocked`'s replay loop uses so
    /// it can't drift from `handleLocked`'s own per-case guards above (both
    /// apply sites must agree, per `ignoredNicks`'s doc comment). Any other
    /// event kind (joins/parts/`.appearsAs`/etc.) is never filtered by
    /// ignore — only the three message-shaped kinds carry an "author" whose
    /// ambient chatter ignore is meant to declutter.
    private func isEventFromIgnoredNickLocked(_ ev: ProtocolEvent) -> Bool {
        let authorNick: String?
        switch ev {
        case .text(let nick, _, _, _, _, _): authorNick = nick
        case .action(let nick, _, _): authorNick = nick
        case .whisper(let nick, _, _, _): authorNick = nick
        default: authorNick = nil
        }
        guard let authorNick else { return false }
        return ignoredNicks.contains(ignoreKey(authorNick))
    }

    // MARK: - room management (Plan 4b Task 7)

    /// Joins an ADDITIONAL room on the same connection (Plan 4b Task 7). The
    /// server's `.selfJoined` confirm creates the room box + announces our
    /// avatar in that channel (`handleLocked`'s `.selfJoined` case). Registers
    /// the tab-order slot immediately so the tab appears before the confirm
    /// lands (the room shows with 0 unread until its first message). Does NOT
    /// switch to the new room — the caller decides when to `setActiveRoom`.
    ///
    /// Live-fix 1: if `room` case-folds (`roomKey(_:)`) to an ALREADY-joined
    /// room, this is a no-op on the wire (no redundant JOIN sent — the server
    /// already considers us a member under whatever casing it confirmed
    /// originally) rather than creating a second box/tab for the same
    /// channel under different casing.
    public func joinRoom(_ room: String) async throws {
        let alreadyJoined = engineQueue.sync { () -> Bool in
            if self.rooms[self.roomKey(room)] != nil { return true }
            self.ensureRoomBoxLocked(room)
            return false
        }
        emitRooms()
        guard !alreadyJoined else { return }
        try await session.join(room)
    }

    /// "Go To" a room from the LIST browser (Plan 4b Task 8, coordinator
    /// ruling post-multi-room): `joinRoom` + `setActiveRoom` — opens a NEW
    /// tab, NO part-first (leaving rooms is the tab's own close button, not
    /// something Go To does on the caller's behalf). `joinRoom`'s
    /// `ensureRoomBoxLocked` call registers the room's tab-order slot
    /// synchronously before the wire JOIN is even sent, so `setActiveRoom`'s
    /// `rooms[room] != nil` guard is already satisfied by the time this calls
    /// it — the switch does not need to wait for the server's `.selfJoined`
    /// confirm (mirrors `EnterRoomSheet`'s existing join-then-show posture;
    /// the strip starts title-only and fills in as the join confirms/messages
    /// arrive, same as any other freshly joined room).
    public func goToRoom(_ room: String) async throws {
        try await joinRoom(room)
        setActiveRoom(room)
    }

    /// Leaves `room` (Plan 4b Task 7). Sends the wire PART, drops the room's
    /// box/tab, and — if it was the active room — activates another surviving
    /// room (falling back to the first in tab order) or clears the strip if it
    /// was the last one.
    ///
    /// Live-fix 1: `room` (a caller-supplied name — typically `RoomInfo.name`,
    /// the display form) is resolved to its DISPLAY name for the wire PART
    /// (`session.part` requires the exact channel string `session.join`
    /// registered a token for) and case-folded (`roomKey(_:)`) for every
    /// internal `rooms`/`roomOrder`/`activeRoom` lookup.
    public func leaveRoom(_ room: String) async throws {
        let key = roomKey(room)
        let wireChannel = engineQueue.sync { self.rooms[key]?.displayName ?? room }
        try await session.part(wireChannel)
        var newActive: String? = nil
        var wasActive = false
        engineQueue.sync {
            guard self.rooms[key] != nil else { return }
            wasActive = (key == self.activeRoom)
            self.rooms.removeValue(forKey: key)
            self.roomOrder.removeAll { $0 == key }
            if wasActive {
                newActive = self.roomOrder.first
            }
        }
        if wasActive, let newActive {
            setActiveRoom(newActive)
        } else if wasActive {
            // Last room left — tear down the live strip (no room to rebuild
            // from). The app clears its own strip image when `rooms` empties.
            engineQueue.sync {
                self.strip?.close()
                self.strip = nil
                self.bridge = nil
                self.selfParticipantID = nil
                self.activeRoom = ""
                self.lastComposedImage = nil
            }
        }
        emitRooms()
    }

    /// Switches the ONE live strip to `room` (Plan 4b Task 7). Engine-queue:
    /// stash the current strip's last image into the OLD room's box, tear down
    /// the strip and rebuild it from the NEW room's transcript (same proven
    /// reflow machinery, `rebuildStripLocked`), zero the new room's unread,
    /// recompose, and refresh the tab bar + member sidebar. A no-op if `room`
    /// is already active or not joined.
    ///
    /// Live-fix 1: `room` (a caller-supplied name — `RoomInfo.name`, any other
    /// casing, or an already-canonical key from an internal call site like
    /// `leaveRoom`'s fallback) is case-folded via `roomKey(_:)` before every
    /// lookup — folding an already-canonical key is idempotent, so internal
    /// callers that already pass a key are unaffected.
    public func setActiveRoom(_ room: String) {
        let key = roomKey(room)
        engineQueue.async { [weak self] in
            guard let self, !self.isShutDown else { return }
            guard self.rooms[key] != nil, key != self.activeRoom else { return }
            // Stash the outgoing room's last composed image (cached for an
            // instant switch-back — the app can show it while the new room
            // rebuilds, though `rebuildStripLocked` is fast enough that the
            // fresh recompose usually lands first).
            let previous = self.activeRoom
            if var box = self.rooms[previous] {
                box.lastImage = self.lastComposedImage
                box.lastSizePoints = self.lastComposedSizePoints
                self.rooms[previous] = box
            }
            // Rebuild the strip from the new room's transcript (title = new
            // room name, via `activeRoom` set inside `rebuildStripLocked`).
            self.rebuildStripLocked(for: key, resetAnnounce: false)
            // Zero the newly-active room's unread.
            self.rooms[key]?.unread = 0
            self.emitRooms()
            self.emitMembers(for: key)
        }
    }

    // MARK: - Auto-reconnect (spec §7)

    /// ENGINE QUEUE ONLY. Called from `handleLocked`'s `.disconnectedHint` case
    /// on an unexpected drop. Kicks off the backoff-reconnect loop, or — if a
    /// loop is ALREADY running (a failed reconnect attempt's own
    /// `.disconnectedHint` re-entering here) — is a no-op so the existing loop
    /// owns the retry cadence. Gated on `shouldAutoReconnect` (config opt-in +
    /// not shutting down); a replay-fixture / user-disconnected / shut-down
    /// session never reconnects.
    private func maybeStartReconnectLocked() {
        guard shouldAutoReconnect, !isReconnecting else { return }
        isReconnecting = true
        // Tear down the strip/bridge NOW so the dead session's stale strip
        // isn't left composing — the transcripts (which live in `rooms`) are
        // deliberately untouched, so a successful reconnect rebuilds each
        // room's strip from its retained transcript. `activeRoom`/`rooms`/
        // `roomOrder` all persist.
        strip?.close()
        strip = nil
        bridge = nil
        selfParticipantID = nil
        reconnectTask = Task { [weak self] in
            await self?.runReconnectLoop()
        }
    }

    /// The backoff-reconnect driver (spec §7): per attempt it waits out a
    /// capped-exponential backoff (2,4,8,…,60s), tears down the dead
    /// `ProtocolSession` and creates a FRESH one (`reconnectAttemptLocked`),
    /// calls `connect()` (whose internal IRCX-probe/login handshake runs
    /// async), then waits a bounded grace window for `.loggedIn` to land. A
    /// successful login flips `isReconnecting` to `false` (via
    /// `finishReconnectLocked`, driven from `handleLocked`'s `.loggedIn` case)
    /// AND cancels this task — so the loop observes `!isReconnecting` (or
    /// `Task.isCancelled`) and exits cleanly. If login doesn't land within the
    /// grace window (or `connect()` throws outright — port still dead), the
    /// loop advances to the next attempt. Cooperatively cancellable at every
    /// suspension point by `shutdown()`/`cancelReconnect()`.
    private func runReconnectLoop() async {
        for attempt in 1...maxReconnectAttempts {
            // Backoff BEFORE each attempt: 2,4,8,16,32,60,60,60 seconds.
            let delaySeconds = min(60, 1 << attempt)   // 1<<1==2 … capped at 60
            emitReconnectState(.reconnecting(attempt: attempt, of: maxReconnectAttempts))
            emitStatus("Connection lost — reconnecting in \(delaySeconds)s… (attempt \(attempt) of \(maxReconnectAttempts))")
            do {
                try await Task.sleep(nanoseconds: UInt64(delaySeconds) * 1_000_000_000)
            } catch {
                return   // cancelled (shutdown / manual supersede / success)
            }
            if Task.isCancelled { return }

            // Rebuild + rebind the session on the engine queue, then await the
            // connect OFF it. `reconnectAttemptLocked` swaps `session` + restarts
            // the consumer synchronously on the queue and hands back the fresh
            // session to connect.
            let fresh: ProtocolSession? = await withCheckedContinuation { cont in
                engineQueue.async { [weak self] in
                    guard let self, !self.isShutDown, self.isReconnecting else {
                        cont.resume(returning: nil); return
                    }
                    cont.resume(returning: self.reconnectAttemptLocked())
                }
            }
            guard let fresh else { return }   // shut down / superseded mid-rebuild
            do {
                try await fresh.connect()
            } catch {
                // TCP connect failed (port still dead) — next attempt's backoff.
                continue
            }
            if Task.isCancelled { return }

            // TCP up; the login handshake is now running inside `fresh`.
            // `.loggedIn` (through the consumer) will call
            // `finishReconnectLocked`, cancelling this task. Poll for that
            // cancellation across a bounded login grace window rather than
            // blindly scheduling the next attempt (which would tear down a
            // session that's mid-login). If the window elapses with no login
            // (dead-air server: accepts TCP, never completes IRC), fall through
            // to the next attempt.
            let loggedIn = await waitForReconnectLoginOrCancel()
            if loggedIn || Task.isCancelled { return }
            // else: no login in the grace window — advance to next attempt.
        }
        // Cap reached with no successful login: give up.
        await withCheckedContinuation { cont in
            engineQueue.async { [weak self] in
                self?.giveUpReconnectLocked()
                cont.resume()
            }
        }
    }

    /// Awaits either a successful reconnect-login (returns `true`) or the login
    /// grace window elapsing (`false`), whichever comes first. Login success is
    /// observed as `isReconnecting` flipping to `false` (set by
    /// `finishReconnectLocked`), polled on the engine queue. Cancellation
    /// (`shutdown`/`cancelReconnect`) makes the inner `sleep` throw, which we
    /// surface as `true` so the caller returns immediately without launching a
    /// redundant attempt (the loop's own `Task.isCancelled` check handles the
    /// exit).
    private func waitForReconnectLoginOrCancel() async -> Bool {
        // ~10s grace, polled every 100ms — comfortably covers a probe→login→001
        // round-trip on a healthy server while staying responsive to give-up.
        for _ in 0..<100 {
            do {
                try await Task.sleep(nanoseconds: 100_000_000)
            } catch {
                return true   // cancelled — treat as "done, stop looping"
            }
            let stillReconnecting = await withCheckedContinuation { (cont: CheckedContinuation<Bool, Never>) in
                engineQueue.async { [weak self] in
                    cont.resume(returning: self?.isReconnecting ?? false)
                }
            }
            if !stillReconnecting { return true }   // login landed (or gave up elsewhere)
        }
        return false
    }

    /// ENGINE QUEUE ONLY. Tears down the dead `ProtocolSession` and builds a
    /// FRESH one with the SAME config (incl. userName/realName/encoding),
    /// rebinds `self.session`, re-seeds `currentOwnNick`, and restarts the
    /// event consumer against the new session's `events` stream. Returns the
    /// fresh session for the caller to `connect()`. Does NOT touch
    /// `rooms`/`roomOrder`/`activeRoom`/transcripts — those persist across the
    /// reconnect so each room's strip rebuilds from its retained log on
    /// re-login. The room-token maps live inside `ProtocolSession` and are
    /// re-established by the `.loggedIn`-driven re-joins (`handleLocked`), which
    /// call `session.join` per room and re-`cc_session_register_room` on the
    /// fresh handle.
    private func reconnectAttemptLocked() -> ProtocolSession {
        // Old session down first (`disconnect()` is idempotent + marks the
        // teardown user-initiated so the dying session's own drop paths stay
        // quiet). Its `events` stream finishes on `deinit`, ending the old
        // consumer loop.
        session.disconnect()
        let fresh = ProtocolSession(host: config.host, port: config.port, nick: config.nick,
                                    encoding: config.encoding, engineQueue: engineQueue,
                                    userName: config.userName, realName: config.realName)
        session = fresh
        // Reset own-nick mirror to the requested nick — the fresh session
        // re-derives the confirmed nick from its own `.loggedIn` (which also
        // re-updates `currentOwnNick` in `handleLocked`).
        currentOwnNick = config.nick
        // Login-flow one-shots that gate outbound calls per room are cleared by
        // the fresh session; here we only need to re-arm the consumer.
        startEventConsumer()
        return fresh
    }

    /// ENGINE QUEUE ONLY. A reconnect (or the initial connect) reached
    /// `.loggedIn`: clear the loop state and notify `.connected`. No-op when no
    /// reconnect was in flight (an ordinary first login), so `handleLocked`'s
    /// `.loggedIn` case can call it unconditionally.
    private func finishReconnectLocked() {
        guard isReconnecting else { return }
        isReconnecting = false
        reconnectTask?.cancel()
        reconnectTask = nil
        // Rebuild the live strip for the active room from its RETAINED
        // transcript (the reconnect tore the strip down in
        // `maybeStartReconnectLocked` but left every room's transcript intact).
        // Without this, the strip would stay nil until a fresh geometry change
        // or tab switch — the `.selfJoined` re-confirm's `recomposeLocked()`
        // is a no-op while `strip`/`bridge` are nil. `resetAnnounce: false`
        // keeps `announcedBackTo` (peers greeted pre-drop stay greeted). Guarded
        // to a non-empty active room (a session dropped before ever joining has
        // none — nothing to rebuild).
        if !activeRoom.isEmpty, rooms[activeRoom] != nil {
            rebuildStripLocked(for: activeRoom, resetAnnounce: false)
        }
        emitStatus("Reconnected.")
        emitReconnectState(.connected)
    }

    /// ENGINE QUEUE ONLY. The reconnect loop exhausted its attempt cap.
    private func giveUpReconnectLocked() {
        guard isReconnecting else { return }
        isReconnecting = false
        reconnectTask = nil
        // Tear the last (never-logged-in) session down so it doesn't linger
        // half-open — a give-up means every attempt failed, so `session` is a
        // dead-or-stuck fresh session with no live login. The app layer's
        // "Reconnect" affordance (`onReconnectStateChanged(.gaveUp)`) drives a
        // clean manual reconnect, which builds its own new model anyway.
        session.disconnect()
        emitStatus("Couldn't reconnect after \(maxReconnectAttempts) attempts.")
        emitReconnectState(.gaveUp)
    }

    /// Cancels any in-flight reconnect loop (spec §7: "manual Connect always
    /// cancels/supersedes the loop"). Called by `shutdown()` and — via the app
    /// layer's disconnect-before-connect — before a fresh manual connect. Sync
    /// on the engine queue so the caller can rely on no reconnect attempt
    /// launching after it returns.
    public func cancelReconnect() {
        engineQueue.sync {
            reconnectTask?.cancel()
            reconnectTask = nil
            isReconnecting = false
        }
    }

    private func emitReconnectState(_ state: ReconnectState) {
        DispatchQueue.main.async { [onReconnectStateChanged] in
            onReconnectStateChanged?(state)
        }
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
        // Auto-reconnect (spec §7): stop the backoff loop FIRST — set
        // `isReconnecting = false` and cancel the task on the engine queue
        // BEFORE `isShutDown` so any in-flight `runReconnectLoop` iteration
        // (already past its cancellation checks) that reaches
        // `reconnectAttemptLocked` sees `!isReconnecting`/`isShutDown` and
        // bails without creating a fresh session. `cancelReconnect()` is its
        // own `engineQueue.sync`, so it fully drains before the teardown below.
        cancelReconnect()
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
