import Foundation
import cchat_engine

/// A scripted-strip session (Plan 2 Task 10 C API `cc_strip`), wrapped in Swift
/// with RAII lifetime (the `AvatarFile`/`BackdropFile` pattern from Plan 1):
/// `init` creates the underlying `cc_strip*`, `deinit` (or an explicit `close()`)
/// destroys it.
///
/// A `Strip` drives the ENTIRE lifted layout engine: it opens participant
/// avatars, wires the session user/talk-to graph, ingests scripted lines through
/// the panel orchestrator, and composites the finished page onto a `Canvas`.
///
/// THREADING CONTRACT (binding — mirrors comicchat.h): the engine uses
/// process-global mutable state (the avatar registry, the session/settings, the
/// font statics, the backdrop registries, the composing-page back-pointer) with
/// NO internal locking. ALL `Strip` calls — and any other ComicChatKit engine
/// call — must originate from ONE thread at a time; concurrent use is undefined
/// behavior. The test suite serializes these (`StripTests` is a `.serialized`
/// suite) for exactly this reason.
///
/// DETERMINISM: `cc_strip_create` seeds the global `rand()` stream
/// (`srand(0x5EED)`) so identical scripts yield byte-identical strips regardless
/// of prior `rand()` consumers.
public final class Strip {
    public struct StripError: Error, CustomStringConvertible {
        public let message: String
        public var description: String { message }
    }

    /// A composition/text mode for a scripted line (mirrors `CC_MODE_*`, which
    /// mirror the engine's `BM_*`). Combine with `Mode` set semantics via the
    /// `modes:` argument to `addLine`.
    public struct Mode: OptionSet, Sendable {
        public let rawValue: UInt32
        public init(rawValue: UInt32) { self.rawValue = rawValue }
        public static let say     = Mode(rawValue: UInt32(CC_MODE_SAY))
        public static let whisper = Mode(rawValue: UInt32(CC_MODE_WHISPER))
        public static let think   = Mode(rawValue: UInt32(CC_MODE_THINK))
        public static let action  = Mode(rawValue: UInt32(CC_MODE_ACTION))
    }

    private var handle: OpaquePointer?

    /// Create an empty strip session. Initializes the avatar registry, resets
    /// the session user table, installs the comics fonts, sets unit-panel
    /// geometry, and deterministically seeds the RNG.
    public init() throws {
        guard let h = cc_strip_create() else {
            throw StripError(message: "cc_strip_create failed")
        }
        handle = h
    }

    /// Destroy the underlying session. Safe to call more than once; `deinit`
    /// calls it too. After `close()` the `Strip` must not be used.
    public func close() {
        if let h = handle {
            cc_strip_destroy(h)
            handle = nil
        }
    }

    deinit {
        close()
    }

    private func requireHandle() throws -> OpaquePointer {
        guard let h = handle else {
            throw StripError(message: "Strip used after close()")
        }
        return h
    }

    /// Open the avatar at `avbPath`, register it, create its session user entry,
    /// and wire it up. `nick` names the participant. Returns the assigned
    /// participant id (>= 1).
    @discardableResult
    public func addParticipant(nick: String, avbPath: String) throws -> Int32 {
        let h = try requireHandle()
        let id = cc_strip_add_participant(h, nick, avbPath)
        guard id >= 0 else {
            throw StripError(message: "addParticipant(\(nick), \(avbPath)) failed")
        }
        return id
    }

    /// Load the `.bgb` at `bgbPath` and register it so every subsequently
    /// created panel inherits it. Call before `addLine` for the backdrop to
    /// appear in composed panels.
    public func setBackdrop(_ bgbPath: String) throws {
        let h = try requireHandle()
        guard cc_strip_set_backdrop(h, bgbPath) == 0 else {
            throw StripError(message: "setBackdrop(\(bgbPath)) failed")
        }
    }

