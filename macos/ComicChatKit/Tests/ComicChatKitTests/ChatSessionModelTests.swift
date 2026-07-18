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
            // this say has actually reached the wire). Plan 4b Task 3: every
            // send is now cooked/annotated (until Task 5's `sendComicsData`
            // opt-out), so the wire line is "PRIVMSG #p4 :(#G...) hi win",
            // not a bare "PRIVMSG #p4 :hi win" -- match on "hi win" alone.
            let sent = try await waitForReceivedLine(server, containing: "hi win")
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

        /// Plan 4b Task 4 fix round 2 regression test: `handleLocked`'s
        /// `.text` case was split out into its own bound case to add
        /// whisper-box routing (§8 Topology A finding), but that edit
        /// dropped the `.action` arm that used to share `.text`'s
        /// `bridge.apply + recomposeLocked` behavior — inbound `.action`
        /// events (a peer's /me) fell through to `default: break`:
        /// transcript-appended but never bridge-applied or recomposed, so
        /// the action never rendered onto the comic strip. Pins that an
        /// inbound CTCP ACTION both lands in the transcript AND triggers a
        /// strip recompose (`onStripImage` firing again).
        ///
        /// Wire form verified against the engine parser (read-only check,
        /// NOT assumed): `ircsock.cpp`'s `cmdidPrivMsg` handler classifies a
        /// PRIVMSG's payload via `OnTextMsg`/`protsupp.cpp`'s payload-stage
        /// dispatch, which recognizes an action via
        /// `!strncmp(szMesg, actionID, g_nActionLen)`
        /// (`protsupp.cpp:1147`), where `actionID` is `{0x01, 'A', 'C', 'T',
        /// 'I', 'O', 'N'}` (`ircproto.h:241`) — the standard CTCP ACTION
        /// prefix, `\x01ACTION <text>`, optionally terminated by a trailing
        /// `\x01` (`ccPrepareTextAction`, `protsupp.cpp:1268`, trims at the
        /// first `0x01` if present but does not require one). This test
        /// sends the fully-terminated form.
        @Test(.timeLimit(.minutes(1)))
        func inboundActionRendersOntoStrip() async throws {
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
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // Baseline: self-join's own announce-avatar recompose already
            // fires onStripImage at least once before any peer activity —
            // capture that count so we can prove a NEW recompose happens
            // after the action, not just that one happened at some point.
            var iter = imagesArrived.stream.makeAsyncIterator()
            _ = await iter.next()
            let baseline = images.count

            try await server.send(":Bob!u@h PRIVMSG #p4 :\u{01}ACTION waves\u{01}")

            // The engine's ccPrepareTextAction (protsupp.cpp:1265-1274)
            // prepends the speaker's nick onto the action text itself
            // (`strNewMesg = szNickname; strNewMesg += (szMesg +
            // g_nActionLen)`), verified above by inspecting the actual
            // transcript event -- so CC_EV_ACTION's text is "Bob waves",
            // not the bare "waves" a first guess might expect.
            func actionLanded() -> Bool {
                model.transcript.contains {
                    if case .action(let nick, let text, _) = $0 { return nick == "Bob" && text == "Bob waves" }
                    return false
                }
            }
            while !actionLanded() {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(actionLanded())

            // The regression: before the fix, the .action event was
            // transcript-appended (above) but never bridge-applied/
            // recomposed, so onStripImage would never fire again here.
            while images.count <= baseline {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(images.count > baseline, "expected a strip recompose after the inbound action, got no new onStripImage call (baseline \(baseline))")

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
            // Plan 4b Task 3: every send is now cooked/annotated, so the wire
            // line is "PRIVMSG #p4 :(#G...) hello once", not a bare
            // "PRIVMSG #p4 :hello once" -- match on "hello once" alone.
            _ = try await waitForReceivedLine(server, containing: "hello once")
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

        /// THE DOCTRINE TEST (Plan 4b Task 5 fix round 1, reviewer-confirmed
        /// Important finding): "anything not derivable from the event log is
        /// lost on reflow" (D2 §2.3) — `changeCharacter` must be replayable
        /// from `_transcript`, not just applied live to the strip. Pre-fix,
        /// `changeCharacter` called `strip.setParticipantAvatar` directly and
        /// recorded NOTHING in `_transcript`; `reflowLocked` always seeds the
        /// self participant from `config.characterName` (the CURRENT/
        /// post-switch character) — so a reflow after a switch silently
        /// rewrote every PRE-switch panel with the post-switch avatar.
        ///
        /// Proof shape (the brief's "simplest robust approach"): capture the
        /// composed strip image at a fixed geometry G (line 1 -> switch
        /// character -> line 2), force an actual reflow via a DIFFERENT
        /// geometry, then return to G and capture again. If the switch is
        /// correctly positional (this fix), returning to the SAME geometry
        /// reproduces BYTE-IDENTICAL pixels, because reflow replays
        /// `_transcript` -- which now records the switch at its correct
        /// position -- from the ORIGINAL character forward. Pre-fix, the
        /// reflow instead seeds the self participant with the POST-switch
        /// character from the start, so the pre-switch panel (line 1) comes
        /// back wearing the wrong avatar -- different pixels.
        ///
        /// Geometry choice (computed offline against `PanelFit`'s actual
        /// math, not guessed): G = 600pt -> 12000 twips -> 3 columns / 3904
        /// unit twips. The differing reflow-forcing geometry is 400pt ->
        /// 8000 twips -> 2 columns / 3928 unit twips (both columns AND unit
        /// differ from G, so `setViewport`'s `didSetViewport && columns ==
        /// currentColumns && unit == currentUnitTwips` gate -- the "same
        /// geometry, recompose only, no reflow" fast path -- is guaranteed to
        /// MISS and fall through to a real `reflowLocked()` both times
        /// (400 -> forces the first reflow away from G; 600 again -> forces
        /// the second reflow BACK to G, since the model's `currentColumns`/
        /// `currentUnitTwips` are now the 400pt values, not G's).
        ///
        /// SETTLING: each mutating call (`setViewport`/`changeCharacter`/
        /// `send`) is followed by `settleEngineQueue`'s sentinel-hop (proven
        /// pattern, `PersonaSettingsTests.changeCharacterMidSessionSwitchesAvatarAndAnnounces`)
        /// to guarantee the ENGINE-QUEUE side of that call (including a full
        /// `reflowLocked()`, which runs entirely on the engine queue) has
        /// finished before the next call fires -- deliberately NOT counting
        /// `onStripImage` callback arrivals via an `AsyncStream`: `.selfJoined`
        /// AND `setViewport`'s OWN first-call-always-reflows path both fire an
        /// early recompose before this test's first explicit `setViewport`
        /// call even runs, so a naive "one `next()` per mutating call" count
        /// is off by one against actual `onStripImage` firings -- settling
        /// via the engine queue itself sidesteps that miscount entirely
        /// (proven during this fix's own debugging: an earlier
        /// stream-counting version of this test produced a genuinely
        /// reordered/duplicated-looking reflow sequence purely from that
        /// miscount, not from any production bug). After settling the engine
        /// queue, a short fixed wait covers `recomposeLocked`'s trailing
        /// `DispatchQueue.main.async` hop (the ONLY part of a recompose that
        /// runs off the engine queue) so `latestImage` is guaranteed current
        /// by the time it's read.
        @Test(.timeLimit(.minutes(1)))
        func characterSwitchSurvivesReflowAtOriginalGeometry() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            model.onStripImage = { image, _ in latestImage.set(image) }

            func settle() async throws {
                await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                    model.settleEngineQueue { cont.resume() }
                }
                // Covers recomposeLocked's trailing DispatchQueue.main.async
                // hop -- the engine-queue settle above only guarantees the
                // engine-side work (including the reflow itself) is done.
                try await Task.sleep(nanoseconds: 100_000_000)
            }

            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // Settle at geometry G (600pt -- see doc comment for the computed
            // columns/unit). This is itself a reflow away from `start()`'s
            // 3-column/minUnitPanelWidth default, which is fine -- it's not
            // part of what's being compared.
            model.setViewport(widthPoints: 600, scale: 2.0)
            try await settle()

            // line 1 (pre-switch)
            try await model.send("first line")
            _ = try await waitForReceivedLine(server, containing: "first line")
            try await settle()

            // the switch itself
            model.changeCharacter("armando")
            try await settle()

            // line 2 (post-switch) -- forces a NEW panel per the switch's
            // future-panels-only semantics.
            try await model.send("second line")
            _ = try await waitForReceivedLine(server, containing: "second line")
            try await settle()

            guard let imageA = latestImage.get() else {
                Issue.record("no strip image captured before reflow")
                return
            }
            let bytesA = try pixelBytes(imageA)

            // Force an actual reflow away from G (400pt -- different columns
            // AND unit, guaranteed to miss setViewport's same-geometry fast
            // path), then back to G.
            model.setViewport(widthPoints: 400, scale: 2.0)
            try await settle()
            model.setViewport(widthPoints: 600, scale: 2.0)
            try await settle()

            guard let imageB = latestImage.get() else {
                Issue.record("no strip image captured after reflow")
                return
            }
            let bytesB = try pixelBytes(imageB)

            // Compared as a precomputed Bool (NOT `#expect(bytesA == bytesB)`
            // directly) -- deliberate, and load-bearing: Swift Testing's
            // `#expect(a == b)` macro, on a FAILING comparison between two
            // large `Collection`s, computes an edit-distance diff
            // (`BidirectionalCollection.difference(from:)`, Myers' algorithm)
            // to build a detailed failure message. For two ~1-2 MB `Data`
            // blobs that are extensively different (exactly the pre-fix
            // regression shape -- a wrong avatar changes most of a panel's
            // pixels), that diff computation is catastrophically slow -- this
            // was empirically confirmed via `lldb -p <pid> -o "bt all"` on a
            // "hung" pre-fix run: the process was NOT deadlocked, it was
            // sitting in `LinearMyers.backwardSearch`/`findDifferences`,
            // still running minutes later. Comparing a precomputed `Bool`
            // instead gives `#expect` nothing to diff -- the RED failure
            // message is less detailed (just the two byte COUNTS/equality),
            // but the test fails in milliseconds instead of hanging.
            let identical = bytesA == bytesB
            #expect(identical,
                    "expected the reflowed strip at the ORIGINAL geometry to be byte-identical to the pre-reflow capture (proves the pre-switch panel kept its ORIGINAL avatar through reflow) -- a mismatch means the character switch was NOT correctly replayed positionally (transcript-doctrine regression). bytesA.count=\(bytesA.count) bytesB.count=\(bytesB.count)")

            model.shutdown()
            server.stop()
        }

        /// Plan 4b Task 6 — model-wiring integration: a peer's `.appearsAs`
        /// naming UNKNOWN art (not in `artDir`, not in the user characters
        /// dir) WITH a well-formed http URL triggers the auto-download path
        /// end to end. `StubHTTPProtocol` (below) intercepts the request
        /// (registered on `URLSession.shared`, which `downloadAvatarIfNeededLocked`'s
        /// `AvatarDownloader()` uses by default) and serves the armando.avb
        /// fixture bytes — no real network. Proves: the file lands under
        /// `ChatSessionModel.userCharactersDir` AND the peer's participant is
        /// re-avatared (observable via a SECOND recompose after the one the
        /// `.appearsAs` handling itself already triggers — `panelCount`
        /// alone can't distinguish "re-avatared" from "not", so this polls
        /// for the downloaded file's existence, the load-bearing assertion,
        /// then confirms no crash/hang followed).
        @Test(.timeLimit(.minutes(1)))
        func unknownAvatarWithURLTriggersAutoDownload() async throws {
            let downloadName = "cc-t6-wiring-\(UUID().uuidString)"
            let stubURL = URL(string: "http://cc-t6-stub.invalid/\(downloadName).avb")!
            let fixtureData = try Data(contentsOf: URL(fileURLWithPath: fixture("armando.avb")))
            StubHTTPProtocol.register(url: stubURL, data: fixtureData)
            defer { StubHTTPProtocol.unregister(url: stubURL) }

            let userCharactersDir = ChatSessionModel.userCharactersDir
            let expectedPath = (userCharactersDir as NSString).appendingPathComponent("\(downloadName.lowercased()).avb")
            try? FileManager.default.removeItem(atPath: expectedPath)
            defer { try? FileManager.default.removeItem(atPath: expectedPath) }

            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art,
                                                       autoDownloadAvatars: true))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // A peer joins, then announces an avatar name this session has
            // never seen anywhere (bundled art OR user characters dir),
            // carrying the stubbed http URL -- the wire grammar is
            // "# Appears as <name>.<url>" (cc_selftest.cpp:3899's verified
            // shape).
            try await server.send(":Win!u@h JOIN #p4")
            try await server.send(":Win!u@h PRIVMSG #p4 :# Appears as \(downloadName).\(stubURL.absoluteString)")

            var attempts = 0
            while !FileManager.default.fileExists(atPath: expectedPath), attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(FileManager.default.fileExists(atPath: expectedPath),
                    "expected the downloaded avatar to land at \(expectedPath)")
            // The downloaded file itself must be the valid fixture content,
            // not a partial/corrupt write.
            let landed = try Data(contentsOf: URL(fileURLWithPath: expectedPath))
            #expect(landed == fixtureData)

            model.shutdown()
            server.stop()
        }
    }
}

