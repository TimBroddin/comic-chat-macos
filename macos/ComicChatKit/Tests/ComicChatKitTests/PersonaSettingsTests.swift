import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4b Task 5 — Settings scene + persona plumbing + character/backdrop
// pickers. Three things this suite proves:
//   1. SettingsStore round-trips every NEW key (realName/sendComicsData/
//      acceptWhispers/soundsEnabled/soundsFolder/comicMode/autoDownloadAvatars)
//      against an isolated UserDefaults suite, including "never set" defaults
//      (same `object(forKey:)` discrimination `port` already uses).
//   2. `ProtocolSession.init(..., userName:, realName:)` actually reaches the
//      wire: a loopback login test asserts the exact
//      "USER timb <host> . :Tim B" line (cc_session_login's grammar,
//      comicchat.h:688-708) — the real gap this task closes (ProtocolSession
//      never set cc_session_config.own_user/.own_realname before this task,
//      so USER always fell back to the nick).
//   3. `ChatSessionModel.changeCharacter` mid-session: after login/join + one
//      line, switch to a second bundled character, send another line, and
//      assert no crash, panelCount grew, and the announce ("# Appears as")
//      landed in the server-received bytes.
//
// CONFIG-STRING LIFETIME (the brief's read-first mandate): `cc_session_create`
// (bridge/cc_session.cpp:41-46) does `s->cfg = *cfg` — a STRUCT copy that
// copies the `const char*` POINTER VALUES, not the pointee bytes. `own_user`/
// `own_realname`/`local_host` are read later still, inside `cc_session_login`
// (cc_session.cpp:399-410) — which can fire an unbounded amount of async time
// after `cc_session_create` returns (the probe -> 451/800-pivot/timeout
// handshake), and can even be RE-entered (see that function's "REENTRANCY"
// doc comment). So a plain Swift local's `withCString` pointer (valid only
// for that closure's duration) does NOT suffice — this mirrors
// `ProtocolSession`'s existing `ownNickStorage`/`refillOwnNickBuffer()`
// pattern: a manually-`allocate`d, NUL-terminated buffer with a fixed address
// for the session's whole lifetime, freed only in `deinit`.
//
// SERIALIZATION: nested inside `EngineGlobalStateSelfTests` (.serialized),
// matching every other suite touching process-global engine state via
// ProtocolSession/ChatSessionModel.
extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct PersonaSettingsTests {
        // MARK: - Step 1a: SettingsStore round-trip (new keys)

        private func freshDefaults() -> UserDefaults {
            let suiteName = "p4b-t5-test-\(UUID().uuidString)"
            UserDefaults().removePersistentDomain(forName: suiteName)
            return UserDefaults(suiteName: suiteName)!
        }

        @Test func newKeysNeverSetDefaults() {
            let defaults = freshDefaults()
            let store = SettingsStore(defaults: defaults)
            #expect(store.realName == "")
            #expect(store.sendComicsData == true)
            #expect(store.acceptWhispers == true)
            #expect(store.soundsEnabled == true)
            #expect(!store.soundsFolder.isEmpty, "soundsFolder must default to the App Support path, not empty")
            #expect(store.comicMode == true)
            #expect(store.autoDownloadAvatars == true)
        }

        @Test func newKeysRoundTrip() {
            let defaults = freshDefaults()
            let store = SettingsStore(defaults: defaults)

            store.realName = "Tim B"
            store.sendComicsData = false
            store.acceptWhispers = false
            store.soundsEnabled = false
            store.soundsFolder = "/tmp/sounds"
            store.comicMode = false
            store.autoDownloadAvatars = false

            #expect(store.realName == "Tim B")
            #expect(store.sendComicsData == false)
            #expect(store.acceptWhispers == false)
            #expect(store.soundsEnabled == false)
            #expect(store.soundsFolder == "/tmp/sounds")
            #expect(store.comicMode == false)
            #expect(store.autoDownloadAvatars == false)

            // A second store instance over the SAME defaults sees the
            // persisted values -- proves round-tripping through UserDefaults,
            // not just an in-memory copy (mirrors SettingsStoreTests'
            // existing `reloaded` check).
            let reloaded = SettingsStore(defaults: defaults)
            #expect(reloaded.realName == "Tim B")
            #expect(reloaded.sendComicsData == false)
            #expect(reloaded.acceptWhispers == false)
            #expect(reloaded.soundsEnabled == false)
            #expect(reloaded.soundsFolder == "/tmp/sounds")
            #expect(reloaded.comicMode == false)
            #expect(reloaded.autoDownloadAvatars == false)
        }

        // MARK: - Step 1b: persona wire test (USER line)

        /// Polls `server.receivedBytes` until it contains `substring`.
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

        /// Real gap this task closes: `ProtocolSession` never plumbed
        /// `userName`/`realName` into `cc_session_config.own_user`/
        /// `.own_realname` before this task -- USER always fell back to the
        /// nick. This drives a real login handshake (plain-IRC 451 fallback,
        /// same scenario as LoginSequencingTests.plainIrcFallback) with
        /// `userName: "timb", realName: "Tim B"` and asserts the exact wire
        /// grammar `cc_session_login` builds: "USER <user> <host> . :<realname>"
        /// (comicchat.h:688-708, verified byte-for-byte against the real 1998
        /// capture's c2s line in that function's doc comment). `<host>`
        /// itself is whatever `local_host` resolves to (today `nil` ->
        /// "localhost", cc_session.cpp:407) -- not asserted here since this
        /// task does not plumb a host override, only user/realname.
        @Test(.timeLimit(.minutes(1)))
        func personaUserAndRealNameReachTheWire() async throws {
            let server = try LoopbackIRCServer()
            let session = ProtocolSession(host: "127.0.0.1", port: server.port, nick: "timb",
                                          encoding: .cp1252, userName: "timb", realName: "Tim B")
            var iterator = session.events.makeAsyncIterator()

            try await session.connect()
            await server.startCollectingReceivedBytes()
            _ = try await waitForReceivedBytes(server, containing: "MODE ISIRCX\r\n")
            try await server.send(":srv 451 * :not registered")

            let afterLogin = try await waitForReceivedBytes(server, containing: "USER ")
            #expect(afterLogin.contains("USER timb localhost . :Tim B\r\n"),
                    "expected the exact USER grammar with plumbed user/realname; got: \(afterLogin)")

            try await server.send(":srv 001 timb :Welcome")
            while true {
                guard let ev = await iterator.next() else { break }
                if case .loggedIn = ev { break }
            }

            session.disconnect()
            server.stop()
        }

        // MARK: - Step 1c: changeCharacter mid-session

        /// Polls `server.receivedBytes` until it contains `substring`,
        /// returning the accumulated c2s bytes split into lines -- same shape
        /// as ChatSessionModelTests.waitForReceivedLine.
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

        /// After login/join + one line, switch the SELF character to a
        /// second bundled avatar (armando), send another line, and assert:
        /// no crash, `panelCount` grew (a new panel was actually rendered
        /// after the switch), and the announce-on-change
        /// (`session.announceAvatar`, mirroring SetMyAvatar's
        /// announce-on-change, avatar.cpp:585-599) reached the server as a
        /// "# Appears as Armando" PRIVMSG.
        @Test(.timeLimit(.minutes(1)))
        func changeCharacterMidSessionSwitchesAvatarAndAnnounces() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.send("first line")
            _ = try await waitForReceivedLine(server, containing: "first line")

            let panelsBeforeSwitch = model.panelCount

            model.changeCharacter("armando")

            // settle the engine-queue hop `changeCharacter` fires (mirrors
            // `settleEngineQueue`'s existing sentinel-hop technique).
            await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                model.settleEngineQueue { cont.resume() }
            }

            try await model.send("second line")
            let afterSwitch = try await waitForReceivedLine(server, containing: "second line")

            #expect(afterSwitch.contains { $0.contains("# Appears as Armando") },
                    "expected the announce-on-change to reach the wire; got: \(afterSwitch)")
            #expect(model.panelCount > panelsBeforeSwitch,
                    "expected a new panel to have been rendered after the character switch + second send")

            model.shutdown()
            server.stop()
        }
    }
}