    /// Ingest one scripted line spoken by `speaker` (a participant id from
    /// `addParticipant`). `addressees` name who the speaker is talking to
    /// (participant ids) — they drive the camera's facing/order.
    public func addLine(speaker: Int32, text: String, modes: Mode,
                        addressees: [Int32]) throws {
        let h = try requireHandle()
        // Pass the text as raw CP-1252 bytes when possible (the engine treats
        // text as CP-1252); fall back to UTF-8 for anything Windows-1252 can't
        // encode. The frozen script is ASCII, where both agree.
        let bytes = text.data(using: .windowsCP1252) ?? Data(text.utf8)
        let rc: Int32 = bytes.withUnsafeBytes { rawBuf -> Int32 in
            let cptr = rawBuf.bindMemory(to: CChar.self).baseAddress
            if addressees.isEmpty {
                return cc_strip_add_line(h, speaker, cptr, modes.rawValue, nil, 0)
            }
            return addressees.withUnsafeBufferPointer { addrBuf in
                cc_strip_add_line(h, speaker, cptr, modes.rawValue,
                                  addrBuf.baseAddress, Int32(addrBuf.count))
            }
        }
        guard rc == 0 else {
            throw StripError(message: "addLine(speaker=\(speaker), \"\(text)\") failed")
        }
    }

    /// Ingest one WIRE-RECEIVED line whose pose should come from a decoded
    /// `Annotations` block rather than text inference (Plan 3 Task 9's
    /// `cc_strip_add_line_cooked`, the ported `SayEntry::Execute` "cooked"
    /// path — histent.cpp:80-108). Pass `nil` (or an `Annotations` with
    /// `cooked == false`) to fall back to exactly `addLine`'s text-inference
    /// behavior — a plain/unannotated line still gets the original heuristic.
    public func addLineCooked(speaker: Int32, text: String, modes: Mode,
                              addressees: [Int32], annotations: Annotations?,
                              encoding: WireEncoding = .cp1252) throws {
        let h = try requireHandle()
        let bytes = text.data(using: .windowsCP1252) ?? Data(text.utf8)
        var cAnn = annotations?.toCAnnotations(encoding: encoding)
        let rc: Int32 = bytes.withUnsafeBytes { rawBuf -> Int32 in
            let cptr = rawBuf.bindMemory(to: CChar.self).baseAddress
            func callWithAddressees(_ annPtr: UnsafePointer<cc_annotations>?) -> Int32 {
                if addressees.isEmpty {
                    return cc_strip_add_line_cooked(h, speaker, cptr, modes.rawValue, nil, 0, annPtr)
                }
                return addressees.withUnsafeBufferPointer { addrBuf in
                    cc_strip_add_line_cooked(h, speaker, cptr, modes.rawValue,
                                             addrBuf.baseAddress, Int32(addrBuf.count), annPtr)
                }
            }
            if cAnn != nil {
                return withUnsafePointer(to: &cAnn!) { callWithAddressees($0) }
            }
            return callWithAddressees(nil)
        }
        guard rc == 0 else {
            throw StripError(message: "addLineCooked(speaker=\(speaker), \"\(text)\") failed")
        }
    }

    /// Number of panels laid out so far.
    public var panelCount: Int32 {
        guard let h = handle else { return 0 }
        return cc_strip_panel_count(h)
    }

    /// Bounding box of the finished page in twips (width, height >= 0).
    public var size: (width: Int32, height: Int32) {
        guard let h = handle else { return (0, 0) }
        var w: Int32 = 0
        var h2: Int32 = 0
        cc_strip_get_size(h, &w, &h2)
        return (w, h2)
    }

    /// Composite the finished page onto `canvas`. The `canvas` is bridged into
    /// a `cc_canvas` via a `CanvasBox` that is held for the whole engine call,
    /// satisfying the box's outlive-engine-use lifetime requirement.
    public func compose(onto canvas: Canvas) throws {
        let h = try requireHandle()
        // The box owns the cc_canvas the engine calls through; keeping it in a
        // local across the (synchronous) compose call keeps it alive for the
        // entire span of engine callbacks. withExtendedLifetime makes that
        // guarantee explicit and robust against optimization.
        let box = CanvasBox(canvas)
        let rc = withExtendedLifetime(box) { () -> Int32 in
            cc_strip_compose(h, box.handle)
        }
        guard rc == 0 else {
            throw StripError(message: "compose failed")
        }
    }
}
