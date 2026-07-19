import Foundation

/// Plan 4b Task 10: the on-disk save format for a Comic Chat conversation —
/// spec §5's deliberate deviation from the original's binary `.cml`/history
/// format. Since the transcript IS already the canonical event log
/// (`ChatSessionModel`'s per-room `RoomBox.transcript` — see that type's own
/// doc comment), saving is just encoding that log plus the config needed to
/// re-render it, and reopening is replaying the log through a fresh
/// `TranscriptRenderer` (this task's other half). This is a JSON transcript,
/// not a pixel snapshot: reopening RE-RENDERS rather than restoring a bitmap,
/// so it also survives a future rendering-engine change.
///
/// MULTI-ROOM ADAPTATION (coordinator ruling, Plan 4b Task 7 carried
/// forward): `rooms[String: RoomBox]` holds one transcript per joined room,
/// but a save captures exactly ONE room's conversation — matching the
/// original's one-document-per-save posture (`ChatSessionModel.conversationFile()`
/// snapshots the ACTIVE room only). `room` names which room `events` came
/// from.
public struct ConversationFile: Codable, Sendable, Equatable {
    /// Bumped on any incompatible shape change. `1` for this task's shape.
    public var formatVersion: Int
    public var host: String
    public var room: String
    public var nick: String
    /// Bare comicart name (no directory/extension) — the character this
    /// session's SELF participant was seeded with at save time (mirrors
    /// `ChatSessionModel.initialCharacterName`, NOT necessarily the
    /// mid-session-switched `config.characterName`: a reopen replays `events`
    /// in order, and any in-session `.appearsAs` self-switch is already
    /// recorded there — see that property's own doc comment for why seeding
    /// with the INITIAL character is the one that keeps pre-switch panels
    /// correct on replay).
    public var characterName: String
    public var backdropName: String
    /// `WireEncoding.rawValue` (0 = CP-1252, 1 = UTF-8) — plain `Int32` rather
    /// than `WireEncoding` itself so the file format doesn't depend on that
    /// enum's own `Codable` conformance (or lack of one) and stays stable if
    /// `WireEncoding` ever changes shape.
    public var encodingRaw: Int32
    public var events: [ProtocolEvent]

    public init(formatVersion: Int = 1, host: String, room: String, nick: String,
                characterName: String, backdropName: String, encodingRaw: Int32,
                events: [ProtocolEvent]) {
        self.formatVersion = formatVersion
        self.host = host
        self.room = room
        self.nick = nick
        self.characterName = characterName
        self.backdropName = backdropName
        self.encodingRaw = encodingRaw
        self.events = events
    }

    /// Decode a `ConversationFile` from `url` (the format written by `write(to:)`).
    public static func read(from url: URL) throws -> ConversationFile {
        let data = try Data(contentsOf: url)
        return try JSONDecoder().decode(ConversationFile.self, from: data)
    }

    /// Encode this file as pretty-printed JSON and write it to `url`
    /// (`.json`, per the brief's `NSSavePanel` content type). Pretty-printed
    /// so a saved transcript is human-readable/diffable, matching the "it's
    /// just JSON" spirit of spec §5's deviation.
    public func write(to url: URL) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(self)
        try data.write(to: url, options: .atomic)
    }
}

extension ChatSessionModel {
    /// Snapshots the config + the ACTIVE room's transcript into a
    /// `ConversationFile` (Plan 4b Task 10) — the File > Save Transcript…
    /// entry point. Reads `transcript` (the active room's event log,
    /// already thread-safe via `engineQueue.sync`) and `currentRoom`
    /// (likewise), plus the config fields recorded at `init` time.
    ///
    /// `characterName` is `initialCharacterName`-shaped (the character this
    /// session's self participant was actually SEEDED with), not whatever
    /// `changeCharacter` may have since moved `config.characterName` to — see
    /// `ConversationFile.characterName`'s own doc comment for why: a reopen
    /// replays `events` from scratch, and any mid-session character switch is
    /// already IN that event log as a `.appearsAs` entry at its correct
    /// position (Plan 4b Task 5 fix round 1's transcript-doctrine fix).
    /// Seeding the replay with the CURRENT (possibly post-switch) character
    /// instead would repaint every pre-switch panel with the wrong avatar,
    /// exactly the bug that fix closed for live reflow — this accessor keeps
    /// the same discipline for the save/reopen path. `saveConfigSnapshot`
    /// exposes precisely that value for this purpose.
    public func conversationFile() -> ConversationFile {
        let snapshot = saveConfigSnapshot
        return ConversationFile(
            host: snapshot.host, room: currentRoom, nick: snapshot.nick,
            characterName: snapshot.characterName, backdropName: snapshot.backdropName,
            encodingRaw: snapshot.encoding.rawValue, events: transcript)
    }
}
