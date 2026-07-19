import Foundation
import CoreGraphics
import cchat_engine

/// Plan 4b Task 10: renders a saved `ConversationFile` back into a comic
/// strip image OFFLINE — no `ProtocolSession`, no network, no live
/// `ChatSessionModel`. This is the "reopen" half of save/reopen: the
/// transcript IS the event log (spec §5's deviation), so reopening a saved
/// conversation is exactly replaying that log through a fresh `Strip`, the
/// same "replay is the reflow" mechanics `ChatSessionModel.rebuildStripLocked`
/// already uses for a live session's viewport reflow — just against a
/// standalone strip instead of the live one.
///
/// ONE-STRIP-AT-A-TIME (binding, plan-review DECISION): the engine's
/// process-global state (comicchat.h's single-thread contract, `Strip`'s own
/// doc comment) tolerates exactly one live `Strip` in the process at a time.
/// `render(...)` is therefore engineered as ONE ATOMIC ENGINE OCCUPATION: it
/// installs its own metrics canvas, builds a fresh `Strip`, does everything,
/// and TEARS DOWN COMPLETELY (strip close + `cc_set_metrics_canvas(nil)` +
/// releasing the canvas box) BEFORE `render` returns — including on a thrown
/// error (`defer`, so a partial failure never leaves the metrics-canvas
/// registration dangling for whatever runs next). This is safe ONLY when no
/// OTHER `Strip`/`ChatSessionModel` is live in the process at the same time —
/// `TranscriptRenderer` itself has no way to enforce that (it has no
/// visibility into whether some other object is mid-render), so the CALLER
/// must gate it: the app refuses to open a saved transcript while
/// `AppState.model != nil` (an alert: "Disconnect first" — a second
/// concurrent strip is engine UB, not a UI choice, per the plan-review
/// decision), and tests must `shutdown()` any live `ChatSessionModel` before
/// constructing a `TranscriptRenderer`.
///
/// THREADING: `render` runs synchronously on ITS OWN private serial queue
/// (`renderQueue`), matching every other engine-touching type's "one serial
/// queue for every `cc_*` call" discipline (`ProtocolStripBridge`'s own doc
/// comment) — but this queue is NOT shared with any `ChatSessionModel`'s
/// `engineQueue`. That is the one sanctioned place in this codebase a SECOND
/// engine-touching queue exists: it is safe only because the one-strip-at-a-
/// time gate above ensures the two queues are never BOTH occupying the engine
/// at once (a live session's `engineQueue` is idle — there is no live
/// session — for the entire span this renderer's queue is active). Callers
/// still must not call `render` from a live session's own engine queue (that
/// would defeat the gate, not just risk a data race).
public final class TranscriptRenderer {
    public struct RenderError: Error, CustomStringConvertible {
        public let message: String
        public var description: String { message }
    }

    private let file: ConversationFile
    private let artDir: String
    private let renderQueue = DispatchQueue(label: "com.comicchat.transcript-renderer")

    /// - Parameters:
    ///   - file: the saved conversation to replay.
    ///   - artDir: the comicart directory to resolve `file.characterName`/
    ///     `file.backdropName`/any `.appearsAs`-announced avatar names
    ///     against — same resolution order as a live session
    ///     (`ChatSessionModel.setUpStripLocked`'s `AvatarResolver`:
    ///     `userCharactersDir` before `artDir`, D1 §4.3), so a reopened
    ///     transcript shows any since-downloaded custom avatars exactly like
    ///     a live reflow would.
    public init(file: ConversationFile, artDir: String) {
        self.file = file
        self.artDir = artDir
    }

