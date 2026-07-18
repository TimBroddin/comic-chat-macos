import Foundation
import Observation
import CoreGraphics
import ComicChatKit

@Observable @MainActor
public final class AppState {
    public var model: ChatSessionModel?
    public var settings = SettingsStore()
    public var showConnectSheet = true
    public var statusLine = ""
    public var members: [String] = []
    public var stripImage: CGImage?
    public var stripSizePoints: CGSize = .zero
    /// The self avatar's live pose preview (Plan 4b Task 3) — updated after
    /// every emotion-wheel drag or typing-preview via `ChatSessionModel.onSelfPose`.
    public var selfPoseImage: CGImage?

    /// Kept alive for the process's whole replay session — `FixtureReplayServer`
    /// services exactly one connection, and its `NWListener`/`NWConnection`
    /// are torn down if this reference drops (Task 12's offline demo hook).
    private var replayServer: FixtureReplayServer?

    public func connect() async {
        // Final review (Plan 4a): tear down any existing session FIRST. Without
        // this, ⌘N -> Connect while already connected builds a SECOND
        // ChatSessionModel (with its own engine queue driving the same
        // process-global engine state -- a two-serial-queues hazard) and
        // drops the old `model` reference without ever calling its mandatory
        // `shutdown()`. `disconnect()` is idempotent (safe to call with no
        // active session).
        disconnect()

        let artDir = Bundle.main.resourceURL!.appendingPathComponent("comicart").path
        var cfg = ChatConfig(host: settings.server, port: UInt16(exactly: settings.port) ?? 6667,
                             nick: settings.nick, room: settings.room,
                             encoding: settings.encoding,
                             characterName: settings.character,
                             backdropName: settings.backdrop, artDir: artDir)

        // Offline demo hook (Task 12): `--replay-fixture <path>` starts a
        // FixtureReplayServer over the given capture-shaped .jsonl and
        // overrides host/port/nick/room to match it, so the MVP is
        // demonstrable with zero network and real 1998-client bytes. The
        // fixture's recorded nick/room MUST match what we connect with —
        // FixtureReplayServer does not rewrite them (see that type's doc
        // comment) — so this is hardcoded to the fixture actually used here
        // (`hand-authored-annotation.jsonl`: nick "Anon", room "#comicrig" —
        // chosen over `smoke-2-replay.jsonl` because it is the one fixture
        // that carries a peer's annotated/cooked-pose PRIVMSG, verified
        // empirically to render a second strip panel; the login-only fixture
        // renders just the title panel).
        if let path = replayFixturePath() {
            do {
                let server = try FixtureReplayServer(fixtureURL: URL(fileURLWithPath: path))
                try server.start()
                replayServer = server
                cfg.host = "127.0.0.1"
                cfg.port = server.port
                cfg.nick = "Anon"
                cfg.room = "#comicrig"
            } catch {
                statusLine = "Replay fixture failed to start: \(error)"
                showConnectSheet = true
                return
            }
        }

        let m = ChatSessionModel(config: cfg)
        m.onStripImage = { [weak self] img, size in
            Task { @MainActor in self?.stripImage = img; self?.stripSizePoints = size } }
        m.onMembers = { [weak self] nicks in Task { @MainActor in self?.members = nicks } }
        m.onStatus = { [weak self] s in Task { @MainActor in self?.statusLine = s } }
        m.onSelfPose = { [weak self] img in Task { @MainActor in self?.selfPoseImage = img } }
        model = m
        do { try await m.start(); showConnectSheet = false }
        catch { statusLine = "Connect failed: \(error)" }
    }

    /// Reads `--replay-fixture <path>` out of the process arguments, if
    /// present (a simple positional lookup — no need for a full argument
    /// parser for one demo-only flag).
    private func replayFixturePath() -> String? {
        let args = ProcessInfo.processInfo.arguments
        guard let flagIndex = args.firstIndex(of: "--replay-fixture"), flagIndex + 1 < args.count else {
            return nil
        }
        return args[flagIndex + 1]
    }

    /// Tears down the current session (Task 9 review: `shutdown()` is
    /// mandatory before dropping a `ChatSessionModel` — no `deinit`, so
    /// dropping one without calling this leaves a dangling engine global).
    /// Used by both the Room > Leave/Disconnect command and as the safety
    /// net before starting a fresh connection.
    public func disconnect() {
        model?.shutdown()
        model = nil
        members = []
        stripImage = nil
        stripSizePoints = .zero
        selfPoseImage = nil
        replayServer?.stop()
        replayServer = nil
    }
}
