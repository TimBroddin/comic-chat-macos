import Foundation
import cchat_engine

// Canvas boundary (Plan 2, spec §4.3), Swift side. The C engine draws through
// a `cc_canvas` vtable (cc_canvas_ops in comicchat.h): a set of C function
// pointers plus an opaque `ctx`. This file defines the Swift-native `Canvas`
// protocol that mirrors those ops with Swift types, and `CanvasBox`, which
// bridges any `Canvas` into a stable `cc_canvas` the engine can call.
//
// Coordinate/color conventions are the engine's, unchanged: coordinates are
// MM_TWIPS logical units (1/1440 inch, y-UP — larger y is higher on the page);
// colors are GDI COLORREF (0x00BBGGRR — red in the low byte). Text `bytes` are
// raw CP-1252 bytes, not a decoded String, exactly as the engine supplies them.

// MARK: - Value types mirroring the C structs

/// A font request (mirrors `cc_font_spec`). `face` is the family name (e.g.
/// "Comic Sans MS"); `height` is a LOGFONT lfHeight in twips (negative means
/// character height, the usual GDI convention).
public struct FontSpec: Hashable, Sendable {
    public var face: String
    public var height: Int32
    public var weight: Int32
    public var italic: Bool
    public var underline: Bool
    public var strikeout: Bool
    public var charset: UInt8

    public init(face: String, height: Int32, weight: Int32 = 400,
                italic: Bool = false, underline: Bool = false,
                strikeout: Bool = false, charset: UInt8 = 0) {
        self.face = face
        self.height = height
        self.weight = weight
        self.italic = italic
        self.underline = underline
        self.strikeout = strikeout
        self.charset = charset
    }

    /// Build from the C `cc_font_spec` the engine hands the vtable.
    init(_ f: cc_font_spec) {
        var f = f
        self.face = withUnsafeBytes(of: &f.face) { raw in
            String(cString: raw.bindMemory(to: CChar.self).baseAddress!)
        }
        self.height = f.height
        self.weight = f.weight
        self.italic = f.italic != 0
        self.underline = f.underline != 0
        self.strikeout = f.strikeout != 0
        self.charset = f.charset
    }
}

/// The TEXTMETRIC fields the engine reads, in twips (mirrors `cc_text_metrics`).
public struct TextMetrics: Sendable {
    public var height: Int32
    public var ascent: Int32
    public var descent: Int32
    public var internalLeading: Int32
    public var externalLeading: Int32
    public var aveCharWidth: Int32
    public var maxCharWidth: Int32

    public init(height: Int32, ascent: Int32, descent: Int32,
                internalLeading: Int32, externalLeading: Int32,
                aveCharWidth: Int32, maxCharWidth: Int32) {
        self.height = height
        self.ascent = ascent
        self.descent = descent
        self.internalLeading = internalLeading
        self.externalLeading = externalLeading
        self.aveCharWidth = aveCharWidth
        self.maxCharWidth = maxCharWidth
    }
}

/// One path entry (mirrors `cc_path_pt`). A cubic segment is THREE consecutive
/// `.cubic` entries (control1, control2, endpoint) — see `PathVerb`.
public enum PathVerb: Int32, Sendable {
    case move = 0   // CC_PATH_MOVE
    case line = 1   // CC_PATH_LINE
    case cubic = 2  // CC_PATH_CUBIC (three consecutive entries = one segment)
    case close = 3  // CC_PATH_CLOSE
}

public struct PathPoint: Sendable {
    public var verb: PathVerb
    public var x: Int32
    public var y: Int32
    public init(verb: PathVerb, x: Int32, y: Int32) {
        self.verb = verb
        self.x = x
        self.y = y
    }
}

// MARK: - Canvas protocol

/// A drawing/measuring surface the engine can target, expressed in Swift. Every
/// method corresponds one-to-one with a `cc_canvas_ops` entry. Implementations
/// receive the engine's raw coordinates/colors/bytes untouched.
///
/// THREADING: like the C engine (see comicchat.h's single-thread contract), a
/// Canvas is driven from ONE thread at a time. Implementations need no internal
/// locking.
public protocol Canvas: AnyObject {
    /// Measure `bytes` (`len` raw CP-1252 bytes) in font `f`. Returns
    /// (width, height) in twips. Must work with no drawing surface active.
    func measureText(_ f: FontSpec, bytes: UnsafePointer<CChar>?, len: Int32) -> (w: Int32, h: Int32)

    /// Font metrics for `f`, in twips.
    func fontMetrics(_ f: FontSpec) -> TextMetrics

