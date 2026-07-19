import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Task 11: the plain-text transcript view's pure builder —
// `[ProtocolEvent] -> AttributedString`, one assertion per styled kind (the
// brief's Step 1). No engine/strip involvement (pure event -> string
// formatting), so these run with no serialization/fixture setup unlike the
// engine-touching suites in this file's siblings.
@Suite struct TranscriptTextBuilderTests {

    // MARK: say (.text, plain "nick: text")

    @Test func sayRendersNickColonText() {
        let events: [ProtocolEvent] = [
            .text(nick: "Alice", ident: "alice@host", target: "#room", text: "hello there",
                  kind: 0, annotations: nil)
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        #expect(result.contains("Alice: hello there"))
    }

    // MARK: whisper (italic secondary "nick whispers: text")

    @Test func whisperRendersNickWhispersText() {
        let events: [ProtocolEvent] = [
            .whisper(nick: "Bob", ident: "bob@host", text: "psst", annotations: nil)
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        #expect(result.contains("Bob whispers: psst"))
    }

    // MARK: action (italic "• text" — the engine ALREADY prepends the nick
    // into the text field, ccPrepareTextAction (protsupp.cpp:1265-1274,
    // verified against ChatSessionModelTests.swift:213's
    // `.action(nick: "Bob", text: "Bob waves", ...)` fixture) — so the
    // builder must NOT prepend the nick a second time, just the bullet.

    @Test func actionRendersBulletTextVerbatimNoDoublePrepend() {
        let events: [ProtocolEvent] = [
            .action(nick: "Bob", text: "Bob waves", annotations: nil)
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        #expect(result.contains("• Bob waves"))
        // Guard against double-prepending ("Bob Bob waves").
        #expect(!result.contains("Bob Bob waves"))
    }

    // MARK: sound ("♪ nick played file")

    @Test func soundRendersNotePrefix() {
        let events: [ProtocolEvent] = [
            .sound(nick: "Carol", file: "boing.wav", text: "boing")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        #expect(result.contains("♪ Carol played boing.wav"))
    }

    // MARK: arrivals ("→ nick joined" / "← nick left"), secondary, gated on
    // showArrivals.

    @Test func arrivalsRenderJoinedAndLeftWhenShown() {
        let events: [ProtocolEvent] = [
            .userJoined(nick: "Dave", ident: "dave@host"),
            .userParted(nick: "Dave", reason: "bye")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events, showArrivals: true).characters)
        #expect(result.contains("→ Dave joined"))
        #expect(result.contains("← Dave left"))
    }

    @Test func arrivalsSuppressedWhenShowArrivalsFalse() {
        let events: [ProtocolEvent] = [
            .userJoined(nick: "Dave", ident: "dave@host"),
            .userParted(nick: "Dave", reason: "bye"),
            .userQuit(nick: "Erin", reason: "quit")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events, showArrivals: false).characters)
        #expect(!result.contains("Dave"))
        #expect(!result.contains("Erin"))
        #expect(result.isEmpty)
    }

    @Test func userQuitTreatedAsArrivalLeft() {
        let events: [ProtocolEvent] = [
            .userQuit(nick: "Erin", reason: "connection reset")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events, showArrivals: true).characters)
        #expect(result.contains("← Erin left"))
    }

    // MARK: status/errors (secondary, verbatim text)

    @Test func statusLineRendersVerbatim() {
        let events: [ProtocolEvent] = [
            .statusLine(text: "Connected to server")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        #expect(result.contains("Connected to server"))
    }

    @Test func errorRendersVerbatimText() {
        let events: [ProtocolEvent] = [
            .error(code: 421, text: "Unknown command")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        #expect(result.contains("Unknown command"))
    }

    // MARK: multi-event ordering — the builder must preserve input order and
    // separate lines (one event per line).

    @Test func multipleEventsProduceOneLineEach() {
        let events: [ProtocolEvent] = [
            .text(nick: "Alice", ident: "a@h", target: "#r", text: "hi", kind: 0, annotations: nil),
            .action(nick: "Bob", text: "Bob waves", annotations: nil),
            .sound(nick: "Carol", file: "x.wav", text: "x")
        ]
        let result = String(TranscriptTextBuilder.attributedString(for: events).characters)
        let lines = result.split(separator: "\n", omittingEmptySubsequences: true)
        #expect(lines.count == 3)
        #expect(lines[0].contains("Alice: hi"))
        #expect(lines[1].contains("• Bob waves"))
        #expect(lines[2].contains("♪ Carol played x.wav"))
    }

    @Test func emptyEventsProduceEmptyString() {
        let result = TranscriptTextBuilder.attributedString(for: [])
        #expect(String(result.characters).isEmpty)
    }
}
