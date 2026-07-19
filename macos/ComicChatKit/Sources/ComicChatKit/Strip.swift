import Foundation
import CoreGraphics
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

    /// Switch an EXISTING participant (a participant id from `addParticipant`)
    /// to a different avatar, loaded fresh from `avbPath`. Mirrors the
    /// original's `ChangeAvatarEntry`/`SetUserAvatarID` behavior: panels already
    /// laid out keep rendering the OLD avatar (no retro-recompose); only
    /// subsequent `addLine`/`addLineCooked` calls for this participant render
    /// with the new one.
    public func setParticipantAvatar(_ participant: Int32, avbPath: String) throws {
        let h = try requireHandle()
        guard cc_strip_set_participant_avatar(h, participant, avbPath) == 0 else {
            throw StripError(message: "setParticipantAvatar(\(participant), \(avbPath)) failed")
        }
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

    /// Set (or update) the comic's title. Builds the title/starring credits
    /// panel fresh (becoming PANEL 0) if no panels exist yet, or rebuilds the
    /// existing title panel's starring rows in place if panel 0 is already a
    /// title panel (`cc_strip_set_title`'s AddTitle/UpdateTitle decision,
    /// Plan 4a Task 7). Call before `addParticipant`/`addLine` so panel 0 is
    /// the title panel, matching `addParticipant`'s member-join refresh
    /// (which itself only fires once a title has been set).
    public func setTitle(_ title: String) throws {
        let h = try requireHandle()
        // CP-1252 bytes, matching addLine's encoding convention for engine text.
        let bytes = title.data(using: .windowsCP1252) ?? Data(title.utf8)
        let rc: Int32 = bytes.withUnsafeBytes { rawBuf -> Int32 in
            var withNul = [CChar](repeating: 0, count: rawBuf.count + 1)
            for i in 0..<rawBuf.count { withNul[i] = CChar(bitPattern: rawBuf[i]) }
            return cc_strip_set_title(h, &withNul)
        }
        guard rc == 0 else {
            throw StripError(message: "setTitle(\"\(title)\") failed")
        }
    }

    /// Record `participant` (an id from `addParticipant`) as the strip's own
    /// avatar -- starring order puts self first. `AddStars` renders nothing
    /// until this is called (mirrors the original's "not registered yet"
    /// guard); if a title has already been set, this also refreshes the
    /// title panel immediately so self's starring row appears.
    public func setSelf(_ participant: Int32) throws {
        let h = try requireHandle()
        guard cc_strip_set_self(h, participant) == 0 else {
            throw StripError(message: "setSelf(\(participant)) failed")
        }
    }

    // MARK: - Plan 4b Task 2: emotion-wheel engine surface

    /// The wheel drag: sets the SELF participant's emotion (`angle` in
    /// radians, `intensity` clamped to `[0, 1]` by the engine) and runs the
    /// original `CBodyCam::UpdateEmotion` chain (`GetBodyFromEmotion` +
    /// `UpdateBody`) on its avatar. `setSelf` must have been called first.
    /// The 0.2 center detente is the CALLER's job (`GetEmotionFromPoint`'s UI
    /// behavior) -- this call always applies the exact angle/intensity given.
    public func setSelfEmotion(angle: Double, intensity: Double) throws {
        let h = try requireHandle()
        guard cc_strip_set_self_emotion(h, angle, intensity) == 0 else {
            throw StripError(message: "setSelfEmotion(angle: \(angle), intensity: \(intensity)) failed")
        }
    }

    /// The typing preview: runs the engine's text->emotion inference
    /// (`ChatPreSendText`) against the SELF participant's avatar so its pose
    /// reflects what `text` WOULD infer, without adding a line to the strip.
    /// `setSelf` must have been called first.
    public func previewSelfText(_ text: String) throws {
        let h = try requireHandle()
        // Same CP-1252-first encoding convention as addLine/setTitle.
        let bytes = text.data(using: .windowsCP1252) ?? Data(text.utf8)
        let rc: Int32 = bytes.withUnsafeBytes { rawBuf -> Int32 in
            var withNul = [CChar](repeating: 0, count: rawBuf.count + 1)
            for i in 0..<rawBuf.count { withNul[i] = CChar(bitPattern: rawBuf[i]) }
            return cc_strip_preview_self_text(h, &withNul)
        }
        guard rc == 0 else {
            throw StripError(message: "previewSelfText(\"\(text)\") failed")
        }
    }

    /// The SELF participant's current pose index (1-based poseID into the
    /// avatar's own pose array -- see `comicchat.h`'s `cc_strip_self_pose` doc
    /// comment for the exact relationship to `cc_avatar_pose_image`'s index
    /// space, which is a DIFFERENT, compacted/icon-skipping numbering).
    /// `setSelf` must have been called first.
    public func selfPoseIndex() throws -> Int32 {
        let h = try requireHandle()
        var idx: Int32 = -1
        guard cc_strip_self_pose(h, &idx) == 0 else {
            throw StripError(message: "selfPoseIndex() failed")
        }
        return idx
    }

    /// The outbound annotation block for the SELF participant's CURRENT pose/
    /// emotion state (pose indices + wire emotion/intensity for both the
    /// gesture/torso and face groups, `cooked == true`). `mode`/`addressees`
    /// are left at their zero/empty defaults -- the caller's job to fill
    /// before sending. `setSelf` must have been called first.
    public func selfAnnotations(encoding: WireEncoding = .cp1252) throws -> Annotations {
        let h = try requireHandle()
        var cAnn = cc_annotations()
        guard cc_strip_self_annotations(h, &cAnn) == 0 else {
            throw StripError(message: "selfAnnotations() failed")
        }
        return Annotations(cAnnotations: cAnn, encoding: encoding)
    }

    /// Render the SELF participant's CURRENT posed body (head + torso + masks --
    /// whatever the last `setSelfEmotion`/`previewSelfText` set) scaled-to-fit
    /// and centered into a `widthTwips` x `heightTwips` image, as a `CGImage`.
    /// This is the bodycam pane's own draw path (the original `CBodyCam`'s
    /// `body->DrawBody`), and it REPLACES the old `selfPoseIndex()` ->
    /// `AvatarFile.poseImage(_:)` single-record preview, which for a COMPLEX
    /// (two-part) avatar drew only the torso record -- a HEADLESS body -- because
    /// such a body is a `CBodyDouble` composited from a separate head and torso
    /// pose. Driving the engine's own `DrawBody` emits BOTH planes for a complex
    /// avatar and the single plane for a simple one.
    ///
    /// Pure image drawing: builds a fresh `CGCanvas` sized to the twips bounds
    /// (no shared metrics-canvas dependency -- `DrawBody` only blits pose planes,
    /// it measures no text), composites onto it via `cc_strip_self_preview`, and
    /// returns `makeCGImage()`. Returns `nil` when no self is set / the
    /// participant no longer resolves / the avatar has no body (the C entry
    /// rejects), so the caller simply shows no preview -- exactly as the old
    /// guard did. `setSelf` must have been called first.
    ///
    /// `scale` is the CGCanvas supersampling factor (points -> pixels); the twips
    /// bounds define the aspect box the body is fit into (the engine's own
    /// `GetBodyBox` does the aspect-fit + horizontal-center + bottom-pin).
    public func selfPreviewImage(widthTwips: Int32, heightTwips: Int32,
                                 scale: CGFloat = 2.0) -> CGImage? {
        guard let h = handle, widthTwips > 0, heightTwips > 0 else { return nil }
        let canvas = CGCanvas(widthTwips: widthTwips, heightTwips: heightTwips, scale: scale)
        let box = CanvasBox(canvas)
        let rc = withExtendedLifetime(box) { () -> Int32 in
            cc_strip_self_preview(h, box.handle, widthTwips, heightTwips)
        }
        guard rc == 0 else { return nil }
        return canvas.makeCGImage()
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

    /// Panel geometry: unit width/height (twips), panels per row, and the
    /// horizontal/vertical interstice constants (not settable). Returns the
    /// create-time default (`(2300, 2300, 2, 144, 144)`) until overridden by
    /// `setPanelGeometry`.
    public var panelGeometry: (unitW: Int32, unitH: Int32, perRow: Int32, hInter: Int32, vInter: Int32) {
        guard let h = handle else { return (0, 0, 0, 0, 0) }
        var w: Int32 = 0, ht: Int32 = 0, perRow: Int32 = 0, hInt: Int32 = 0, vInt: Int32 = 0
        cc_strip_get_panel_geometry(h, &w, &ht, &perRow, &hInt, &vInt)
        return (w, ht, perRow, hInt, vInt)
    }

    /// Set the unit panel size (`unitTwips` for both width and height -- panels
    /// are square, mirroring the original's `SetPanelsWide`) and panels-per-row.
    ///
    /// FRESH STRIP ONLY: mirrors the original `CPageView::SetPanelsWide`, which
    /// always reflows via `ResetExistingPanels(TRUE)` (destroy all panels +
    /// recreate + replay the history log, pageview.cpp:1110-1125) -- something
    /// the headless engine has no history log to do. Call this BEFORE the first
    /// `addLine`/`addLineCooked`; once any line has been added, the underlying
    /// `cc_strip_set_panel_geometry` rejects the call and this throws.
    public func setPanelGeometry(unitTwips: Int32, panelsPerRow: Int32) throws {
        let h = try requireHandle()
        guard cc_strip_set_panel_geometry(h, unitTwips, unitTwips, panelsPerRow) == 0 else {
            throw StripError(message: "setPanelGeometry(unitTwips: \(unitTwips), panelsPerRow: \(panelsPerRow)) failed -- a line has already been added")
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
