import Foundation
import cchat_engine

/// Plan 3 Task 9 — the exit-milestone bridge: turns a stream of `ProtocolEvent`
/// (Task 7, the wire/protocol layer) into `cc_strip` calls (Plan 2, the comic
/// compositor), so a received annotated chat message renders as a comic panel.
/// This is the one place the two halves of the port meet.
///
/// THREADING / SERIALIZATION (binding, carried over from `Strip`'s and
/// `ProtocolSession`'s own contracts): `cc_strip_*` is a separate engine API
/// from `cc_session_*`, but BOTH share the same process-global engine state
/// (comicchat.h's single-thread contract covers every `cc_*` entry point).
/// `ProtocolStripBridge` itself does not call any `cc_session_*` function and
/// does not touch `ProtocolSession` at all — it only consumes already-decoded
/// Swift `ProtocolEvent` values and drives `Strip`.
///
/// The invariant this contract enforces is "one serial queue for every `cc_*`
/// call in the process", NOT phase separation. Plan 3 only ever exercised
/// drain-then-render (collect the full event stream first, THEN feed it to
/// this bridge — see `cc-dumpart --replay`), because that was the only
/// pattern proven safe at the time. Plan 4a Task 1's `EngineInterleaveTests`
/// proves the stronger, live-app-shaped pattern is ALSO safe: a caller may
/// apply events to this bridge WHILE a `ProtocolSession` is still connected
/// and mid-conversation, interleaved with that session's own
/// `cc_session_feed_bytes`/outbound calls — PROVIDED every one of those calls
/// (both the session's and this bridge's) is funneled through the SAME serial
/// queue. `ProtocolSession.performOnEngineQueue`/`enqueueEngineWork` expose
/// that queue (as "the engine queue") for exactly this purpose; a caller
/// driving both a session and a bridge should construct the bridge and call
/// `apply`/`compose` only via one of those two methods, never directly from
/// an arbitrary thread or a second queue of its own (two independently
/// serial queues each serialize their own calls but do nothing to prevent a
/// `cc_strip_*` call on one from running concurrently with a `cc_session_*`
/// call on the other — concurrency across the engine's shared statics is the
/// actual hazard, not which "half" of the API is being called).
///
/// The one remaining hard rule: never call `apply`/`compose` synchronously
/// from inside the `on_event` C-callback stack (i.e. from within
/// `ProtocolSession`'s `handleEvent`/`emit`, or synchronously from a
/// `session.events` consumer closure that is itself still on the engine
/// queue's call stack) — that queue is already blocked running the
/// `cc_session_*` call that triggered the event, so `performOnEngineQueue`'s
/// `sync` would deadlock (its `dispatchPrecondition` traps rather than
/// hanging). Event consumers must run on their own `Task` (reading
/// `session.events`, an `AsyncStream`, off the engine queue) and hop onto the
/// engine queue from there — the shape `EngineInterleaveTests` exercises.
public final class ProtocolStripBridge {
    /// Resolves an `.appearsAs` avatar name (or an unknown/never-announced
    /// participant) to a concrete `.avb` path. Defaults to cycling through a
    /// small built-in set of fixture-shaped avatars so every participant gets
    /// SOME avatar even with no art directory configured; callers with a real
    /// comicart directory should supply `AvatarResolver(comicartDir:)`.
    public struct AvatarResolver {
        /// Bare avatar file names (no directory), tried in order for
        /// unannounced participants — cycles if there are more participants
        /// than names.
        public var defaultOrder: [String]
        /// Directory bare names resolve against. `nil` means bare names are
        /// used as-is (caller's CWD-relative resolution, matching
        /// `StripScript`'s own convention when `comicartDir` is omitted).
        public var comicartDir: String?
        /// Additional directories searched BEFORE `comicartDir` (Plan 4b
        /// Task 6, D1 §4.3's search order) — user-downloaded/user-provided
        /// characters shadow the bundled comicart set. Checked in order;
        /// first directory containing the resolved bare filename wins. Empty
        /// by default (pre-Task-6 behavior: `comicartDir` alone).
        public var extraDirs: [String]