    /// Draw `len` raw bytes at (x,y) in `color`. `bkOpaque`/`bkColor` are the
    /// text background fill state (opaque vs transparent).
    func drawText(_ f: FontSpec, x: Int32, y: Int32, color: UInt32,
                  bkOpaque: Bool, bkColor: UInt32,
                  bytes: UnsafePointer<CChar>?, len: Int32)

    /// Fill the rectangle (l,t,r,b) with `color`.
    func fillRect(l: Int32, t: Int32, r: Int32, b: Int32, color: UInt32)

    /// Blit `img` (an engine-owned RGBA8 straight-alpha image; borrow only for
    /// the duration of the call) from source rect (sl,st,sr,sb) into dest rect
    /// (dl,dt,dr,db).
    func drawImage(_ img: UnsafePointer<cc_image>?,
                   dl: Int32, dt: Int32, dr: Int32, db: Int32,
                   sl: Int32, st: Int32, sr: Int32, sb: Int32)

    /// Fill and/or stroke a path. `pts` uses the 3-consecutive-`.cubic`
    /// convention. Colors are COLORREF; `strokeWidth` is in twips; `dashed`
    /// selects a dashed pen.
    func path(_ pts: UnsafePointer<cc_path_pt>?, n: Int32,
              doFill: Bool, fillColor: UInt32,
              doStroke: Bool, strokeColor: UInt32,
              strokeWidth: Int32, dashed: Bool)

    /// Push an intersecting clip rectangle (l,t,r,b).
    func clipPush(l: Int32, t: Int32, r: Int32, b: Int32)

    /// Pop the last-pushed clip (restore the previous clip region).
    func clipPop()

    /// Non-zero if the target is a printer (affects some engine paths).
    func isPrinting() -> Bool
}

// MARK: - CanvasBox: Swift Canvas -> cc_canvas bridge

/// Bridges a Swift `Canvas` into a `cc_canvas` the C engine can call.
///
/// The engine talks to canvases purely through the `cc_canvas` value's function
/// pointers plus its opaque `ctx`. `CanvasBox` fills `ctx` with an UNRETAINED
/// pointer to itself and points `ops` at a table of static C thunks; each thunk
/// recovers the box from `ctx` (via `takeUnretainedValue`) and forwards to the
/// wrapped Swift `Canvas`.
///
/// LIFETIME (load-bearing): the engine keeps the raw `cc_canvas*` (via
/// `cc_set_metrics_canvas` and/or the `cc_strip_compose` argument) and may call
/// through it at any point until the strip is composed/destroyed. The box owns
/// the `cc_canvas` and the ops table, and it MUST OUTLIVE all engine use — but it
/// does NOT keep itself alive (that would be a leak-forever cycle, since nothing
/// external could break it). The CALLER is responsible for keeping the box
/// alive: hold it in a local across the whole span of engine calls, e.g. via
/// `withExtendedLifetime` (see `Strip.compose`). Never let a `CanvasBox`
/// deallocate while its `cc_canvas` is still registered with, or being called
/// by, the engine.
public final class CanvasBox {
    /// The wrapped Swift canvas. Public so callers can recover it after use
    /// (e.g. read a RecordingCanvas's log once composing is done).
    public let canvas: Canvas

    // Heap-allocated so `ops`/`canvasValue` have stable addresses for the C side.
    private let opsPtr: UnsafeMutablePointer<cc_canvas_ops>
    private let canvasPtr: UnsafeMutablePointer<cc_canvas>

    public init(_ canvas: Canvas) {
        self.canvas = canvas
        opsPtr = UnsafeMutablePointer<cc_canvas_ops>.allocate(capacity: 1)
        canvasPtr = UnsafeMutablePointer<cc_canvas>.allocate(capacity: 1)

        opsPtr.pointee = cc_canvas_ops(
            measure_text: CanvasBox.thunkMeasureText,
            font_metrics: CanvasBox.thunkFontMetrics,
            draw_text: CanvasBox.thunkDrawText,
            fill_rect: CanvasBox.thunkFillRect,
            draw_image: CanvasBox.thunkDrawImage,
            path: CanvasBox.thunkPath,
            clip_push: CanvasBox.thunkClipPush,
            clip_pop: CanvasBox.thunkClipPop,
            is_printing: CanvasBox.thunkIsPrinting)

        // ctx is an UNRETAINED pointer to self — the box does NOT keep itself
        // alive. Its lifetime is managed by whoever holds the CanvasBox (the
        // caller keeps it in a local across all engine calls; see the LIFETIME
        // note above and `Strip.compose`'s withExtendedLifetime). Retaining self
        // here instead would create a cycle the box could never break, leaking
        // it plus the wrapped canvas. The thunks pair this with
        // `takeUnretainedValue()`.
        canvasPtr.pointee = cc_canvas(
            ops: UnsafePointer(opsPtr),
            ctx: Unmanaged.passUnretained(self).toOpaque())
    }

