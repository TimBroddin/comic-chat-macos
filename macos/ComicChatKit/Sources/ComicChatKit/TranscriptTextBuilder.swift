import Foundation
import SwiftUI

/// Plan 4b Task 11 (D1 §1.2): a PURE `[ProtocolEvent] -> AttributedString`
/// formatter for the plain-text transcript view — the same per-room event
/// log `ComicStripView`'s strip renders, laid out as an `NSTextView`-ready
/// scrollback instead of comic panels. No engine/strip touched here (unlike
/// `TranscriptRenderer`, Task 10's OFFLINE comic-image reopen path): this is
/// a straight string formatter over already-decoded `ProtocolEvent` payloads,
/// so it is NOT serialized against the engine queue and is fully
/// unit-testable with plain fixtures.
///
/// Per-kind styling (brief's table):
///   - `.text` (say):     `nick: text` — plain.
///   - `.whisper`:        `nick whispers: text` — italic, secondary.
///   - `.action`:         `• text` — italic. NOTE: the engine's
///     `ccPrepareTextAction` (protsupp.cpp:1265-1274) ALREADY prepends the
///     nick into the action's `text` field before it ever crosses the C
///     boundary (verified against the existing fixture in
///     `ChatSessionModelTests.swift:213`: `.action(nick: "Bob", text: "Bob
///     waves", ...)`) — so this builder does NOT prepend `nick` a second
///     time, it only adds the bullet.
///   - `.sound`:          `♪ nick played file` — plain.
///   - `.userJoined`:     `→ nick joined` — secondary, gated on `showArrivals`.
///   - `.userParted`/`.userQuit`: `← nick left` — secondary, gated on
///     `showArrivals` (a quit is a departure from the room's point of view,
///     same bucket as a part — the transcript doesn't distinguish "why" any
///     more than the arrival glyph does).
///   - `.statusLine`/`.error`: verbatim text — secondary.
///   - everything else (membership plumbing like `.names`/`.endOfNames`,
///     room-state events like `.topicChanged`, WHOIS/WHO results, etc.) is
///     silently skipped — those are not user-facing transcript lines in the
///     original UI either (they drive sidebar/status state, not scrollback).
public enum TranscriptTextBuilder {
    /// Builds the full transcript as one `AttributedString`, one line (`\n`-
    /// terminated, except the last) per rendered event, in input order.
    ///
    /// - Parameters:
    ///   - events: the event log to render — typically a room's transcript
    ///     plus the session-scoped events relevant to it (whispers arrive
    ///     pre-mixed into the room transcript already when room-scoped; see
    ///     `ChatSessionModel.sessionEvents`'s own doc comment for what's
    ///     session- vs room-scoped).
    ///   - showArrivals: when `false`, join/part/quit lines are omitted
    ///     entirely (not even a blank line) — default `true`.
    public static func attributedString(for events: [ProtocolEvent], showArrivals: Bool = true) -> AttributedString {
        var result = AttributedString()
        var first = true
        for event in events {
            guard let line = line(for: event, showArrivals: showArrivals) else { continue }
            if !first { result += AttributedString("\n") }
            result += line
            first = false
        }
        return result
    }

    /// Builds one line's `AttributedString` for a single event, or `nil` if
    /// this event kind has no transcript representation (either it's not a
    /// display-worthy event at all, or it's an arrival/departure suppressed
    /// by `showArrivals`).
    private static func line(for event: ProtocolEvent, showArrivals: Bool) -> AttributedString? {
        switch event {
        case .text(let nick, _, _, let text, _, _):
            var s = AttributedString("\(nick): \(text)")
            s.foregroundColor = .primary
            return s

        case .whisper(let nick, _, let text, _):
            var s = AttributedString("\(nick) whispers: \(text)")
            s.foregroundColor = .secondary
            s.font = .body.italic()
            return s

        case .action(_, let text, _):
            // `text` already has the nick prepended by the engine — see the
            // type's own doc comment. Do NOT prepend it again here.
            var s = AttributedString("• \(text)")
            s.font = .body.italic()
            return s

        case .sound(let nick, let file, _):
            var s = AttributedString("♪ \(nick) played \(file)")
            s.foregroundColor = .primary
            return s

        case .userJoined(let nick, _):
            guard showArrivals else { return nil }
            var s = AttributedString("→ \(nick) joined")
            s.foregroundColor = .secondary
            return s

        case .userParted(let nick, _):
            guard showArrivals else { return nil }
            var s = AttributedString("← \(nick) left")
            s.foregroundColor = .secondary
            return s

        case .userQuit(let nick, _):
            guard showArrivals else { return nil }
            var s = AttributedString("← \(nick) left")
            s.foregroundColor = .secondary
            return s

        case .statusLine(let text):
            var s = AttributedString(text)
            s.foregroundColor = .secondary
            return s

        case .error(_, let text):
            var s = AttributedString(text)
            s.foregroundColor = .secondary
            return s

        default:
            // Membership/room-state/session plumbing (.names, .endOfNames,
            // .topicChanged, .channelMode, .userMode, .roomProp, room-list
            // events, WHOIS/WHO results, .motd, .nickRejected,
            // .authUnsupported, .serverCaps, .loggedIn, .disconnectedHint,
            // .selfJoined/.selfParted, .kicked, .invited, .nickChanged,
            // .data, .awayPeer, .appearsAs) — none of these are transcript
            // scrollback lines in the original UI; they drive other UI state
            // (sidebar, status line, popovers) instead.
            return nil
        }
    }
}