        public init(comicartDir: String? = nil,
                    defaultOrder: [String] = ["anna.avb", "armando.avb"],
                    extraDirs: [String] = []) {
            self.comicartDir = comicartDir
            self.defaultOrder = defaultOrder
            self.extraDirs = extraDirs
        }

        mutating func resolve(avatarName: String?) -> String {
            let name = avatarName.flatMap { $0.isEmpty ? nil : $0 }
            if let name {
                // `.appearsAs`'s avatarName has no required extension on the
                // wire; the bundled comicart set uses "<name>.avb" — append it
                // only if the caller-supplied name doesn't already end in
                // ".avb" (an explicit path segment naming its own file wins).
                let bare = name.lowercased().hasSuffix(".avb") ? name : "\(name).avb"
                if let resolved = existingPath(forBare: bare, allowCWDRelative: true) {
                    return resolved
                }
                // Plan 4b live-fix: an announced name that resolves to NO real
                // art on disk (e.g. the `_NoArt` sentinel a peer with no avatar
                // of its own sends, or any character we don't have locally and
                // couldn't download) must NOT be handed to `addParticipant` as
                // a literal path — `cc_strip_add_participant`'s `LoadAvatar`
                // would fail and the whole event would throw. The ORIGINAL
                // never treats an unknown announced name as an error:
                // `ChangeAvatarEntry::Execute` (histent.cpp:384) resolves via
                // `GetAvatar3(name, pui, bRandomIfNotFound=TRUE)`, whose
                // `LoadAvatar` miss cycles a default avatar via
                // `GetNextAvatarName` (avatar.cpp:699-704). Mirror that: fall
                // through to the same default-cycling path a nil name uses, so
                // the participant still gets SOME avatar and the line renders.
                return cycleDefault()
            }
            return cycleDefault()
        }

        /// The default-cycling fallback (a nil announced name, or a non-nil one
        /// that resolved to no real file). Advances `nextDefaultIndex` so
        /// successive unannounced/unresolvable participants get distinct
        /// defaults — mirrors `GetNextAvatarName`'s round-robin (avatar.cpp).
        private mutating func cycleDefault() -> String {
            let bare = defaultOrder[nextDefaultIndex % defaultOrder.count]
            nextDefaultIndex += 1
            if bare.hasPrefix("/") { return bare }
            // A default name is normally a real fixture/comicart path; resolve
            // it through the same search order for consistency, but fall back
            // to the bare form (the pre-fix behavior for defaults) if it isn't
            // found anywhere — a caller with a genuinely-present default gets
            // the located path; a misconfigured one gets what it always got.
            return existingPath(forBare: bare)
                ?? comicartDir.map { ($0 as NSString).appendingPathComponent(bare) }
                ?? bare
        }

        /// The bare name resolved to an EXISTING file over `extraDirs` then
        /// `comicartDir` (D1 §4.3 search order), or `nil` if it exists nowhere.
        /// An absolute bare path is honored as-is when it exists.
        ///
        /// `allowCWDRelative` (named-avatar resolution only): when no directory
        /// locates the name, also probe the bare form CWD-relative — this
        /// preserves the documented `comicartDir == nil` convention ("bare
        /// names are used as-is, caller's CWD-relative resolution") for a named
        /// avatar the caller genuinely staged next to its CWD, while still
        /// answering `nil` for a name (like `_NoArt`) present nowhere at all.
        private func existingPath(forBare bare: String, allowCWDRelative: Bool = false) -> String? {
            if bare.hasPrefix("/") {
                return FileManager.default.fileExists(atPath: bare) ? bare : nil
            }
            // extraDirs BEFORE comicartDir (D1 §4.3): a user-downloaded
            // character with the same bare name as a bundled one shadows it.
            for dir in extraDirs {
                let candidate = (dir as NSString).appendingPathComponent(bare)
                if FileManager.default.fileExists(atPath: candidate) {
                    return candidate
                }
            }
            if let dir = comicartDir {
                let candidate = (dir as NSString).appendingPathComponent(bare)
                if FileManager.default.fileExists(atPath: candidate) { return candidate }
            }
            if allowCWDRelative, FileManager.default.fileExists(atPath: bare) {
                return bare
            }
            return nil
        }

