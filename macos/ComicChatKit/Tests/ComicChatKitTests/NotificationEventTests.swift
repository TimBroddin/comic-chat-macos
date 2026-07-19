import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Batch B — TDD for the Kit halves of "mention + whisper
// notifications" and "away-return". `ChatSessionModel.onNotificationEvent`
// is engine-queue-detected and fires on the MAIN thread (mirrors every other
// callback on this type); `ProtocolSession`'s `.awayPeer` handling is what
// backs `MemberRow.isAway` clearing.
//
// SERIALIZATION: nested inside `EngineGlobalStateSelfTests` (.serialized),
// matching every other suite that drives a live `ChatSessionModel`/
// `ProtocolSession` against the shared engine global state (see
// `ChatSessionModelTests`/`WhisperRoutingTests`'s own top doc comments).

/// Thread-safe accumulator for `onNotificationEvent`'s captured
/// `[NotificationEvent]` — same lock-guarded-box shape as
/// `ChatSessionModelTests.MembersBox` (the callback fires on the main thread
/// via `DispatchQueue.main.async`, while the test body polls it from its own
/// task).
private final class NotificationBox: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [NotificationEvent] = []

    func append(_ event: NotificationEvent) {
        lock.lock(); defer { lock.unlock() }
        storage.append(event)
    }

    var count: Int {
        lock.lock(); defer { lock.unlock() }
        return storage.count
    }

    var all: [NotificationEvent] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
}

extension EngineGlobalStateSelfTests {
    @Suite(.serialized)
    struct NotificationEventTests {
        /// A room `.text` whose body contains our own nick (case-folded)
        /// fires `.mention(room:)` with the sender's nick and the raw text.
        @Test(.timeLimit(.minutes(1)))
        func channelMessageMentioningOwnNickFiresMentionEvent() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let notifications = NotificationBox()
            model.onNotificationEvent = { notifications.append($0) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            // Case-folded contains: "MAC" (all-caps) still matches "Mac".
            try await server.send(":Bob!u@h PRIVMSG #p4 :hey MAC, over here")

            var attempts = 0
            while notifications.count == 0, attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(notifications.count == 1)
            if let event = notifications.all.first {
                #expect(event.fromNick == "Bob")
                #expect(event.text == "hey MAC, over here")
                #expect(event.kind == .mention(room: "#p4"))
            }

            model.shutdown()
            server.stop()
        }

        /// A channel message with NO mention fires nothing.
        @Test(.timeLimit(.minutes(1)))
        func channelMessageWithoutMentionFiresNothing() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let notifications = NotificationBox()
            model.onNotificationEvent = { notifications.append($0) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!u@h PRIVMSG #p4 :just chatting, nothing to see")

            // Poll the transcript growing (a signal the event was processed)
            // rather than a fixed sleep, then assert no notification fired.
            var attempts = 0
            while model.transcript.count < 2, attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(notifications.count == 0)

            model.shutdown()
            server.stop()
        }

        /// An ignored nick's mention does NOT fire a notification (mirrors
        /// the ignore doctrine's render-routing guard — `ignoredNicks` is a
        /// view-side filter, and notifications are a "did I need to look at
        /// this" signal, same posture as unread/strip rendering).
        @Test(.timeLimit(.minutes(1)))
        func ignoredNickMentionFiresNothing() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let notifications = NotificationBox()
            model.onNotificationEvent = { notifications.append($0) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")
            model.setIgnored("Bob", true)

            func settle() async {
                await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                    model.settleEngineQueue { cont.resume() }
                }
            }
            await settle()

            try await server.send(":Bob!u@h PRIVMSG #p4 :hey Mac")
            var attempts = 0
            while model.transcript.count < 2, attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(notifications.count == 0)

            model.shutdown()
            server.stop()
        }

        /// An inbound whisper (the IRCX `WHISPER` verb form) fires
        /// `.whisper`, not `.mention` — even when the whisper text itself
        /// contains our nick (whisper detection runs BEFORE the mention
        /// check, `handleLocked`'s `.text` case doc comment).
        @Test(.timeLimit(.minutes(1)))
        func inboundWhisperFiresWhisperEvent() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let notifications = NotificationBox()
            model.onNotificationEvent = { notifications.append($0) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!u@h WHISPER #p4 Mac :psst")

            var attempts = 0
            while notifications.count == 0, attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(notifications.count == 1)
            if let event = notifications.all.first {
                #expect(event.fromNick == "Bob")
                #expect(event.text == "psst")
                #expect(event.kind == .whisper)
            }

            model.shutdown()
            server.stop()
        }

