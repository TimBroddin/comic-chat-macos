import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Auto-reconnect (spec §7): a ChatSessionModel that drops mid-session
// schedules a backoff reconnect, resurrects a fresh ProtocolSession against
// the SAME config.host/port, re-logs-in, re-joins EVERY room in join order,
// and keeps every room's transcript across the reconnect (strips rebuild from
// the retained logs).
//
// The LoopbackIRCServer here stays LISTENING across the drop: `dropPeer()`
// severs the accepted connection (the client sees EOF/error and reconnects),
// and the reconnect re-dials the same port where `newConnectionHandler`
// accepts the fresh session as a replacement peer. This sidesteps the
// TIME_WAIT rebind race a stop-then-resurrect-on-same-port approach hits.
//
// SERIALIZATION (same reasoning as ChatSessionModelTests/MultiRoomTests):
// ChatSessionModel drives BOTH cc_session_* and cc_strip_* against the same
// process-global engine (comicchat.h's single-thread contract), so this suite
// is nested inside EngineGlobalStateSelfTests (.serialized).

/// Thread-safe accumulator for `onReconnectStateChanged` deliveries (a
/// `@Sendable` main-thread callback, read from the test's own task — the same
/// lock-box shape as ChatSessionModelTests' `ImagesBox`).
private final class ReconnectStateBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [ReconnectState] = []
    func append(_ s: ReconnectState) { lock.lock(); defer { lock.unlock() }; storage.append(s) }
    func all() -> [ReconnectState] { lock.lock(); defer { lock.unlock() }; return storage }
    var last: ReconnectState? { lock.lock(); defer { lock.unlock() }; return storage.last }
    var sawReconnecting: Bool {
        lock.lock(); defer { lock.unlock() }
        return storage.contains { if case .reconnecting = $0 { return true } else { return false } }
    }
}

extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ReconnectTests {
        /// Full drop-then-reconnect: login + join #a + join #b + a message, then
        /// the server drops the socket. The model schedules a reconnect that
        /// re-dials the same still-listening server, re-logs-in, re-joins BOTH
        /// rooms (JOIN #a then JOIN #b, in order), the pre-drop transcripts are
        /// retained (the pre-drop message still in #a's box), and a
        /// post-reconnect message renders appended to the existing panels.
        @Test(.timeLimit(.minutes(1)))
        func dropReconnectsRejoinsBothRoomsAndRetainsTranscripts() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let states = ReconnectStateBox()
            model.onReconnectStateChanged = { states.append($0) }

            try await model.start()
            // Initial login + join #a.
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a")
            try await waitUntil { model.roomInfos.contains { $0.name == "#a" } }

            // Join a second room #b.
            Task { try? await model.joinRoom("#b") }
            try await server.waitForClientLine(containing: "JOIN #b")
            try await server.send(
                ":Mac!mac@h JOIN :#b",
                ":srv 353 Mac = #b :Mac",
                ":srv 366 Mac #b :End of NAMES list")
            try await waitUntil { model.roomInfos.contains { $0.name == "#b" } }

            // A message in #a (the active room) before the drop.
            try await server.send(":Win!u@h PRIVMSG #a :hello before drop")
            try await waitUntil { model.transcriptContains("#a", "hello before drop") }

            // --- DROP: the server severs the socket (but keeps listening).
            // Clear the received-byte record so the assertions below see ONLY
            // the reconnect's own re-login/re-join bytes.
            server.resetReceivedBytes()
            server.dropPeer()

            // The model enters its reconnect loop.
            try await waitUntil { states.sawReconnecting }

            // The reconnect's fresh session re-dials this same server and
            // re-logs-in (NICK/USER). Reply through the probe->451->001
            // handshake, then confirm BOTH re-joins in join order (#a, #b).
            try await server.replyToProbeWith451ThenWelcome(nick: "Mac")
            try await server.waitForClientLine(containing: "JOIN #a")
            try await server.send(
                ":Mac!mac@h JOIN :#a",
                ":srv 353 Mac = #a :Mac",
                ":srv 366 Mac #a :End of NAMES list")
            try await server.waitForClientLine(containing: "JOIN #b")
            try await server.send(
                ":Mac!mac@h JOIN :#b",
                ":srv 353 Mac = #b :Mac",
                ":srv 366 Mac #b :End of NAMES list")

            // The reconnect state resolves to `.connected`.
            try await waitUntil { states.last == .connected }

            // Re-login actually happened (NICK + USER seen on the fresh peer).
            let reloginText = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            #expect(reloginText.contains("NICK Mac") || reloginText.contains("NICK :Mac"))
            #expect(reloginText.contains("USER "))
            // BOTH re-joins seen, #a before #b.
            let joinA = reloginText.range(of: "JOIN #a")
            let joinB = reloginText.range(of: "JOIN #b")
            #expect(joinA != nil, "reconnect must re-join #a")
            #expect(joinB != nil, "reconnect must re-join #b")
            if let joinA, let joinB { #expect(joinA.lowerBound < joinB.lowerBound, "re-joins in join order #a then #b") }

            // Transcript retained across the reconnect.
            #expect(model.transcriptContains("#a", "hello before drop"),
                    "pre-drop line must survive the reconnect")
            let panelsBefore = model.panelCount

            // A post-reconnect message renders APPENDED (panelCount grows; the
            // new line joins the old one in the SAME room's transcript).
            try await server.send(":Win!u@h PRIVMSG #a :hello after reconnect")
            try await waitUntil { model.transcriptContains("#a", "hello after reconnect") }
            try await waitUntil { model.panelCount > panelsBefore }
            #expect(model.transcriptContains("#a", "hello before drop"),
                    "pre-drop line still present alongside the post-reconnect one")

            model.shutdown()
            server.stop()
        }

        /// `shutdown()` during the backoff window stops the loop: nothing new
        /// connects afterward.
        @Test(.timeLimit(.minutes(1)))
        func shutdownDuringBackoffStopsReconnect() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let states = ReconnectStateBox()
            model.onReconnectStateChanged = { states.append($0) }

            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a")
            try await waitUntil { model.roomInfos.contains { $0.name == "#a" } }

            server.resetReceivedBytes()
            server.dropPeer()
            try await waitUntil { states.sawReconnecting }

            // Shut down mid-backoff (the first backoff is 2s; shutdown lands
            // inside it), then confirm NO reconnect re-dials for a window longer
            // than that first backoff. The reconnect signature is a FRESH
            // session's `MODE ISIRCX` probe (its first outbound line); asserting
            // on that rather than strict emptiness tolerates any residual
            // pre-drop outbound (e.g. a queued channel-`MODE` query) that may
            // flush after the reset — what matters is that no NEW login begins.
            model.shutdown()
            try await Task.sleep(nanoseconds: 3_000_000_000)
            let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            #expect(!text.contains("MODE ISIRCX"),
                    "no reconnect probe should re-dial after shutdown, got: \(text)")
            server.stop()
        }

        /// A user-initiated `shutdown()` (the disconnect path) produces NO
        /// reconnect at all: the drop it causes is expected, not an unexpected
        /// network drop.
        @Test(.timeLimit(.minutes(1)))
        func manualDisconnectDoesNotReconnect() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#a", artDir: art))
            let states = ReconnectStateBox()
            model.onReconnectStateChanged = { states.append($0) }

            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#a")
            try await waitUntil { model.roomInfos.contains { $0.name == "#a" } }

            server.resetReceivedBytes()
            model.shutdown()   // user-initiated teardown

            // Confirm no reconnect probe re-dials for a comfortable window (the
            // reconnect signature is a fresh session's `MODE ISIRCX`; a residual
            // pre-shutdown outbound flushing after the reset is not a reconnect).
            try await Task.sleep(nanoseconds: 3_000_000_000)
            let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            #expect(!text.contains("MODE ISIRCX"), "manual disconnect must not reconnect, got: \(text)")
            #expect(!states.sawReconnecting, "manual disconnect must not enter the reconnecting state")
            server.stop()
        }

        /// Polls `predicate` until true (bounded by the caller's `.timeLimit`).
        private func waitUntil(_ predicate: @escaping () -> Bool) async throws {
            while !predicate() {
                try await Task.sleep(nanoseconds: 20_000_000)
            }
        }
    }
}

private extension ChatSessionModel {
    /// Whether `room`'s transcript holds a `.text` event with body `text`.
    func transcriptContains(_ room: String, _ text: String) -> Bool {
        transcript(for: room).contains { ev in
            if case .text(_, _, _, let t, _, _) = ev { return t == text }
            return false
        }
    }
}

extension LoopbackIRCServer {
    /// Like `replyToProbeWith451ThenWelcomeAndJoin` but stops after the 001
    /// welcome — the caller drives the JOIN confirms itself (the reconnect
    /// test re-joins MULTIPLE rooms in order and needs to confirm each). Waits
    /// for the peer's FIRST `MODE ISIRCX` in the CURRENT received-byte record
    /// (call `resetReceivedBytes()` before a reconnect so this observes the
    /// fresh session's probe, not the pre-drop one's).
    func replyToProbeWith451ThenWelcome(nick: String) async throws {
        await startCollectingReceivedBytes()
        while true {
            let text = String(data: receivedBytes, encoding: .isoLatin1) ?? ""
            if text.contains("MODE ISIRCX\r\n") { break }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
        try await send(":srv 451 * :not registered")
        while true {
            let text = String(data: receivedBytes, encoding: .isoLatin1) ?? ""
            if text.contains("USER ") { break }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
        try await send(":srv 001 \(nick) :Welcome")
    }
}