/// Minimal request-URL-keyed `URLProtocol` stub (Plan 4b Task 6): intercepts
/// exactly the registered URL(s) and serves canned bytes with a 200 response,
/// so `unknownAvatarWithURLTriggersAutoDownload` exercises the REAL
/// `AvatarDownloader`/`URLSession.shared` path (matching what
/// `downloadAvatarIfNeededLocked` actually constructs in production) without
/// touching the real network. Registered process-wide via
/// `URLProtocol.registerClass` (affects `.shared`'s default configuration,
/// which consults registered protocol classes for any URL matching
/// `canInit(with:)`) and unregistered per-URL by the test's `defer`.
private final class StubHTTPProtocol: URLProtocol, @unchecked Sendable {
    private static let lock = NSLock()
    nonisolated(unsafe) private static var responses: [URL: Data] = [:]
    nonisolated(unsafe) private static var registered = false

    static func register(url: URL, data: Data) {
        lock.lock()
        responses[url] = data
        if !registered {
            URLProtocol.registerClass(StubHTTPProtocol.self)
            registered = true
        }
        lock.unlock()
    }

    static func unregister(url: URL) {
        lock.lock()
        responses.removeValue(forKey: url)
        lock.unlock()
    }

    override class func canInit(with request: URLRequest) -> Bool {
        guard let url = request.url else { return false }
        lock.lock(); defer { lock.unlock() }
        return responses[url] != nil
    }

    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        guard let url = request.url else {
            client?.urlProtocol(self, didFailWithError: URLError(.badURL))
            return
        }
        Self.lock.lock()
        let data = Self.responses[url]
        Self.lock.unlock()
        guard let data else {
            client?.urlProtocol(self, didFailWithError: URLError(.fileDoesNotExist))
            return
        }
        let response = HTTPURLResponse(url: url, statusCode: 200, httpVersion: "HTTP/1.1",
                                       headerFields: ["Content-Length": "\(data.count)"])!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: data)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