        /// Pure path-existence check over `extraDirs + [comicartDir]`,
        /// mirroring `resolve(avatarName:)`'s own bare-name rule exactly
        /// (append ".avb" unless `name` already ends in it, case-insensitive)
        /// — Plan 4b Task 6's "did this name resolve to real art vs a cycled
        /// default" answer. Unlike `resolve`, this is non-mutating (no
        /// `defaultOrder` cycling — an unresolved name never consumes a
        /// default-cycle slot just by being CHECKED) and never falls back to
        /// a bundled default: a name that resolves to no real file on disk
        /// answers `false`, full stop (the caller's cue to attempt a
        /// download, not to accept a cycled placeholder as "known").
        public func resolvesName(_ name: String) -> Bool {
            guard !name.isEmpty else { return false }
            let bare = name.lowercased().hasSuffix(".avb") ? name : "\(name).avb"
            if bare.hasPrefix("/") {
                return FileManager.default.fileExists(atPath: bare)
            }
            for dir in extraDirs {
                let candidate = (dir as NSString).appendingPathComponent(bare)
                if FileManager.default.fileExists(atPath: candidate) { return true }
            }
            guard let comicartDir else { return false }
            let candidate = (comicartDir as NSString).appendingPathComponent(bare)
            return FileManager.default.fileExists(atPath: candidate)
        }

        private var nextDefaultIndex = 0
    }

    public struct BridgeError: Error, CustomStringConvertible {
        public let message: String
        public var description: String { message }
    }

    private let strip: Strip
    private var resolver: AvatarResolver
    private let encoding: WireEncoding

    /// nick -> assigned cc_strip participant id (>=1).
    public private(set) var participantIDs: [String: Int32] = [:]
    /// nick -> most recently announced avatar name from an `.appearsAs`,
    /// whether seen before OR after that nick became a participant. An
    /// announcement arriving before participant creation is picked up by
    /// `ensureParticipant`'s initial `addParticipant`; one arriving after
    /// immediately switches the existing participant's avatar via
    /// `Strip.setParticipantAvatar` (Plan 4a Task 6) — existing panels keep
    /// the old avatar (no retro-recompose), only later lines get the new one.
    public private(set) var announcedAvatarNames: [String: String] = [:]
    /// Every participant nick this bridge has assigned an id for, in the
    /// order they were first added — exposed for tests/observability and for
    /// CLI callers (`cc-dumpart --replay`) to report what was rendered.
    public private(set) var participantOrder: [String] = []
    /// nick -> the avatar basename (e.g. "anna.avb", the last path component
    /// of whatever `.avb` the resolver actually returned) currently loaded
    /// for that participant, whether that came from a real `.appearsAs`
    /// announcement or a cycled default the resolver picked for a nick that
    /// was never announced (Plan 4b live-fix: "eager member avatars at
    /// join"). Set at the same two sites that hand a resolved path to the
    /// engine -- `ensureParticipant`'s initial `addParticipant` and
    /// `.appearsAs`'s `setParticipantAvatar` switch -- so it always reflects
    /// the CURRENTLY assigned avatar, same posture as `announcedAvatarNames`.
    /// This is the middle icon-fallback tier `ChatSessionModel.emitMembers`
    /// needs for a member who has never spoken or announced: the ORIGINAL
    /// (`AssignArbitraryAvatar`, protsupp.cpp:470-477, called from
    /// `CIUserJoin`, protsupp.cpp:574-575) assigns every member SOME avatar
    /// at join/NAMES time, not lazily on first speech -- so its member list
    /// always has an icon to show. `resolvedAvatarName` -> `assignedAvatarNames`
    /// is this bridge's equivalent record.
    public private(set) var assignedAvatarNames: [String: String] = [:]

