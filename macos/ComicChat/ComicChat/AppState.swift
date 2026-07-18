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

    public func connect() async {
        let artDir = Bundle.main.resourceURL!.appendingPathComponent("comicart").path
        let cfg = ChatConfig(host: settings.server, port: UInt16(settings.port),
                             nick: settings.nick, room: settings.room,
                             encoding: settings.encoding,
                             characterName: settings.character,
                             backdropName: settings.backdrop, artDir: artDir)
        let m = ChatSessionModel(config: cfg)
        m.onStripImage = { [weak self] img, size in
            Task { @MainActor in self?.stripImage = img; self?.stripSizePoints = size } }
        m.onMembers = { [weak self] nicks in Task { @MainActor in self?.members = nicks } }
        m.onStatus = { [weak self] s in Task { @MainActor in self?.statusLine = s } }
        model = m
        do { try await m.start(); showConnectSheet = false }
        catch { statusLine = "Connect failed: \(error)" }
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
    }
}
