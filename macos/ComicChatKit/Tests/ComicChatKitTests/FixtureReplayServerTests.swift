import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4a Task 9 Step 4: FixtureReplayServer — offline demo/E2E replay of a
// capture's s2c bytes. Points a real ChatSessionModel at the server (exactly
// as a live app would, over a real loopback TCP connection) and asserts the
// login/join sequence produces a strip image and a `.selfJoined` transcript
// entry, with NO hand-scripted server responses (unlike ChatSessionModelTests,
// which drives LoopbackIRCServer directly).
//
// FIXTURE: Fixtures/captures/smoke-2-replay.jsonl, derived from the committed
// smoke-2.jsonl Wine-rig capture's "client 2" slice (nick "Anonymous") — see
// that file's own doc comment / FixtureReplayServer.swift's top doc comment
// for why a raw capture's c2s lines can't drive pacing directly and what was
// kept (3 s2c chunks: 451, 001+MOTD, JOIN-echo+NAMES).
//
// SERIALIZATION: nested inside EngineGlobalStateSelfTests (.serialized), same
// reasoning as ChatSessionModelTests (ChatSessionModel drives both
// cc_session_* and cc_strip_* against the shared process-global engine state).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct FixtureReplayServerTests {
        @Test(.timeLimit(.minutes(1)))
        func replayProducesStripImageAndSelfJoined() async throws {
            let fixtureURL = Bundle.module.url(forResource: "smoke-2-replay", withExtension: "jsonl",
                                               subdirectory: "Fixtures/captures")!
            let server = try FixtureReplayServer(fixtureURL: fixtureURL)
            try server.start()

            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            // The fixture's client-2 slice used nick "Anonymous" throughout
            // (NICK/USER, JOIN echo, NAMES) — FixtureReplayServer does not
            // rewrite the fixture's recorded nick to match the connecting
            // client (see that type's doc comment), so a real demo/test must
            // connect with the SAME nick the fixture was captured under.
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Anonymous", room: "#comicrig", artDir: art))
            let images = FixtureImagesBox()
            let imagesArrived = AsyncStream<Void>.makeStream()
            model.onStripImage = { _, size in
                images.append(size)
                imagesArrived.continuation.yield()
            }

            try await model.start()
            var iter = imagesArrived.stream.makeAsyncIterator()
            _ = await iter.next()
            #expect(images.count >= 1)

            let transcript = model.transcript
            #expect(transcript.contains { if case .selfJoined(let ch) = $0 { return ch == "#comicrig" } else { return false } })

            model.shutdown()
            server.stop()
        }
    }
}

/// Same thread-safe accumulator shape as `ChatSessionModelTests`' `ImagesBox`
/// (separate type: test files in this target don't share internal symbols
/// across files by default without `internal`, and duplicating this tiny
/// helper is simpler than threading visibility).
private final class FixtureImagesBox: @unchecked Sendable {
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
}