    /// `strip` is driven from `nil` (creates a fresh `Strip`) or an existing
    /// one (e.g. so a caller can set a backdrop first). `resolver` maps
    /// `.appearsAs` avatar names (and defaults for participants never
    /// announced) to `.avb` paths.
    public init(strip: Strip? = nil, resolver: AvatarResolver = AvatarResolver(),
                encoding: WireEncoding = .cp1252) throws {
        self.strip = try strip ?? Strip()
        self.resolver = resolver
        self.encoding = encoding
    }

    /// Set the strip's backdrop. Must be called before the first `addLine`
    /// producing a panel for the backdrop to appear in it (mirrors
    /// `Strip.setBackdrop`'s own ordering requirement).
    public func setBackdrop(_ bgbPath: String) throws {
        try strip.setBackdrop(bgbPath)
    }

    /// Passthrough to the bridge's own `resolver.resolvesName` (Plan 4b
    /// Task 6) — `resolver` itself is `private` (an implementation detail of
    /// `apply`'s avatar resolution), so callers that need the "did this name
    /// resolve to real art vs a cycled default" answer (`ChatSessionModel`'s
    /// auto-download gate) go through this method rather than reaching past
    /// the bridge's encapsulation.
    public func resolvesName(_ name: String) -> Bool {
        resolver.resolvesName(name)
    }

