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
/// while the test body reads it from its own task). Keeps the FULL history of
/// delivered snapshots (not just the latest), in delivery order, so a test
/// can assert on which snapshot arrived LAST — `emitMembers`'s detached-`Task`
/// pattern means a slow earlier snapshot can be delivered after a later one,
/// and only the history captures whether that actually happened.
private final class MembersBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [String] = []
    private var deliveryHistory: [[String]] = []

    func set(_ nicks: [String]) {
        lock.lock(); defer { lock.unlock() }
        storage = nicks
        deliveryHistory.append(nicks)
    }

    func get() -> [String] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }

    /// Every snapshot delivered so far, in delivery order.
    func history() -> [[String]] {
        lock.lock(); defer { lock.unlock() }
        return deliveryHistory
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

        /// Plan 4b Task 1 (4a final-review carryover): own-say echo dedup.
        /// Some IRC servers echo a client's own PRIVMSG back to the sender
        /// (unlike the loopback rig's default, which never does). `send(_:)`
        /// already renders the own line immediately via a synthetic local
        /// `.text` event (that method's doc comment) — if the server ALSO
        /// echoes the same PRIVMSG back, the transcript must still contain
        /// exactly ONE `.text` event with that text, not two.
        @Test(.timeLimit(.minutes(1)))
        func ownSayEchoIsDeduped() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.send("hello once")
            // wait for the say to actually reach the wire before the server
            // "echoes" it back — otherwise the echo could arrive and be
            // processed before the synthetic local event, which would still
            // dedupe correctly but wouldn't exercise the intended ordering.
            _ = try await waitForReceivedLine(server, containing: "PRIVMSG #p4 :hello once")
            try await server.send(":Mac!mac@h PRIVMSG #p4 :hello once")

            func matchCount() -> Int {
                model.transcript.filter {
                    if case .text(_, _, _, let text, _, _) = $0 { return text == "hello once" }
                    return false
                }.count
            }
            // Wait for the synthetic local render (always exactly one, sent
            // BEFORE the echo above) to land, then give the echo a further
            // settling window — long enough for it to have been processed if
            // it were going to double-append (pre-fix behavior) — before
            // asserting the final count.
            while matchCount() < 1 {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            try await Task.sleep(nanoseconds: 200_000_000)
            #expect(matchCount() == 1, "expected exactly one .text event for the own-say echo, got \(matchCount())")

            model.shutdown()
            server.stop()
        }

        /// Plan 4b Task 1 (4a final-review carryover): members-ordering
        /// guard. `emitMembers()` reads `session.room(_:)` from a detached
        /// `Task` (that method's own doc comment explains why it can't be a
        /// direct synchronous read) — under a burst of rapid membership
        /// churn, several of these `Task`s can be in flight at once, and
        /// without a sequence guard nothing stops an earlier-fired-but-slower
        /// `Task` from delivering its (now stale) snapshot to `onMembers`
        /// AFTER a later, more current one already arrived. This test drives
        /// a burst of churn (twenty nicks JOIN, then all but one PART, each
        /// its own real network round-trip) and asserts the LAST `onMembers`
        /// delivery equals the final member set — not merely that the final
        /// set is eventually reached (which `peerJoinRefreshesMemberSidebar`-
        /// style polling would miss: polling for "contains X" doesn't notice
        /// a stale snapshot arriving last).
        ///
        /// HONESTY NOTE: this specific inversion was NOT reproduced as a
        /// reliable black-box failure against the pre-fix code on this
        /// harness (tried up to 20-way bursts, both batched and per-line
        /// real round-trips, across many repeated runs — the detached
        /// `Task`s' completion order tracked spawn order closely enough in
        /// practice that the race did not flip). The `membersSeq`/
        /// `appliedMembersSeq` guard below is still implemented exactly per
        /// the brief (it is cheap, clearly correct, and matches the documented
        /// hazard `emitMembers`'s own doc comment describes), and this test
        /// stands as a real regression guard for its observable contract
        /// going forward, not as adversarial proof the pre-fix code was
        /// broken. See the Task 1 report for how this was investigated.
        @Test(.timeLimit(.minutes(1)))
        func membersOrderingReflectsLatestChurn() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let members = MembersBox()
            model.onMembers = { nicks in members.set(nicks) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // Large burst of membership churn, each line its own network
            // round-trip (rather than one batched write) so real socket I/O
            // and GCD scheduling interleave with the `emitMembers()` detached
            // `Task`s each event spawns -- maximizing the chance that
            // several are genuinely in flight at once and can complete out
            // of spawn order (each Task's own `session.room(_:)` read
            // contends with `sessionQueue`/`engineQueue`, which is BUSY
            // processing the rest of this same burst). Twenty nicks join,
            // then all but the last one part. Final state: {Mac, N19}.
            for i in 0..<20 {
                try await server.send(":N\(i)!u@h JOIN #p4")
            }
            for i in 0..<19 {
                try await server.send(":N\(i)!u@h PART #p4")
            }

            let expectedFinal: Set<String> = ["Mac", "N19"]
            // Settle on QUIESCENCE, not on "the expected value showed up" --
            // the latter would make the loop's own exit condition the thing
            // under test (trivially true the moment it's checked). Instead,
            // poll `deliveryHistory`'s COUNT until it stops growing for a
            // sustained window, then assert on whatever the last entry
            // actually is. This lets a stale, later-arriving delivery (the
            // pre-fix bug) show up as the final entry if the race fires.
            var lastCount = -1
            var stableSince = ContinuousClock.now
            while ContinuousClock.now - stableSince < .milliseconds(150) {
                let count = members.history().count
                if count != lastCount {
                    lastCount = count
                    stableSince = ContinuousClock.now
                }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            let history = members.history()
            #expect(!history.isEmpty)
            #expect(Set(history.last!) == expectedFinal,
                    "expected the LAST onMembers delivery to be \(expectedFinal), got \(history.last!) (full history count: \(history.count), last 5: \(history.suffix(5)))")

            model.shutdown()
            server.stop()
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
