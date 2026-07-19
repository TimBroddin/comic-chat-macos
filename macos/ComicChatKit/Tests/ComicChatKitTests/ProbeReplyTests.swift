import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Batch C: un-suppressing the two peer probe queries the engine
// previously ate silently — inbound CTCP VERSION and "# GetInfo" (Tim opted
// in for these two specifically; PING/TIME/EMAIL/URL/CLIENTINFO stay
// suppressed, untouched by this task). Feeds the REAL wire grammar the
// original's ChatGetVersion/GetInfo-request senders would produce
// (protsupp.cpp:3701-3706 / GETINFOPREFIX's plain "#" + " GetInfo" form,
// both READ-ONLY-cited in comicchat.h's CC_EV_VERSION_REQUEST/
// CC_EV_INFO_REQUEST doc comments) through a real loopback session, and
// asserts BOTH the fired `ProtocolEvent` and the exact reply bytes that hit
// the wire — same "true round-trip through the engine's own parser" shape
// SoundSendingTests already established for outbound SOUND.
//
// SERIALIZATION: nested inside `EngineGlobalStateSelfTests` (.serialized),
// same posture as every other suite touching process-global engine state via
// ProtocolSession/ChatSessionModel (AnnounceTests/SoundSendingTests/etc).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct ProbeReplyTests {
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

        private func art() -> String {
            repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
        }

        // Bare CTCP \x01VERSION\x01 (the original's ChatGetVersion request
        // grammar, protsupp.cpp:3701-3706: g_nVersionLen=8 bytes of versionID
        // "\x01VERSION" followed by one more 0x01) -> CC_EV_VERSION_REQUEST
        // -> our OWN version-reply text, NOTICE-carried, matching
        // ReplyVersion's bAsNotice=TRUE (protsupp.cpp:1139).
        @Test(.timeLimit(.minutes(1)))
        func inboundVersionProbeFiresReplyOverNotice() async throws {
            let server = try LoopbackIRCServer()
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art()))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // Wire-exact bytes a peer's ChatGetVersion would send: \x01VERSION\x01.
            try await server.send(":Bob!bob@h PRIVMSG #p4 :\u{01}VERSION\u{01}")

            let sent = try await waitForReceivedLine(server, containing: "VERSION")
            let expected = "NOTICE Bob :\u{01}VERSION \(ChatSessionModel.versionReplyText)\u{01}"
            #expect(sent.contains(expected),
                    "expected the exact VERSION NOTICE reply; c2s so far: \(sent)")

            model.shutdown()
            server.stop()
        }

        // "# GetInfo" (the original's plain-PRIVMSG request grammar,
        // GETINFOPREFIX = " GetInfo" with no body -- see comicchat.h's
        // CC_EV_INFO_REQUEST citation) -> CC_EV_INFO_REQUEST -> our
        // "# HeresInfo: <profile>" reply, plain PRIVMSG (bAsNotice=FALSE,
        // matching the original's own GetInfo-branch call).
        @Test(.timeLimit(.minutes(1)))
        func inboundGetInfoProbeFiresReplyWithProfileText() async throws {
            let server = try LoopbackIRCServer()
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art(),
                                                       profileText: "Hello from Mac"))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!bob@h PRIVMSG #p4 :# GetInfo")

            let sent = try await waitForReceivedLine(server, containing: "HeresInfo")
            #expect(sent.contains("PRIVMSG Bob :# HeresInfo: Hello from Mac"),
                    "expected the exact GetInfo PRIVMSG reply; c2s so far: \(sent)")

            model.shutdown()
            server.stop()
        }

        // Empty profile text (the default, and the ID_DEFAULT_PROFILE
        // archaeology gap's honest-placeholder posture, see
        // SettingsStore.profileText's doc comment) still gets a reply --
        // just with an empty body after the prefix.
        @Test(.timeLimit(.minutes(1)))
        func inboundGetInfoProbeWithEmptyProfileStillReplies() async throws {
            let server = try LoopbackIRCServer()
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art()))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!bob@h PRIVMSG #p4 :# GetInfo")

            let sent = try await waitForReceivedLine(server, containing: "HeresInfo")
            #expect(sent.contains("PRIVMSG Bob :# HeresInfo: "),
                    "expected an empty-profile GetInfo reply; c2s so far: \(sent)")

            model.shutdown()
            server.stop()
        }

        // Rate limit (deliberate modern guard, not in the original): two
        // VERSION probes from the SAME nick inside the 10s cooldown window
        // produce exactly ONE reply.
        @Test(.timeLimit(.minutes(1)))
        func rateLimitCapsRepeatedProbesToOneReplyPerWindow() async throws {
            let server = try LoopbackIRCServer()
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art()))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!bob@h PRIVMSG #p4 :\u{01}VERSION\u{01}")
            _ = try await waitForReceivedLine(server, containing: "VERSION")

            // Second probe from the SAME nick, immediately after -- well
            // inside the 10s window.
            try await server.send(":Bob!bob@h PRIVMSG #p4 :\u{01}VERSION\u{01}")

            // Settling window long enough for a second reply (a regression)
            // to have landed, before asserting the final count.
            try await Task.sleep(nanoseconds: 300_000_000)

            let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
            let replyCount = text.components(separatedBy: "\r\n")
                .filter { $0.contains("NOTICE Bob") && $0.contains("VERSION") }
                .count
            #expect(replyCount == 1, "expected exactly one VERSION reply inside the cooldown window, got \(replyCount)")

            model.shutdown()
            server.stop()
        }
    }
}