    /// Feed one decoded protocol event. Events that don't map to a strip
    /// action (room state, WHOIS/WHO replies, errors, ...) are silently
    /// ignored — this bridge only cares about the subset that drives a comic
    /// panel: membership (for participants) and messages (for lines).
    public func apply(_ event: ProtocolEvent) throws {
        switch event {
        case .selfJoined:
            // No nick carried on this event (channel-scoped only) — the
            // participant for "self" is created lazily the first time a
            // .text/.userJoined names them (matches how ProtocolSession
            // itself only learns "own nick" from .loggedIn, a SEPARATE event
            // this bridge doesn't special-case since it carries the nick
            // directly via .userJoined/.text like any other participant).
            break

        case .userJoined(let nick, _):
            try ensureParticipant(nick)

        case .appearsAs(let nick, let avatarName, _):
            // Plan 4a Task 6: cc_strip now HAS a "change avatar" entry point
            // (cc_strip_set_participant_avatar), so an EXISTING participant's
            // avatar is switched immediately -- resolved through the same
            // `resolver` ensureParticipant uses, so an explicit avatarName
            // resolves the same way it would have if seen before creation. A
            // not-yet-created participant still just stashes the name (there
            // is no id to switch yet); ensureParticipant picks it up as usual.
            announcedAvatarNames[nick] = avatarName
            if let id = participantIDs[nick] {
                let avatarPath = resolver.resolve(avatarName: avatarName)
                try strip.setParticipantAvatar(id, avbPath: avatarPath)
                assignedAvatarNames[nick] = (avatarPath as NSString).lastPathComponent
            }

        case .text(let nick, _, _, let text, let kind, let annotations):
            let speaker = try ensureParticipant(nick)
            let modes = Self.stripModes(kind: kind, annotations: annotations)
            let addressees = try resolveAddressees(annotations?.addressees ?? [])
            try strip.addLineCooked(speaker: speaker, text: text, modes: modes,
                                    addressees: addressees, annotations: annotations,
                                    encoding: encoding)

        case .names(_, let nicks):
            // Plan 4b live-fix (Tim's screenshot report): the ORIGINAL
            // assigns every member an avatar at JOIN/NAMES time
            // (`CIUserJoin` -> `AssignArbitraryAvatar`, protsupp.cpp:574-575,
            // driven per-NAMES-nick via `bSingleJoin`/`JoinEntry::Execute`,
            // ircsock.cpp:2508-2539 / histent.cpp:259-265), not lazily on
            // first speech -- so its member list always shows an icon for
            // silent members too. Mirror that here: eagerly ensureParticipant
            // every listed nick. NAMES prefixes op/voice with '@'/'+' (same
            // grammar `ProtocolSession`'s own `.names` handling strips,
            // ProtocolSession.swift:1149-1157) -- strip it so the bare nick
            // matches what `.userJoined`/`.text` events for the same person
            // use as their dictionary key. Handling this HERE (rather than
            // only in `ChatSessionModel`) means a room's `.names` entry in
            // its transcript replays through this SAME path on reflow/
            // room-switch (`rebuildStripLocked`'s replay loop), so a
            // background room's eventual activation reproduces the identical
            // assignment deterministically -- no separate replay logic
            // needed. Self is not special-cased: `preRegisterSelfParticipant`
            // has already claimed self's nick by the time any `.names` event
            // can arrive (`ChatSessionModel.setUpStripLocked` registers self
            // before the bridge ever sees an event), so `ensureParticipant`
            // for self's own nick here is the same safe no-op it already is
            // for a self-authored `.userJoined` echo.
            let bareNicks = nicks.map { nick -> String in
                var bare = nick
                if bare.hasPrefix("@") || bare.hasPrefix("+") { bare.removeFirst() }
                return bare
            }.filter { !$0.isEmpty }
            ensureParticipants(bareNicks)

        case .whisper(let nick, _, let text, let annotations):
            let speaker = try ensureParticipant(nick)
            let addressees = try resolveAddressees(annotations?.addressees ?? [])
            try strip.addLineCooked(speaker: speaker, text: text, modes: .whisper,
                                    addressees: addressees, annotations: annotations,
                                    encoding: encoding)

        case .action(let nick, let text, let annotations):
            let speaker = try ensureParticipant(nick)
            let addressees = try resolveAddressees(annotations?.addressees ?? [])
            try strip.addLineCooked(speaker: speaker, text: text, modes: .action,
                                    addressees: addressees, annotations: annotations,
                                    encoding: encoding)

        default:
            // Everything else (room state, list/who/whois replies, errors,
            // nick changes, parts/quits, ...) has no comic-panel rendering in
            // this milestone's scope — silently ignored, not an error.
            break
        }
    }

    /// Feed a whole collected event stream in order (the shape `cc-dumpart
    /// --replay` and the exit-milestone test both use).
    public func apply<S: Sequence>(_ events: S) throws where S.Element == ProtocolEvent {
        for event in events {
            try apply(event)
        }
    }

    /// Compose the strip built so far onto `canvas`.
    public func compose(onto canvas: Canvas) throws {
        try strip.compose(onto: canvas)
    }

    /// Bounding box of the finished page in twips.
    public var size: (width: Int32, height: Int32) { strip.size }

    /// Number of panels laid out so far.
    public var panelCount: Int32 { strip.panelCount }

    // MARK: - participant lifecycle

    /// Registers a participant id this bridge did NOT itself assign — the
    /// seam `ChatSessionModel` (Plan 4a Task 9) uses for the SELF
    /// participant, which it must add directly via `Strip.addParticipant` +
    /// `Strip.setSelf` (so `setSelf` sees the id at construction time,
    /// ordering `addParticipant`/`ensureParticipant` doesn't guarantee) yet
    /// still wants tracked here: without this call, the first `.userJoined`
    /// or `.text` event carrying the SAME nick (an IRC server typically
    /// echoes the joining client's own JOIN/NAMES like any other member)
    /// would fall into `ensureParticipant`'s "not seen yet" branch and add a
    /// SECOND, distinct participant for the same person. Idempotent no-op if
    /// `nick` is already registered (keeps whichever id was assigned first).
    public func preRegisterSelfParticipant(nick: String, id: Int32) {
        guard participantIDs[nick] == nil else { return }
        participantIDs[nick] = id
        participantOrder.append(nick)
    }

