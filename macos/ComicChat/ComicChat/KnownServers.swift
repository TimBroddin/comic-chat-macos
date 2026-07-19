import Foundation

/// A curated, hand-picked entry in the connect sheet's known-servers list
/// (UI-improvement pass: "make the server dialog a bit prettier — include
/// known Comic Chat servers etc."). This is a small static list, not a
/// fetched directory — every entry below was probed live against the real
/// server during this pass's research, not sourced blind from an old list.
///
/// `protocolNote` is a short tag shown as a capsule badge in the picker row
/// (e.g. "IRCX" vs "IRC") — purely informational, it does not change how
/// `AppState.connect()` behaves; the wire-protocol negotiation is unchanged
/// and works the same regardless of which kind of server answers.
public struct KnownServer: Identifiable, Hashable {
    public var id: String { host + ":\(port)" }
    public let name: String
    public let host: String
    public let port: Int
    public let room: String
    public let blurb: String
    public let protocolNote: String

    public init(name: String, host: String, port: Int, room: String, blurb: String, protocolNote: String) {
        self.name = name
        self.host = host
        self.port = port
        self.room = room
        self.blurb = blurb
        self.protocolNote = protocolNote
    }
}

public enum KnownServers {
    /// The curated list itself. Order matches the brief: The Crypt (the
    /// original MS Chat protocol, full IRCX) first, then the two plain-IRC
    /// community servers, then — DEBUG builds only — this machine's local
    /// capture rig for offline development.
    public static let all: [KnownServer] = {
        var list = [
            KnownServer(
                name: "The Crypt",
                host: "www.crypthome.com",
                port: 6667,
                room: "#Crypt",
                blurb: "OfficeIRC, full IRCX (the original MS Chat protocol). Small memorial server — be a good guest.",
                protocolNote: "IRCX"
            ),
            KnownServer(
                name: "Comic Chat Network",
                host: "comic.dedoky.com",
                port: 6667,
                room: "#garage",
                blurb: "Community plain-IRC server. Nicknames max 9 characters.",
                protocolNote: "IRC"
            ),
            KnownServer(
                name: "Koach.com",
                host: "chat1.koach.com",
                port: 6667,
                room: "#koachsworkshop",
                blurb: "Long-running community server.",
                protocolNote: "IRC"
            ),
        ]
        #if DEBUG
        list.append(
            KnownServer(
                name: "Local rig",
                host: "127.0.0.1",
                port: 6668,
                room: "#comicrig",
                blurb: "This machine's capture rig proxy (when running).",
                protocolNote: "dev"
            )
        )
        #endif
        return list
    }()

    /// Finds a known entry whose host/port match `host`/`port` exactly
    /// (case-insensitive host compare) — used by `ConnectSheet` to decide
    /// whether free-typed server/port fields still correspond to a picker
    /// selection, or whether the user has wandered off into "Custom Server"
    /// territory.
    public static func match(host: String, port: Int) -> KnownServer? {
        all.first { $0.host.caseInsensitiveCompare(host) == .orderedSame && $0.port == port }
    }
}
