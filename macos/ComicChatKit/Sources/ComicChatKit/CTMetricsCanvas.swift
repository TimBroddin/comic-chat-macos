import Foundation
import cchat_engine

/// A `Canvas` that answers real CoreText measurement/metrics queries
/// (delegating to `CoreTextMetrics`) and does NOTHING else — the production
/// default metrics canvas (`cc_set_metrics_canvas`), replacing the fake
/// `RecordingCanvas` metrics (`len*120 x 240`, fixed 240/190/50/40/20/120/240
/// table) that every layout-time install used before Plan 4a Task 4.
///
/// WHY THIS MATTERS: the engine measures glyphs ONCE per strip, at layout
/// time, through whatever canvas `cc_set_metrics_canvas` names — completely
/// separately from the canvas that later DRAWS the composed page (`CGCanvas`,
/// which already uses real CoreText metrics for its own measurement calls).
/// Installing the fake `RecordingCanvas` as the metrics canvas while `CGCanvas`
/// draws with real glyph metrics is exactly the mismatch that produced the
/// "balloon text sits high" artifact: layout reserved fake-sized space, but
/// drawing rendered real-sized glyphs into it. `CTMetricsCanvas` closes that
/// gap — layout and drawing now agree by construction, both backed by the
/// same `CoreTextMetrics` helper `CGCanvas.fontMetrics` also calls into.
///
/// MEASUREMENT-ONLY (binding): this canvas is registered ONLY via
/// `cc_set_metrics_canvas`, which the engine's own contract restricts to
/// `measure_text`/`font_metrics` calls (see comicchat.h's canvas boundary
/// doc: "measurement -- must work with no drawing surface active"). Every
/// draw-side op here is unreachable in correct engine use, so each is an
/// `assertionFailure` (loud in DEBUG, where a genuine engine bug SHOULD be
/// caught immediately) followed by a safe no-op (so a RELEASE build never
/// crashes on it — `assertionFailure` alone does not abort in release,
/// matching the brief's "must never crash release" requirement).
public final class CTMetricsCanvas: Canvas {
    public init() {}

    public func measureText(_ f: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32) -> (w: Int32, h: Int32) {
        CoreTextMetrics.measure(f, bytes: bytes, len: len)
    }

    public func fontMetrics(_ f: FontSpec) -> TextMetrics {
        CoreTextMetrics.metrics(for: f)
    }

    public func drawText(_ f: FontSpec, x: Int32, y: Int32, color: UInt32,
                         bkOpaque: Bool, bkColor: UInt32,
                         bytes: UnsafePointer<CChar>?, len: Int32) {
        assertionFailure("CTMetricsCanvas is measurement-only: drawText must never be called on the metrics canvas")
    }

    public func fillRect(l: Int32, t: Int32, r: Int32, b: Int32, color: UInt32) {
        assertionFailure("CTMetricsCanvas is measurement-only: fillRect must never be called on the metrics canvas")
    }

    public func drawImage(_ img: UnsafePointer<cc_image>?,
                          dl: Int32, dt: Int32, dr: Int32, db: Int32,
                          sl: Int32, st: Int32, sr: Int32, sb: Int32) {
        assertionFailure("CTMetricsCanvas is measurement-only: drawImage must never be called on the metrics canvas")
    }

    public func path(_ pts: UnsafePointer<cc_path_pt>?, n: Int32,
                     doFill: Bool, fillColor: UInt32,
                     doStroke: Bool, strokeColor: UInt32,
                     strokeWidth: Int32, dashed: Bool) {
        assertionFailure("CTMetricsCanvas is measurement-only: path must never be called on the metrics canvas")
    }

    public func clipPush(l: Int32, t: Int32, r: Int32, b: Int32) {
        assertionFailure("CTMetricsCanvas is measurement-only: clipPush must never be called on the metrics canvas")
    }

    public func clipPop() {
        assertionFailure("CTMetricsCanvas is measurement-only: clipPop must never be called on the metrics canvas")
    }

    public func isPrinting() -> Bool { false }
}