    @discardableResult
    private func ensureParticipant(_ nick: String) throws -> Int32 {
        if let existing = participantIDs[nick] { return existing }
        let avatarPath = resolver.resolve(avatarName: announcedAvatarNames[nick])
        let id = try strip.addParticipant(nick: nick, avbPath: avatarPath)
        participantIDs[nick] = id
        participantOrder.append(nick)
        assignedAvatarNames[nick] = (avatarPath as NSString).lastPathComponent
        return id
    }

    /// Public seam for eagerly creating a participant for every nick in
    /// `nicks` (Plan 4b live-fix) -- the bridge's own `ensureParticipant` is
    /// `private` (an implementation detail of `apply`'s event routing), so a
    /// caller that needs to force eager creation from a nick list that isn't
    /// itself a `.names`/`.userJoined` `ProtocolEvent` (or wants to do so
    /// without going through `apply`) uses this instead. `.names`'s own
    /// handling in `apply` above calls this internally, so most callers never
    /// need to call it directly -- it's exposed primarily for
    /// `ChatSessionModel`/tests that want the eager-creation behavior as a
    /// standalone step. Nicks already participants are silently skipped
    /// (idempotent, same as any individual `ensureParticipant` call); a
    /// resolver failure for one nick does not abort the rest -- unlike
    /// `apply`, which propagates a throw for its OWN triggering event, this
    /// is a best-effort bulk operation over a list the caller doesn't want a
    /// single bad entry to abort (mirrors the original's behavior: a NAMES
    /// list is processed nick-by-nick, `bSingleJoin`, ircsock.cpp:270-276,
    /// with no single point where one bad entry could stop the rest).
    public func ensureParticipants(_ nicks: [String]) {
        for nick in nicks {
            _ = try? ensureParticipant(nick)
        }
    }

    private func resolveAddressees(_ nicks: [String]) throws -> [Int32] {
        try nicks.map { try ensureParticipant($0) }
    }

    // MARK: - mode mapping

    /// `CC_EV_TEXT`'s `kind` (MT_*, the message-type classification from the
    /// engine's payload stage) plus a cooked annotation's own `mode` (SM_*,
    /// defines.h:57-61 — SM_SAY=1, SM_WHISPER=2, SM_THINK=3, SM_SHOUT=4,
    /// SM_ACTION=5) both describe "how was this said" — but
    /// `cc_strip_add_line{,_cooked}`'s `modes` parameter wants a `CC_MODE_*`
    /// bitmask (SAY/WHISPER/THINK/ACTION), matching `Strip.Mode`. Prefer the
    /// annotation's decoded `mode` field when present (it is the wire's own
    /// say-mode classification, authoritative when available); fall back to
    /// a neutral `.say` for a plain (unannotated) `.text` event, since
    /// CC_EV_TEXT's `kind` is a passthrough int with no Swift-side SM_*/BM_*
    /// mapping table in this task's scope (ProtocolEvents.swift's own doc
    /// comment: "Swift doesn't need to interpret them to route the event,
    /// only to display it"). The SM_* -> CC_MODE_* mapping mirrors SM2BM
    /// exactly (protsupp.cpp:125-138): WHISPER/THINK/ACTION map to their own
    /// bit; everything else (SAY, and SHOUT -- there is no BM_SHOUT bit) maps
    /// to SAY, same as the original's `default: return BM_SAY`.
    static func stripModes(kind: MessageKind, annotations: Annotations?) -> Strip.Mode {
        guard let annotations else { return .say }
        switch annotations.mode {
        case 2: return .whisper  // SM_WHISPER
        case 3: return .think    // SM_THINK
        case 5: return .action   // SM_ACTION
        default: return .say     // SM_SAY, SM_SHOUT (no BM_SHOUT bit)
        }
    }
}
