import Foundation

/// Typed `UserDefaults` wrapper for the app's persisted connect/persona/comic
/// settings (Plan 4a Task 9). Every property is a plain computed accessor
/// over an injected `UserDefaults` instance — no caching, no notifications:
/// callers that need change observation should wrap this (or observe
/// `UserDefaults` directly) themselves. Injectable so tests can point at an
/// isolated suite (`UserDefaults(suiteName:)`) instead of `.standard`,
/// matching the brief's Step 1 round-trip test.
public struct SettingsStore {
    private let defaults: UserDefaults

    public init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    // MARK: Connect

    public var server: String {
        get { defaults.string(forKey: Keys.server) ?? "" }
        nonmutating set { defaults.set(newValue, forKey: Keys.server) }
    }

    /// Default 6667. Stored as an `Int` (UserDefaults has no fixed-width
    /// integer API); `ChatConfig.port` callers narrow to `UInt16` themselves.
    public var port: Int {
        get {
            // `object(forKey:)` distinguishes "never set" (nil -> default)
            // from an explicitly-stored 0, which `integer(forKey:)` alone
            // cannot (it returns 0 for both).
            guard defaults.object(forKey: Keys.port) != nil else { return 6667 }
            return defaults.integer(forKey: Keys.port)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.port) }
    }

    public var room: String {
        get { defaults.string(forKey: Keys.room) ?? "" }
        nonmutating set { defaults.set(newValue, forKey: Keys.room) }
    }

    public var encoding: WireEncoding {
        get {
            guard defaults.object(forKey: Keys.encoding) != nil else { return .cp1252 }
            let raw = Int32(defaults.integer(forKey: Keys.encoding))
            return WireEncoding(rawValue: raw) ?? .cp1252
        }
        nonmutating set { defaults.set(Int(newValue.rawValue), forKey: Keys.encoding) }
    }

    // MARK: Persona

    public var nick: String {
        get { defaults.string(forKey: Keys.nick) ?? "" }
        nonmutating set { defaults.set(newValue, forKey: Keys.nick) }
    }

    public var character: String {
        get { defaults.string(forKey: Keys.character) ?? "anna" }
        nonmutating set { defaults.set(newValue, forKey: Keys.character) }
    }

    /// Plan 4b Task 5: the USER command's `<realname>` field (persona
    /// plumbing — `ProtocolSession.init`'s new `realName:` parameter reads
    /// this via `AppState.connect`'s `ChatConfig` build). Empty by default
    /// (falls back to nick, matching the pre-Task-5 behavior when unset).
    public var realName: String {
        get { defaults.string(forKey: Keys.realName) ?? "" }
        nonmutating set { defaults.set(newValue, forKey: Keys.realName) }
    }

    /// Plan 4b Batch C: the free-text "profile" a peer's `# GetInfo` probe
    /// receives back (`cc_session_send_info_reply`'s `profile_text`,
    /// `ChatConfig.profileText`). Empty by default — an empty profile still
    /// gets an honest reply (see `ChatSessionModel`'s `.infoRequest` handler
    /// doc comment for the `ID_DEFAULT_PROFILE` archaeology gap this default
    /// stands in for), it is simply an empty "# HeresInfo: " body.
    public var profileText: String {
        get { defaults.string(forKey: Keys.profileText) ?? "" }
        nonmutating set { defaults.set(newValue, forKey: Keys.profileText) }
    }

    // MARK: Comic

    public var backdrop: String {
        get { defaults.string(forKey: Keys.backdrop) ?? "field" }
        nonmutating set { defaults.set(newValue, forKey: Keys.backdrop) }
    }

    public var comicMode: Bool {
        get {
            guard defaults.object(forKey: Keys.comicMode) != nil else { return true }
            return defaults.bool(forKey: Keys.comicMode)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.comicMode) }
    }

    /// Quick-wins batch item 3 (original `UnitsWide`, pageview.cpp): a
    /// user-forced panels-per-row column count, `0` (default) meaning
    /// "automatic" — `PanelFit.columns(forViewportWidthTwips:)`'s own
    /// viewport-fit arithmetic, unchanged. `1...5` matches the original's
    /// `FitPanelsWide` cap (`PanelFit.columns`'s own `1...5` scan range) —
    /// forcing a value outside that range would ask `ChatSessionModel
    /// .setViewport` for a column count `PanelFit.unitPanelTwips` was never
    /// exercised at, so the Settings Picker only ever writes 0...5 here.
    public var panelsPerRow: Int {
        get {
            guard defaults.object(forKey: Keys.panelsPerRow) != nil else { return 0 }
            return defaults.integer(forKey: Keys.panelsPerRow)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.panelsPerRow) }
    }

    // MARK: Protocol (Plan 4b Task 5)

    /// Gates `ChatSessionModel.send`'s outbound cooked pose annotations: when
    /// `false`, `send(_:mode:)` passes `annotations: nil` (peers then run
    /// text inference — the original's ComicsData toggle semantics). The
    /// wheel/preview still work locally either way.
    public var sendComicsData: Bool {
        get {
            guard defaults.object(forKey: Keys.sendComicsData) != nil else { return true }
            return defaults.bool(forKey: Keys.sendComicsData)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.sendComicsData) }
    }

    /// Wires `ChatSessionModel`'s `_acceptWhispers` seam (Task 4's internal
    /// test-only default `true`) to a real user-facing setting.
    public var acceptWhispers: Bool {
        get {
            guard defaults.object(forKey: Keys.acceptWhispers) != nil else { return true }
            return defaults.bool(forKey: Keys.acceptWhispers)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.acceptWhispers) }
    }

    // MARK: Sounds (Plan 4b Task 5)

    public var soundsEnabled: Bool {
        get {
            guard defaults.object(forKey: Keys.soundsEnabled) != nil else { return true }
            return defaults.bool(forKey: Keys.soundsEnabled)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.soundsEnabled) }
    }

    /// Default: the app's Application Support directory (per-app
    /// subdirectory keyed by the bundle identifier, falling back to
    /// "ComicChat" for a non-bundled/test context where
    /// `Bundle.main.bundleIdentifier` is nil) — never empty, matching the
    /// brief's "default = the App Support path" requirement.
    public var soundsFolder: String {
        get { defaults.string(forKey: Keys.soundsFolder) ?? Self.defaultSoundsFolder }
        nonmutating set { defaults.set(newValue, forKey: Keys.soundsFolder) }
    }

    private static var defaultSoundsFolder: String {
        let fm = FileManager.default
        let base = (try? fm.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                appropriateFor: nil, create: false))
            ?? URL(fileURLWithPath: NSHomeDirectory() + "/Library/Application Support")
        let appDir = Bundle.main.bundleIdentifier ?? "ComicChat"
        return base.appendingPathComponent(appDir).appendingPathComponent("Sounds").path
    }

    // MARK: Art (Plan 4b Task 5)

    public var autoDownloadAvatars: Bool {
        get {
            guard defaults.object(forKey: Keys.autoDownloadAvatars) != nil else { return true }
            return defaults.bool(forKey: Keys.autoDownloadAvatars)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.autoDownloadAvatars) }
    }

    // MARK: Notifications (Plan 4b Batch B)

    /// Gates `AppState`'s mention/whisper `UNUserNotificationCenter` posts
    /// (`ChatSessionModel.onNotificationEvent`'s app-layer consumer). Default
    /// `true` — notifications are opt-OUT, matching every other Advanced-tab
    /// toggle's "on by default, disable if unwanted" posture
    /// (`sendComicsData`/`acceptWhispers`/`autoDownloadAvatars`).
    public var notificationsEnabled: Bool {
        get {
            guard defaults.object(forKey: Keys.notificationsEnabled) != nil else { return true }
            return defaults.bool(forKey: Keys.notificationsEnabled)
        }
        nonmutating set { defaults.set(newValue, forKey: Keys.notificationsEnabled) }
    }

    private enum Keys {
        static let server = "connect.server"
        static let port = "connect.port"
        static let room = "connect.room"
        static let encoding = "connect.encoding"
        static let nick = "persona.nick"
        static let character = "persona.character"
        static let realName = "persona.realName"
        static let profileText = "persona.profile"
        static let backdrop = "comic.backdrop"
        static let comicMode = "view.comicMode"
        static let panelsPerRow = "comic.panelsPerRow"
        static let sendComicsData = "protocol.sendComicsData"
        static let acceptWhispers = "protocol.acceptWhispers"
        static let soundsEnabled = "sounds.enabled"
        static let soundsFolder = "sounds.folder"
        static let autoDownloadAvatars = "art.autoDownloadAvatars"
        static let notificationsEnabled = "notifications.enabled"
    }
}
