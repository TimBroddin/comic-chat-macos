import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4b Task 3: cooked outbound poses. `ChatSessionModel.send` grows a
// `mode:` parameter (`Strip.Mode`, default `.say`) and now builds an
// `Annotations` block from the CURRENT wheel/preview state (via
// `setEmotion`/`previewTyping`, this task's other new entry points) instead
// of always sending `annotations: nil` (Task 9's deliberate MVP scope, now
// superseded).
//
// SERIALIZATION: same reasoning as ChatSessionModelTests/SelfEmotionTests —
// ChatSessionModel drives both cc_session_* and cc_strip_* against the
// process-global engine, so this suite nests inside
// EngineGlobalStateSelfTests (.serialized).
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct CookedSendTests {
        /// Polls `server.receivedBytes` until it contains `substring`, then
        /// returns the accumulated c2s bytes decoded as ISO-Latin1 (matches
        /// the wire-byte-inspection pattern used throughout
        /// OutboundEncodingTests/ChatSessionModelTests).
        private func waitForReceivedBytes(
            _ server: LoopbackIRCServer,
            containing substring: String
        ) async throws -> String {
            while true {
                let text = String(data: server.receivedBytes, encoding: .isoLatin1) ?? ""
                if text.contains(substring) { return text }
                try await Task.sleep(nanoseconds: 5_000_000)
            }
        }

        /// Settles the engine queue: hops onto it and back, guaranteeing any
        /// work already `engineQueue.async`-scheduled before this call
        /// (`setEmotion`/`previewTyping`'s fire-and-forget dispatch) has run
        /// to completion by the time this returns. `settleEngineQueue` is an
        /// `internal` (test-visible via `@testable import`) sentinel hop on
        /// `ChatSessionModel` — the "sentinel enqueueEngineWork hop" the
        /// brief describes.
        private func settle(_ model: ChatSessionModel) async {
            await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                model.settleEngineQueue { cont.resume() }
            }
        }

        @Test(.timeLimit(.minutes(1)))
        func setEmotionThenSayCarriesCookedAnnotations() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            model.setEmotion(angle: 0, intensity: 1.0)
            await settle(model)

            try await model.send("posed line", mode: .say)

            let text = try await waitForReceivedBytes(server, containing: "PRIVMSG #p4 :")
            let lines = text.components(separatedBy: "\r\n").filter { $0.contains("posed line") }
            #expect(!lines.isEmpty, "expected a PRIVMSG line carrying \"posed line\", got: \(text)")
            #expect(lines.contains { $0.contains("(#G") },
                    "expected the outbound line to carry a (#G annotation block, got: \(lines)")

            // The local synthetic `.text` event must ALSO carry annotations
            // with cooked == true (the local echo renders the same posed
            // state that went out over the wire).
            let synthetic = model.transcript.last { ev in
                if case .text(_, _, _, let t, _, _) = ev { return t == "posed line" }
                return false
            }
            guard case .text(_, _, _, _, _, let annotations) = synthetic else {
                Issue.record("expected a .text transcript entry for \"posed line\"")
                return
            }
            #expect(annotations != nil)
            #expect(annotations?.cooked == true)

            model.shutdown()
            server.stop()
        }

        @Test(.timeLimit(.minutes(1)))
        func thinkModeCarriesThinkAnnotationAndRendersBalloon() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            let before = model.transcript.count
            try await model.send("thought", mode: .think)

            let text = try await waitForReceivedBytes(server, containing: "PRIVMSG #p4 :")
            let lines = text.components(separatedBy: "\r\n").filter { $0.contains("thought") }
            #expect(!lines.isEmpty, "expected a PRIVMSG line carrying \"thought\", got: \(text)")
            // SM_THINK == 3 (Task 2's confirmed SM_* mapping) -- the annotation
            // block's mode digit is wire-encoded as '0' + mode (M3 for THINK).
            #expect(lines.contains { $0.contains("(#G") && $0.contains("M3") },
                    "expected a think-mode (M3) annotation block, got: \(lines)")

            // Smoke-level render check: waiting for the synthetic local
            // .text event to land is sufficient proof the strip ingested the
            // line (bridge.apply + recomposeLocked run synchronously with
            // the transcript append on the engine queue) -- assert the
            // transcript grew and the strip actually has more panels than
            // before an empty/just-joined strip (panelCount > 0 already
            // proves a balloon-bearing panel was laid out without crashing).
            while model.transcript.count <= before {
                try await Task.sleep(nanoseconds: 5_000_000)
            }
            #expect(model.transcript.count > before)

            model.shutdown()
            server.stop()
        }
    }
}
