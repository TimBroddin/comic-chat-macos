import Testing
import Foundation
@testable import ComicChatKit

// Plan 4b Task 10, Step 1: `ConversationFile` round-trip via `Codable`. Pure
// data — NOT `.serialized` (unlike `ReflowDeterminismTests`, this suite
// touches no engine-global state at all: no `Strip`, no metrics canvas, no
// `cc_*` call anywhere). `Annotations`/`ProtocolEvent` are synthesized
// `Codable` (enums with associated values synthesize since Swift 5.5, per
// the task brief) — this test is what proves that synthesis actually round-
// trips correctly for a representative event mix, not just that it compiles.
@Suite
struct ConversationFileTests {
    /// A representative event array: text with cooked annotations, a
    /// whisper, an appearsAs, a sound, and joins — one of each shape the
    /// brief calls out, so the round-trip exercises every associated-value
    /// case that resists trivial synthesis (nested `Annotations`, optional
    /// `Annotations?`, arrays, bools).
    private func representativeEvents() -> [ProtocolEvent] {
        let cookedAnn = Annotations(gesturePose: 3, gestureEmotion: 2, gestureIntensity: 80,
                                    facePose: 1, faceEmotion: 4, faceIntensity: 60,
                                    requested: true, mode: 1, addressees: ["Boris"],
                                    cooked: true)
        return [
            .selfJoined(channel: "#comicrig"),
            .userJoined(nick: "Boris", ident: "boris@example.com"),
            .text(nick: "Anon", ident: "anon@example.com", target: "#comicrig",
                 text: "Hello there, cooked and ready.", kind: 1,
                 annotations: cookedAnn),
            .whisper(nick: "Boris", ident: "boris@example.com", text: "psst, over here",
                     annotations: nil),
            .appearsAs(nick: "Boris", avatarName: "Armando", url: "https://example.com/armando.avb"),
            .sound(nick: "Anon", file: "tada.wav", text: "*tada*"),
        ]
    }

    private func makeFile() -> ConversationFile {
        ConversationFile(host: "irc.example.com", room: "#comicrig", nick: "Anon",
                         characterName: "anna", backdropName: "field",
                         encodingRaw: WireEncoding.cp1252.rawValue,
                         events: representativeEvents())
    }

    @Test func roundTripsThroughJSONDecoder() throws {
        let original = makeFile()
        let data = try JSONEncoder().encode(original)
        let decoded = try JSONDecoder().decode(ConversationFile.self, from: data)
        #expect(decoded == original)
        #expect(decoded.formatVersion == 1)
        #expect(decoded.events.count == original.events.count)
    }

    @Test func roundTripsThroughReadWrite() throws {
        let original = makeFile()
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("cc-conversation-\(UUID().uuidString).json")
        defer { try? FileManager.default.removeItem(at: url) }

        try original.write(to: url)
        let decoded = try ConversationFile.read(from: url)
        #expect(decoded == original)

        // Pretty-printed JSON, per write(to:)'s doc comment -- a saved file
        // is human-readable, not a single minified line.
        let text = try String(contentsOf: url, encoding: .utf8)
        #expect(text.contains("\n"))
    }

    @Test func eachEventCaseSurvivesIndividually() throws {
        // One event per case in representativeEvents(), decoded and compared
        // individually -- pins down exactly which case would fail if a future
        // ProtocolEvent case addition ever broke synthesis (rather than only
        // asserting the whole-array equality above).
        for event in representativeEvents() {
            let data = try JSONEncoder().encode(event)
            let decoded = try JSONDecoder().decode(ProtocolEvent.self, from: data)
            #expect(decoded == event)
        }
    }

    @Test func annotationsRoundTrip() throws {
        let ann = Annotations(gesturePose: 3, gestureEmotion: 2, gestureIntensity: 80,
                              facePose: 1, faceEmotion: 4, faceIntensity: 60,
                              requested: true, mode: 1, addressees: ["Boris", "Anna"],
                              cooked: true)
        let data = try JSONEncoder().encode(ann)
        let decoded = try JSONDecoder().decode(Annotations.self, from: data)
        #expect(decoded == ann)
    }
}
