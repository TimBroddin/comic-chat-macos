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

// Final-review Important #4a/4b — the shared whisper-shape predicate
// (`ProtocolEvent.isWhisperShapedText`) `AppState.recomputeTranscriptText`
// filters `sessionEvents` with, and `ChatSessionModel.handleLocked`'s `.text`
// case uses for its own whisper-box routing decision. Pure logic, no
// engine/strip involvement (same posture as `TranscriptTextBuilderTests`
// above) — `AppState` itself lives in the app target (no test target exists
// there per this repo's structure), so this pins the SHARED predicate the fix
// depends on at the Kit level, plus an end-to-end demonstration of the exact
// filter `recomputeTranscriptText` applies to `sessionEvents` before handing
// events to `TranscriptTextBuilder`.
@Suite struct WhisperShapedTextPredicateTests {
    @Test func plainPrivmsgToOwnNickIsWhisperShaped() {
        let event = ProtocolEvent.text(nick: "Bob", ident: "b@h", target: "Mac", text: "psst",
                                       kind: 0, annotations: nil)
        #expect(ProtocolEvent.isWhisperShapedText(event, ownNick: "Mac"))
    }

    @Test func ownNickMatchIsCaseInsensitive() {
        let event = ProtocolEvent.text(nick: "Bob", ident: "b@h", target: "mac", text: "psst",
                                       kind: 0, annotations: nil)
        #expect(ProtocolEvent.isWhisperShapedText(event, ownNick: "Mac"))
    }

    @Test func cookedWhisperModeAnnotationIsWhisperShaped() {
        var annotations = Annotations()
        annotations.mode = 2   // SM_WHISPER
        let event = ProtocolEvent.text(nick: "Bob", ident: "b@h", target: "#room", text: "psst",
                                       kind: 0, annotations: annotations)
        #expect(ProtocolEvent.isWhisperShapedText(event, ownNick: "Mac"))
    }

    @Test func channelMessageToOthersIsNotWhisperShaped() {
        let event = ProtocolEvent.text(nick: "Bob", ident: "b@h", target: "#room", text: "hello all",
                                       kind: 0, annotations: nil)
        #expect(!ProtocolEvent.isWhisperShapedText(event, ownNick: "Mac"))
    }

    @Test func nonTextEventIsNeverWhisperShaped() {
        // `.whisper` is already unambiguous via its own case -- the predicate
        // only classifies `.text`-shaped events, per its own doc comment.
        let event = ProtocolEvent.whisper(nick: "Bob", ident: "b@h", text: "psst", annotations: nil)
        #expect(!ProtocolEvent.isWhisperShapedText(event, ownNick: "Mac"))
    }

    /// End-to-end demonstration of `AppState.recomputeTranscriptText`'s own
    /// filter (Important #4a): given a `sessionEvents`-shaped mix of a plain
    /// private whisper (`.text` to our own nick), an IRCX `.whisper`, and a
    /// genuine session-scoped line (`.statusLine`), only the status line
    /// should survive into what reaches `TranscriptTextBuilder` — the two
    /// whisper shapes must be dropped BEFORE the builder ever sees them
    /// (against the "whisper box, NOT text view" ruling), while a room's own
    /// `.text` (not whisper-shaped) still renders normally.
    @Test func recomputeTranscriptTextStyleFilterDropsWhisperShapedSessionEvents() {
        let ownNick = "Mac"
        let sessionEvents: [ProtocolEvent] = [
            .text(nick: "Bob", ident: "b@h", target: "Mac", text: "private aside", kind: 0, annotations: nil),
            .whisper(nick: "Carol", ident: "c@h", text: "ircx whisper", annotations: nil),
            .statusLine(text: "Connected to server"),
        ]
        // The exact filter `recomputeTranscriptText` applies.
        let filtered = sessionEvents.filter { event in
            if case .whisper = event { return false }
            if ProtocolEvent.isWhisperShapedText(event, ownNick: ownNick) { return false }
            return true
        }
        let result = String(TranscriptTextBuilder.attributedString(for: filtered).characters)
        #expect(!result.contains("private aside"), "a private plain-IRC whisper must not leak into the text view")
        #expect(!result.contains("ircx whisper"), "an IRCX whisper must not leak into the text view")
        #expect(result.contains("Connected to server"), "a genuine session-scoped status line must still render")
    }
}