/// Thread-safe single-slot box for the MOST RECENT `CGImage` `onStripImage`
/// delivered -- `characterSwitchSurvivesReflowAtOriginalGeometry` needs the
/// actual image (not just its size, which `ImagesBox` already tracks) to do
/// its byte-identical pixel comparison. Same lock-guarded shape as
/// `ImagesBox`/`MembersBox` above (`onStripImage` is `@Sendable`, called on
/// the main thread, read concurrently from the test's own task).
private final class LatestImageBox: @unchecked Sendable {
    private let lock = NSLock()
    private var image: CGImage?

    func set(_ image: CGImage) {
        lock.lock(); defer { lock.unlock() }
        self.image = image
    }

    func get() -> CGImage? {
        lock.lock(); defer { lock.unlock() }
        return image
    }
}

private let latestImage = LatestImageBox()

/// Reads back EVERY pixel of `image` into a `Data` buffer for byte-identical
/// comparison -- same `CGContext` readback technique as
/// `CGCanvasMirrorTests.pixel(_:x:y:)`, generalized to the WHOLE image rather
/// than one pixel, since `characterSwitchSurvivesReflowAtOriginalGeometry`
/// needs to prove TWO FULL COMPOSITED STRIPS are pixel-for-pixel identical,
/// not just one probe point (a positional avatar regression could plausibly
/// only move a handful of pixels within one panel).
private func pixelBytes(_ image: CGImage) throws -> Data {
    let width = image.width, height = image.height
    var buffer = [UInt8](repeating: 0, count: width * height * 4)
    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    guard let ctx = CGContext(
        data: &buffer, width: width, height: height,
        bitsPerComponent: 8, bytesPerRow: width * 4, space: colorSpace,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        throw Strip.StripError(message: "pixelBytes readback context failed")
    }
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
    return Data(buffer)
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
