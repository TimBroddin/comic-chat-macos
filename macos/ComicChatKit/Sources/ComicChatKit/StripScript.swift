import Foundation
import CoreGraphics

/// A user-authored JSON conversation, decoded and validated into a renderable
/// comic strip (`cc-dumpart --script`, Plan 2 follow-on Task 13). The JSON
/// schema:
///
/// ```json
/// {
///   "backdrop": "field.bgb",
///   "participants": [
///     {"nick": "Anna", "avatar": "anna.avb"},
///     {"nick": "Armando", "avatar": "armando.avb"}
///   ],
///   "lines": [
///     {"speaker": "Anna", "text": "Hello there!", "mode": "say", "to": ["Armando"]},
///     {"speaker": "Armando", "text": "Hmm, who is this?", "mode": "think"},
///     {"speaker": "Anna", "text": "psst... it's me", "mode": "whisper", "to": ["Armando"]}
///   ]
/// }
/// ```
///
/// - `backdrop` is optional; omit it for no backdrop call.
/// - `mode` is optional (default `"say"`); one of `say`, `think`, `whisper`,
///   `action` (mapped to `Strip.Mode.say/.think/.whisper/.action`).
/// - `to` is optional (default `[]`); each entry must be a participant nick
///   declared in `participants`.
/// - `avatar`/`backdrop` values: an absolute path is used as-is; a bare name
///   is resolved against the caller-supplied comicart directory (the file
///   extension is REQUIRED -- ".avb"/".bgb" are never appended automatically,
///   so the resolved path is always predictable from the JSON text alone).
public struct StripScript {
    /// One decode/validation/render-time failure. Every case names the exact
    /// bad input so callers (tests, the CLI) can act on the failure kind
    /// without parsing an error string.
    public enum ScriptError: Error, CustomStringConvertible, Equatable {
        /// The JSON file at `path` could not be read.
        case unreadableFile(path: String, underlying: String)
        /// The JSON at `path` failed to decode; `underlying` is
        /// `DecodingError`'s description.
        case invalidJSON(path: String, underlying: String)
        /// `participants` was empty.
        case noParticipants
        /// `lines` was empty.
        case noLines
        /// A line's `mode` string wasn't one of the accepted values.
        /// `valid` lists the accepted set for the error message.
        case unknownMode(mode: String, valid: [String])
        /// A line's `speaker`, or an entry in its `to` list, named a nick
        /// that was never declared in `participants`.
        case unknownNick(String)
        /// Loading an avatar (`kind == "avatar"`) or backdrop
        /// (`kind == "backdrop"`) at the resolved `path` failed.
        case artLoadFailed(kind: String, path: String)
        /// `Strip.addParticipant` returned -1 -- either a bad avatar file the
        /// engine itself rejected past the open, or the session table's fixed
        /// 32-participant limit (CC_SESSION_MAX_USERS) was exceeded.
        case participantLimitExceeded(nick: String)
        /// The composed strip had zero size (nothing rendered).
        case emptyStrip

        public var description: String {
            switch self {
            case .unreadableFile(let path, let underlying):
                return "cannot read script file \(path): \(underlying)"
            case .invalidJSON(let path, let underlying):
                return "invalid JSON in \(path): \(underlying)"
            case .noParticipants:
                return "script has no participants (need at least one)"
            case .noLines:
                return "script has no lines (need at least one)"
            case .unknownMode(let mode, let valid):
                return "unknown mode \"\(mode)\" (valid: \(valid.joined(separator: ", ")))"
            case .unknownNick(let nick):
                return "unknown participant nick \"\(nick)\" (not declared in \"participants\")"
            case .artLoadFailed(let kind, let path):
                return "failed to load \(kind) at \(path)"
            case .participantLimitExceeded(let nick):
                return "could not add participant \"\(nick)\" (session table full, max 32 participants)"
            case .emptyStrip:
                return "composed strip is empty (zero size)"
            }
        }
    }

    /// The accepted `mode` strings, in the order shown in error messages.
    public static let validModes = ["say", "think", "whisper", "action"]

    struct JSONRoot: Codable {
        var backdrop: String?
        var participants: [JSONParticipant]
        var lines: [JSONLine]
    }
    struct JSONParticipant: Codable {
        var nick: String
        var avatar: String
    }
    struct JSONLine: Codable {
        var speaker: String
        var text: String
        var mode: String?
        var to: [String]?
    }

    /// Resolved, validated fields ready to drive a `Strip`.
    struct ResolvedLine {
        var speaker: String
        var text: String
        var mode: Strip.Mode
        var to: [String]
    }

    let backdropPath: String?
    let participants: [(nick: String, avatarPath: String)]
    let lines: [ResolvedLine]

