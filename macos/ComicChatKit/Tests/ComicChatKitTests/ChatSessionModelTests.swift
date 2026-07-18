import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4a Task 9 — the app's headless core, live-loop test: drives a full
// ChatSessionModel session (connect -> auto-login -> join -> announce, a
// peer's annotated message rendering a strip image, and a local `send`
// emitting PRIVMSG bytes) over a real LoopbackIRCServer.
//
// SERIALIZATION (same reasoning as StripTests/AnnounceTests/EngineInterleaveTests):
// ChatSessionModel drives BOTH cc_session_* (ProtocolSession) and cc_strip_*
// (ProtocolStripBridge/Strip) against the same process-global engine state
// (comicchat.h's single-thread contract), so this suite is nested inside
// EngineGlobalStateSelfTests (.serialized).
/// Thread-safe accumulator for `onStripImage`'s captured `[CGSize]` list —
/// `onStripImage` is a `@Sendable` closure the model calls back on the main
/// thread (`DispatchQueue.main.async`), but the test body reads `images`
/// concurrently from its own task, which Swift 6 correctly flags as a data
/// race on a plain captured `var`. A lock-guarded box (same shape as
/// LoopbackIRCServer's `PortBox`/`PeerState`) fixes that without changing
/// the test's actual synchronization (still driven by `imagesArrived`).
private final class ImagesBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [CGSize] = []

    func append(_ size: CGSize) {
        lock.lock(); defer { lock.unlock() }
        storage.append(size)
    }

    var count: Int {
        lock.lock(); defer { lock.unlock() }
        return storage.count
    }

    var last: CGSize? {
        lock.lock(); defer { lock.unlock() }
        return storage.last
    }
}

/// Thread-safe accumulator for `onMembers`'s captured `[String]` (same
/// lock-guarded-box shape as `ImagesBox` above — `onMembers` is also a
/// `@Sendable` closure called on the main thread via `DispatchQueue.main.async`,
/// while the test body reads it from its own task).
private final class MembersBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [String] = []

    func set(_ nicks: [String]) {
        lock.lock(); defer { lock.unlock() }
        storage = nicks
    }

    func get() -> [String] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
}

extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ChatSessionModelTests {
        @Test(.timeLimit(.minutes(1)))
        func liveLoopRendersAndSends() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let images = ImagesBox()
            let imagesArrived = AsyncStream<Void>.makeStream()
            model.onStripImage = { _, size in
                images.append(size)
                imagesArrived.continuation.yield()
            }
            try await model.start()
            // login handshake (Task 2): reply 451 -> NICK/USER -> 001 -> JOIN echo
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")
            // a peer's annotated message must produce a strip image
            try await server.send(":Win!u@h PRIVMSG #p4 :(#G295E193M1)hello mac")
            var iter = imagesArrived.stream.makeAsyncIterator()
            _ = await iter.next()
            #expect(images.count >= 1 && images.last!.width > 0)
            // sending a say emits PRIVMSG bytes
            try await model.send("hi win")
            // wait for the "hi win" line specifically -- the earlier
            // self-join announce ALSO starts with "PRIVMSG #p4 :", so a
            // wait keyed on just that prefix would return too early (before
            // this say has actually reached the wire).
            let sent = try await waitForReceivedLine(server, containing: "PRIVMSG #p4 :hi win")
            #expect(sent.contains { $0.hasPrefix("PRIVMSG #p4 :") && $0.contains("hi win") })
            // the self-join announce (Task 8) went out
            #expect(sent.contains { $0.contains("# Appears as Anna") || $0.contains("# Appears as anna") })
            model.shutdown()
            server.stop()
        }

        /// Final review (Plan 4a) regression test for the `.userJoined` ->
        /// `emitMembers()` fix: after login/join, a peer JOINing mid-session
        /// must refresh the member sidebar (`onMembers`), not just the strip.
        /// Before the fix, `.userJoined` only routed to `bridge.apply` +
        /// `recomposeLocked()` — the member list never updated until some
        /// unrelated membership event (part/quit/kick/names/nick-change)
        /// happened to fire.
        @Test(.timeLimit(.minutes(1)))
        func peerJoinRefreshesMemberSidebar() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let members = MembersBox()
            model.onMembers = { nicks in members.set(nicks) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")
            // a peer joins mid-session
            try await server.send(":Peer!u@h JOIN #p4")
            // Poll (rather than wait for a single `onMembers` callback):
            // login/join itself already fires `emitMembers()` from `.names`/
            // `.endOfNames` as independent detached `Task`s (see `emitMembers`'s
            // doc comment), each racing to write `members` on its own
            // schedule -- waiting for just ONE more callback after sending the
            // peer's JOIN can observe an earlier, stale (pre-Peer) callback
            // instead of the one this test actually cares about. Polling until
            // the list actually contains "Peer" is the same shape as this
            // file's other polling helpers (`waitForReceivedLine`) and is
            // robust to that interleaving.
            while !members.get().contains("Peer") {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(members.get().contains("Peer"))
            model.shutdown()
            server.stop()
        }

        /// Polls `server.receivedBytes` until it contains `substring`, then
        /// returns the accumulated c2s bytes split into lines (CRLF-stripped,
        /// blank lines dropped) — the "receivedLines" accessor the brief
        /// refers to (LoopbackIRCServer exposes raw `receivedBytes`; this
        /// helper adds the line-splitting + wait-for-arrival convenience
        /// LoginSequencingTests/AnnounceTests already inline as
        /// `waitForReceivedBytes`, generalized to return lines instead of the
        /// raw joined string since this test greps individual lines).
        private func waitForReceivedLine(
            _ server: LoopbackIRCServer,
            containing substring: String
        ) async throws -> [String] {
            while true {
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                if text.contains(substring) {
                    return text.components(separatedBy: "\r\n").filter { !$0.isEmpty }
                }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }
    }
}

/// Test-only helper composing the Task-2 scenario-1 login lines
/// (LoginSequencingTests.plainIrcFallback: probe -> 451 -> NICK/USER -> 001)
/// with a JOIN echo (AnnounceTests.loginAndJoin's JOIN/353/366 block) — the
/// single call `ChatSessionModelTests`'s live-loop test needs to get a
/// `ChatSessionModel` from `start()` all the way to a joined room. Waits for
/// "MODE ISIRCX" to appear in the received bytes before replying (mirrors
/// LoginSequencingTests's own `waitForReceivedBytes` polling pattern) so this
/// doesn't race the probe.
extension LoopbackIRCServer {
    func replyToProbeWith451ThenWelcomeAndJoin(nick: String, channel: String, otherMembers: String = "") async throws {
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
        while true {
            let text = String(data: receivedBytes, encoding: .isoLatin1) ?? ""
            if text.contains("JOIN \(channel)\r\n") || text.contains("JOIN :\(channel)\r\n") { break }
            try await Task.sleep(nanoseconds: 5_000_000)
        }
        let names = otherMembers.isEmpty ? nick : "\(nick) \(otherMembers)"
        try await send(
            ":\(nick)!\(nick.lowercased())@h JOIN :\(channel)",
            ":srv 353 \(nick) = \(channel) :\(names)",
            ":srv 366 \(nick) \(channel) :End of NAMES list")
    }
}