    /// Replays `file.events` into a fresh strip at the given panel geometry
    /// and composes it. Mirrors `ChatSessionModel.setUpStripLocked`'s exact
    /// sequence (geometry BEFORE any line, then title, then backdrop, then
    /// the self participant, then the bridge) so a reopened transcript lays
    /// out identically to how it would have live at the same width:
    ///
    ///   1. install a metrics canvas (`CTMetricsCanvas` + `CanvasBox`,
    ///      mirroring `setUpStripLocked`'s retention discipline — the box
    ///      must live for the WHOLE render, not just the registration call);
    ///   2. create a fresh `Strip` and set its panel geometry
    ///      (`columns`/`unitTwips`) BEFORE any line is added (`Strip.setPanelGeometry`'s
    ///      own "fresh strip only" contract);
    ///   3. set the title to `file.room` (panel 0, matching a live session's
    ///      `setTitle(activeRoom)`);
    ///   4. set the backdrop from `file.backdropName`;
    ///   5. add the SELF participant seeded from `file.characterName` and
    ///      register it with the bridge (`preRegisterSelfParticipant`, so a
    ///      transcript `.appearsAs`/`.userJoined` for our OWN nick resolves to
    ///      this SAME participant rather than a second one — mirrors
    ///      `setUpStripLocked`'s own reasoning);
    ///   6. build the `ProtocolStripBridge` and apply EVERY event in
    ///      `file.events`, IN ORDER;
    ///   7. compose onto a fresh `CGCanvas` at `scale` and produce
    ///      `(CGImage, Data)`.
    ///
    /// Runs synchronously on `renderQueue` (see the type's own doc comment
    /// for why a private queue here is safe). Tears the strip + metrics
    /// canvas down completely (via `defer`) before returning, on both the
    /// success and throw paths — so a failed render never leaves the
    /// process-global metrics-canvas registration pointing at a
    /// since-deallocated box.
    public func render(columns: Int32, unitTwips: Int32, scale: CGFloat) throws -> (image: CGImage, pngData: Data) {
        try renderQueue.sync {
            let metrics = CTMetricsCanvas()
            let metricsBox = CanvasBox(metrics)
            cc_set_metrics_canvas(metricsBox.handle)
            var strip: Strip?
            defer {
                strip?.close()
                cc_set_metrics_canvas(nil)
                // `metricsBox`/`metrics` are released when this closure
                // returns (both are plain locals) — kept alive up to here via
                // `withExtendedLifetime` around the whole body below, matching
                // `setUpStripLocked`'s own "outlive every addLine call" rule
                // (that property's doc comment) rather than `Strip.compose`'s
                // narrower per-call `withExtendedLifetime` (a metrics canvas
                // must stay registered across MANY `addLine`/`addLineCooked`
                // calls, not just one `compose`).
            }

            return try withExtendedLifetime(metricsBox) {
                let newStrip = try Strip()
                strip = newStrip
                try newStrip.setPanelGeometry(unitTwips: unitTwips, panelsPerRow: columns)
                try newStrip.setTitle(file.room)

                let userCharactersDir = ChatSessionModel.userCharactersDir
                let resolver = ProtocolStripBridge.AvatarResolver(
                    comicartDir: artDir, extraDirs: [userCharactersDir])
                let bridge = try ProtocolStripBridge(strip: newStrip, resolver: resolver,
                                                     encoding: WireEncoding(rawValue: file.encodingRaw) ?? .cp1252)
                try bridge.setBackdrop(artDir + "/" + file.backdropName + ".bgb")

                let avbPath = artDir + "/" + file.characterName + ".avb"
                let selfID = try newStrip.addParticipant(nick: file.nick, avbPath: avbPath)
                try newStrip.setSelf(selfID)
                bridge.preRegisterSelfParticipant(nick: file.nick, id: selfID)

                for event in file.events {
                    try bridge.apply(event)
                }

                let (w, h) = newStrip.size
                guard w > 0, h > 0 else {
                    throw RenderError(message: "TranscriptRenderer: composed strip has zero size (no panels?)")
                }
                let canvas = CGCanvas(widthTwips: w, heightTwips: h, scale: scale)
                try bridge.compose(onto: canvas)
                guard let image = canvas.makeCGImage() else {
                    throw RenderError(message: "TranscriptRenderer: makeCGImage() failed")
                }
                guard let png = canvas.pngData() else {
                    throw RenderError(message: "TranscriptRenderer: pngData() failed")
                }
                return (image, png)
            }
        }
    }
}