    /// Decode and validate the script at `url`. `comicartDir`, if given,
    /// resolves bare (non-absolute) avatar/backdrop names; if omitted, bare
    /// names resolve relative to the current working directory (Foundation's
    /// default `URL(fileURLWithPath:)` behavior).
    public init(contentsOf url: URL, comicartDir: String? = nil) throws {
        let data: Data
        do {
            data = try Data(contentsOf: url)
        } catch {
            throw ScriptError.unreadableFile(path: url.path, underlying: String(describing: error))
        }
        let root: JSONRoot
        do {
            root = try JSONDecoder().decode(JSONRoot.self, from: data)
        } catch {
            throw ScriptError.invalidJSON(path: url.path, underlying: String(describing: error))
        }
        try self.init(root: root, comicartDir: comicartDir)
    }

    init(root: JSONRoot, comicartDir: String?) throws {
        guard !root.participants.isEmpty else { throw ScriptError.noParticipants }
        guard !root.lines.isEmpty else { throw ScriptError.noLines }

        func resolve(_ name: String) -> String {
            if name.hasPrefix("/") { return name }
            guard let dir = comicartDir else { return name }
            return (dir as NSString).appendingPathComponent(name)
        }

        var nickSeen = Set<String>()
        var resolvedParticipants: [(nick: String, avatarPath: String)] = []
        for p in root.participants {
            let path = resolve(p.avatar)
            resolvedParticipants.append((nick: p.nick, avatarPath: path))
            nickSeen.insert(p.nick)
        }

        var resolvedLines: [ResolvedLine] = []
        for line in root.lines {
            guard nickSeen.contains(line.speaker) else {
                throw ScriptError.unknownNick(line.speaker)
            }
            let modeString = line.mode ?? "say"
            let mode: Strip.Mode
            switch modeString {
            case "say": mode = .say
            case "think": mode = .think
            case "whisper": mode = .whisper
            case "action": mode = .action
            default:
                throw ScriptError.unknownMode(mode: modeString, valid: Self.validModes)
            }
            let to = line.to ?? []
            for addressee in to {
                guard nickSeen.contains(addressee) else {
                    throw ScriptError.unknownNick(addressee)
                }
            }
            resolvedLines.append(ResolvedLine(speaker: line.speaker, text: line.text,
                                              mode: mode, to: to))
        }

        self.backdropPath = root.backdrop.map(resolve)
        self.participants = resolvedParticipants
        self.lines = resolvedLines
    }

    /// The rendered result: PNG bytes plus the dimensions/panel count the
    /// caller reports.
    public struct RenderResult {
        public let pngData: Data
        public let pixelWidth: Int
        public let pixelHeight: Int
        public let panelCount: Int32
    }

    /// Build the `Strip` (participants, backdrop, lines) and composite it onto
    /// a `CGCanvas` at `scale`, exactly like `cc-dumpart --strip`'s demo path.
    /// Requires a metrics canvas to already be installed via
    /// `cc_set_metrics_canvas` for deterministic layout, same as every other
    /// Strip-driving path in this package.
    public func render(scale: CGFloat = 2.0) throws -> RenderResult {
        let strip = try Strip()

        var ids: [String: Int32] = [:]
        for p in participants {
            let id = addParticipantChecked(strip: strip, nick: p.nick, avbPath: p.avatarPath)
            switch id {
            case .success(let value):
                ids[p.nick] = value
            case .failure(let error):
                throw error
            }
        }

        if let backdrop = backdropPath {
            do {
                try strip.setBackdrop(backdrop)
            } catch {
                throw ScriptError.artLoadFailed(kind: "backdrop", path: backdrop)
            }
        }

        for line in lines {
            // Validated in init: speaker/to nicks are guaranteed present in
            // `ids` (every declared participant got an id above).
            let speakerID = ids[line.speaker]!
            let addressees = line.to.map { ids[$0]! }
            try strip.addLine(speaker: speakerID, text: line.text, modes: line.mode,
                              addressees: addressees)
        }

        let (w, h) = strip.size
        guard w > 0, h > 0 else {
            throw ScriptError.emptyStrip
        }

        let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: scale)
        try strip.compose(onto: canvas)

        guard let png = canvas.pngData() else {
            throw ScriptError.emptyStrip
        }
        return RenderResult(pngData: png, pixelWidth: canvas.pixelWidth,
                            pixelHeight: canvas.pixelHeight, panelCount: strip.panelCount)
    }
}

/// Add a participant and translate failure into a specific `ScriptError`:
/// the engine's `cc_strip_add_participant` returns a bare -1 for both a bad
/// avatar file and a full session table (CC_SESSION_MAX_USERS = 32), so this
/// distinguishes them the only way available -- by first checking whether the
/// avatar file itself opens (AvatarFile's own error), then attributing any
/// remaining -1 to the participant limit.
private func addParticipantChecked(
    strip: Strip, nick: String, avbPath: String
) -> Result<Int32, StripScript.ScriptError> {
    do {
        let id = try strip.addParticipant(nick: nick, avbPath: avbPath)
        return .success(id)
    } catch {
        // Disambiguate: does the avatar file even open on its own? If not,
        // it's an art-load failure; if it does, addParticipant's -1 must be
        // the session table being full.
        if (try? AvatarFile(path: avbPath)) == nil {
            return .failure(.artLoadFailed(kind: "avatar", path: avbPath))
        }
        return .failure(.participantLimitExceeded(nick: nick))
    }
}