        /// The plain-IRC whisper wire form (`PRIVMSG <ourNick> :text`, no
        /// channel prefix — §8 Topology A) ALSO fires `.whisper`, matching
        /// `WhisperRoutingTests.plainPrivmsgToSelfRoutesToWhisperBox`'s own
        /// wire-classification finding.
        @Test(.timeLimit(.minutes(1)))
        func plainPrivmsgWhisperFiresWhisperEvent() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let notifications = NotificationBox()
            model.onNotificationEvent = { notifications.append($0) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await server.send(":Bob!u@h PRIVMSG Mac :psst")

            var attempts = 0
            while notifications.count == 0, attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(notifications.count == 1)
            if let event = notifications.all.first {
                #expect(event.fromNick == "Bob")
                #expect(event.kind == .whisper)
            }

            model.shutdown()
            server.stop()
        }

        /// Our OWN say (rendered via `send(_:)`'s synthetic local echo, never
        /// `fromServer`) never fires a notification even if it happens to
        /// contain our own nick.
        @Test(.timeLimit(.minutes(1)))
        func ownSayNeverFiresNotification() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let notifications = NotificationBox()
            model.onNotificationEvent = { notifications.append($0) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(nick: "Mac", channel: "#p4")

            try await model.send("hi, this is Mac speaking")

            var attempts = 0
            while model.transcript.count < 2, attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(notifications.count == 0)

            model.shutdown()
            server.stop()
        }

        // MARK: Away-return (READ-FIRST finding, Batch B)

        /// The wire-level peer-away CTCP marker (`\x01AWAY <msg>\x01`,
        /// `protsupp.cpp`'s ported `ccPayloadAwayPeer` parse — verified
        /// against the ENGINE, not assumed) already carries BOTH directions:
        /// a non-empty message means away, an EMPTY message (`\x01AWAY\x01`,
        /// the wire form `ChatSetAway(bAway: FALSE, "")` sends on RETURN)
        /// means back. This test drives both and asserts `MemberRow.isAway`
        /// flips both ways — the bug this closes: `ProtocolSession`'s
        /// `.awayPeer` handler used to unconditionally set `isAway = true`
        /// regardless of the message, silently dropping the "back" signal.
        @Test(.timeLimit(.minutes(1)))
        func peerAwayThenReturnClearsIsAway() async throws {
            let server = try LoopbackIRCServer()
            let art = repoRoot5Up().appendingPathComponent("v2.5-beta-1-modern/comicart").path
            let model = ChatSessionModel(config: .init(host: "127.0.0.1", port: server.port,
                                                       nick: "Mac", room: "#p4", artDir: art))
            let members = NotificationBox2()
            model.onMembers = { rows in members.set(rows) }
            try await model.start()
            try await server.replyToProbeWith451ThenWelcomeAndJoin(
                nick: "Mac", channel: "#p4", otherMembers: "Bob")

            func settle() async {
                await withCheckedContinuation { (cont: CheckedContinuation<Void, Never>) in
                    model.settleEngineQueue { cont.resume() }
                }
                try? await Task.sleep(nanoseconds: 100_000_000)
            }
            await settle()

            // Bob goes away.
            try await server.send(":Bob!u@h PRIVMSG #p4 :\u{01}AWAY gone fishing\u{01}")
            var attempts = 0
            while !(members.rows().first { $0.nick == "Bob" }?.isAway ?? false), attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(members.rows().first { $0.nick == "Bob" }?.isAway == true,
                    "expected the peer AWAY marker (non-empty message) to set isAway")

            // Bob returns: the SAME marker, empty message.
            try await server.send(":Bob!u@h PRIVMSG #p4 :\u{01}AWAY\u{01}")
            attempts = 0
            while (members.rows().first { $0.nick == "Bob" }?.isAway ?? true), attempts < 400 {
                try await Task.sleep(nanoseconds: 25_000_000)
                attempts += 1
            }
            #expect(members.rows().first { $0.nick == "Bob" }?.isAway == false,
                    "expected the peer's RETURN marker (empty message) to clear isAway")

            model.shutdown()
            server.stop()
        }
    }
}

/// Thread-safe `[MemberRow]` snapshot box — same shape as
/// `ChatSessionModelTests.MembersBox`, kept separate (not `@testable`-shared
/// across files) since that one is `private` to its own file.
private final class NotificationBox2: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: [MemberRow] = []

    func set(_ rows: [MemberRow]) {
        lock.lock(); defer { lock.unlock() }
        storage = rows
    }

    func rows() -> [MemberRow] {
        lock.lock(); defer { lock.unlock() }
        return storage
    }
}
