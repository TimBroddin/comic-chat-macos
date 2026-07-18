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

    // MARK: Comic

    public var backdrop: String {
        get { defaults.string(forKey: Keys.backdrop) ?? "field" }
        nonmutating set { defaults.set(newValue, forKey: Keys.backdrop) }
    }

    private enum Keys {
        static let server = "connect.server"
        static let port = "connect.port"
        static let room = "connect.room"
        static let encoding = "connect.encoding"
        static let nick = "persona.nick"
        static let character = "persona.character"
        static let backdrop = "comic.backdrop"
    }
}