    deinit {
        canvasPtr.deallocate()
        opsPtr.deallocate()
    }

    /// The stable `cc_canvas*` to hand the engine. Valid for the box's lifetime.
    public var handle: UnsafeMutablePointer<cc_canvas> { canvasPtr }

    /// Recover the box from an engine-supplied `ctx`.
    private static func from(_ ctx: UnsafeMutableRawPointer?) -> CanvasBox {
        Unmanaged<CanvasBox>.fromOpaque(ctx!).takeUnretainedValue()
    }

    // MARK: static C thunks (must be plain C function pointers: no captures)

    private static let thunkMeasureText: @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<cc_font_spec>?,
        UnsafePointer<CChar>?, Int32,
        UnsafeMutablePointer<Int32>?, UnsafeMutablePointer<Int32>?) -> Void = {
        ctx, f, bytes, len, outW, outH in
        let box = CanvasBox.from(ctx)
        let spec = f != nil ? FontSpec(f!.pointee) : FontSpec(face: "", height: 0)
        let (w, h) = box.canvas.measureText(spec, bytes: bytes, len: len)
        if let outW = outW { outW.pointee = w }
        if let outH = outH { outH.pointee = h }
    }

    private static let thunkFontMetrics: @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<cc_font_spec>?,
        UnsafeMutablePointer<cc_text_metrics>?) -> Void = {
        ctx, f, out in
        let box = CanvasBox.from(ctx)
        let spec = f != nil ? FontSpec(f!.pointee) : FontSpec(face: "", height: 0)
        let m = box.canvas.fontMetrics(spec)
        if let out = out {
            out.pointee.height = m.height
            out.pointee.ascent = m.ascent
            out.pointee.descent = m.descent
            out.pointee.internal_leading = m.internalLeading
            out.pointee.external_leading = m.externalLeading
            out.pointee.ave_char_width = m.aveCharWidth
            out.pointee.max_char_width = m.maxCharWidth
        }
    }

    private static let thunkDrawText: @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<cc_font_spec>?,
        Int32, Int32, UInt32, Int32, UInt32,
        UnsafePointer<CChar>?, Int32) -> Void = {
        ctx, f, x, y, color, bkOpaque, bkColor, bytes, len in
        let box = CanvasBox.from(ctx)
        let spec = f != nil ? FontSpec(f!.pointee) : FontSpec(face: "", height: 0)
        box.canvas.drawText(spec, x: x, y: y, color: color,
                            bkOpaque: bkOpaque != 0, bkColor: bkColor,
                            bytes: bytes, len: len)
    }

    private static let thunkFillRect: @convention(c) (
        UnsafeMutableRawPointer?, Int32, Int32, Int32, Int32, UInt32) -> Void = {
        ctx, l, t, r, b, color in
        CanvasBox.from(ctx).canvas.fillRect(l: l, t: t, r: r, b: b, color: color)
    }

    private static let thunkDrawImage: @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<cc_image>?,
        Int32, Int32, Int32, Int32, Int32, Int32, Int32, Int32) -> Void = {
        ctx, img, dl, dt, dr, db, sl, st, sr, sb in
        CanvasBox.from(ctx).canvas.drawImage(img, dl: dl, dt: dt, dr: dr, db: db,
                                             sl: sl, st: st, sr: sr, sb: sb)
    }

    private static let thunkPath: @convention(c) (
        UnsafeMutableRawPointer?, UnsafePointer<cc_path_pt>?, Int32,
        Int32, UInt32, Int32, UInt32, Int32, Int32) -> Void = {
        ctx, pts, n, doFill, fillColor, doStroke, strokeColor, strokeWidth, dashed in
        CanvasBox.from(ctx).canvas.path(pts, n: n,
                                        doFill: doFill != 0, fillColor: fillColor,
                                        doStroke: doStroke != 0, strokeColor: strokeColor,
                                        strokeWidth: strokeWidth, dashed: dashed != 0)
    }

    private static let thunkClipPush: @convention(c) (
        UnsafeMutableRawPointer?, Int32, Int32, Int32, Int32) -> Void = {
        ctx, l, t, r, b in
        CanvasBox.from(ctx).canvas.clipPush(l: l, t: t, r: r, b: b)
    }

    private static let thunkClipPop: @convention(c) (UnsafeMutableRawPointer?) -> Void = {
        ctx in
        CanvasBox.from(ctx).canvas.clipPop()
    }

    private static let thunkIsPrinting: @convention(c) (UnsafeMutableRawPointer?) -> Int32 = {
        ctx in
        CanvasBox.from(ctx).canvas.isPrinting() ? 1 : 0
    }
}
