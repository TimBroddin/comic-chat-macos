import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4a carryover, paid off here (Plan 4b Task 10 brief, Step 3/4): a fixed
// `ConversationFile` rendered TWICE through separate `TranscriptRenderer`
// instances at IDENTICAL geometry must produce a byte-identical PNG.
//
// LEGALITY (binding, read this before questioning the byte-equal assertion):
// this is REAL-vs-REAL within one process run, NOT a frozen golden fixture
// compared against a value baked in on a different machine/OS/font build.
// `RealMetricsTests`' own doc comment explains why a golden-PNG comparison
// across environments would be illegitimate (CoreText metrics are
// OS-dependent) — this test sidesteps that entirely by rendering the SAME
// transcript twice IN THE SAME RUN and comparing the two outputs to each
// other, never to a value committed to disk. `Strip.init`'s doc comment
// ("DETERMINISM: cc_strip_create seeds the global rand() stream") is exactly
// the guarantee this test exercises: two independent strips built from the
// same script must consume the RNG identically and lay out identically,
// regardless of what any OTHER test happened to do to rand() first.
//
// SERIALIZATION (load-bearing, same reasoning as StripTests/EngineInterleaveTests/
// RealMetricsTests): nested inside `EngineGlobalStateSelfTests` (`.serialized`,
// declared in BodyDrawTests.swift) so the two renders in this test — and this
// test itself relative to every other engine-global-state suite — never race
// another `Strip`/metrics-canvas registration in the same process. Each
// `TranscriptRenderer.render` call is its own complete, torn-down engine
// occupation (that type's own doc comment), so the two calls below are safe
// to run back-to-back even serialized on the SAME thread.
extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct ReflowDeterminismTests {
    /// A fixed, hand-built `ConversationFile` — six events spanning the
    /// shapes a real save would carry (join, cooked text, an addressed cooked
    /// line, a whisper, an appearsAs switch, another cooked line after the
    /// switch) so the determinism claim isn't resting on a single trivial
    /// panel.
    private func fixedFile() -> ConversationFile {
        let cookedAnn = Annotations(gesturePose: 2, gestureEmotion: 1, gestureIntensity: 70,
                                    facePose: 1, faceEmotion: 3, faceIntensity: 55,
                                    requested: false, mode: 1, addressees: ["Boris"],
                                    cooked: true)
        let secondAnn = Annotations(gesturePose: 4, gestureEmotion: 2, gestureIntensity: 40,
                                    facePose: 2, faceEmotion: 1, faceIntensity: 30,
                                    requested: false, mode: 1, addressees: [],
                                    cooked: true)
        return ConversationFile(
            host: "irc.example.com", room: "#comicrig", nick: "Anon",
            characterName: "anna", backdropName: "field",
            encodingRaw: WireEncoding.cp1252.rawValue,
            events: [
                .selfJoined(channel: "#comicrig"),
                .userJoined(nick: "Boris", ident: "boris@example.com"),
                .text(nick: "Anon", ident: "anon@example.com", target: "#comicrig",
                     text: "Hello there my good friend, how are you today?", kind: 1,
                     annotations: cookedAnn),
                .whisper(nick: "Boris", ident: "boris@example.com", text: "over here",
                         annotations: nil),
                .appearsAs(nick: "Boris", avatarName: "Armando", url: ""),
                .text(nick: "Boris", ident: "boris@example.com", target: "#comicrig",
                     text: "Look, a new face!", kind: 1, annotations: secondAnn),
            ])
    }

    private func fixtureArtDir() -> String {
        // fixture(_:) returns a path to a specific file inside
        // Fixtures/ (ArtTests.swift's helper) -- strip back to the directory
        // itself, since TranscriptRenderer wants a DIRECTORY (it appends
        // "<name>.avb"/"<name>.bgb" itself, matching setUpStripLocked's own
        // artDir convention).
        (fixture("anna.avb") as NSString).deletingLastPathComponent
    }

    @Test func sameTranscriptSameGeometryProducesByteIdenticalPNG() throws {
        let file = fixedFile()
        let artDir = fixtureArtDir()

        let renderer1 = TranscriptRenderer(file: file, artDir: artDir)
        let (_, png1) = try renderer1.render(columns: 3, unitTwips: PanelFit.minUnitPanelWidth, scale: 2.0)

        let renderer2 = TranscriptRenderer(file: file, artDir: artDir)
        let (_, png2) = try renderer2.render(columns: 3, unitTwips: PanelFit.minUnitPanelWidth, scale: 2.0)

        #expect(!png1.isEmpty)
        #expect(png1.count == png2.count)
        // Precompute the equality as a Bool before handing it to #expect
        // (Task 5 doctrine-test precedent): Swift Testing's value-diffing for
        // a failed #expect on two large Data blobs runs a Myers-diff style
        // comparison that is pathologically slow (effectively hangs) on
        // multi-KB binary payloads -- comparing a plain Bool sidesteps that
        // entirely while still failing loudly (with sizes) if the render
        // ever goes nondeterministic.
        let identical = png1 == png2
        #expect(identical, "two TranscriptRenderer instances rendering the SAME ConversationFile at IDENTICAL geometry produced different PNG bytes (png1: \(png1.count) bytes, png2: \(png2.count) bytes) -- engine determinism (cc_strip_create's srand(0x5EED) seed) is broken")
    }

    /// A second geometry (different column count) must ALSO be internally
    /// deterministic (two renders of the SAME (transcript, geometry) pair
    /// agree), even though it need not match the first geometry's own bytes
    /// -- guards against a renderer that's only "accidentally" deterministic
    /// at the default 3-column width.
    @Test func differentGeometryStillDeterministicWithinItself() throws {
        let file = fixedFile()
        let artDir = fixtureArtDir()

        func render(_ r: TranscriptRenderer) throws -> Data {
            try r.render(columns: 2, unitTwips: PanelFit.minUnitPanelWidth, scale: 1.0).pngData
        }
        let png1 = try render(TranscriptRenderer(file: file, artDir: artDir))
        let png2 = try render(TranscriptRenderer(file: file, artDir: artDir))

        let identical = png1 == png2
        #expect(identical, "2-column render was not internally deterministic across two TranscriptRenderer instances")
    }
  }
}
