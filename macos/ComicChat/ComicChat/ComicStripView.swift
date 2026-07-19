import SwiftUI
import AppKit
import ComicChatKit

/// The comic strip document view: a flipped, layer-backed NSView whose layer
/// contents is the composed strip CGImage (D2 §5 option (a)). Scrolling is
/// NSScrollView's; stick-to-bottom replicates CPageView::m_bAtBottom
/// (pageview.cpp:355-403): auto-scroll on growth ONLY if the user was already
/// at the bottom before the growth.
///
/// Comic hit-testing (Plan 4b): a click sets the talk-to target (the original's
/// FindAvatarUnderPoint -> member selection), and a hover-settle shows the
/// balloon text as a tooltip (a richer take on the original's screen-name
/// OnToolHitTest). Both convert the AppKit view point to PAGE TWIPS and hand it
/// to the model's engine-queue hit-test hop (see `pageTwips(from:)`).
///
/// Panel export/copy (Plan 4b Batch D): right-click shows a context menu —
/// "Copy Panel as Image" / "Save Panel as PNG…" (disabled when the click
/// misses every panel) and "Copy Strip as Image" (always enabled once
/// something has composed). WHICH panel was clicked is computed LOCALLY
/// (`PanelFit.panelIndex`, the same grid arithmetic the engine's own
/// `cc_strip_hit_test_avatar` walk uses) from `model.panelGeometry` — a
/// synchronous `engineQueue.sync` read-through, unlike `hitTestNick`'s
/// async completion-callback hop, because `menu(for:)` must return the built
/// `NSMenu` immediately (AppKit has no async context-menu API).
final class StripDocumentView: NSView {
    override var isFlipped: Bool { true }

    /// The model for engine-queue hit-testing, and the click callback that
    /// toggles the talk-to target. Set by the coordinator on every update so a
    /// reconnect/rebuild swaps in the live model. Both weak-captured at use.
    weak var model: ChatSessionModel?
    var onAvatarClick: ((String) -> Void)?

    /// Panel export/copy callbacks (Plan 4b Batch D), set by the coordinator
    /// alongside `model`/`onAvatarClick`. Each takes the 0-based panel index
    /// `menu(for:)` resolved from the right-click point.
    var onCopyPanel: ((Int32) -> Void)?
    var onSavePanel: ((Int32) -> Void)?
    var onCopyStrip: (() -> Void)?
    var canCopyStrip: Bool = false

    /// The last balloon-tooltip query point (view coords) — coalesces the
    /// hover query so a settled cursor only fires one engine hop per position.
    private var lastTooltipPoint: NSPoint = NSPoint(x: -1, y: -1)
    private var toolTipTrackingArea: NSTrackingArea?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.contentsGravity = .topLeft
    }
    required init?(coder: NSCoder) { fatalError() }

    func present(image: CGImage, sizePoints: CGSize, scale: CGFloat) {
        layer?.contents = image
        layer?.contentsScale = scale
        setFrameSize(sizePoints)
    }

    /// Convert an AppKit view point (points, this flipped view's own coords) to
    /// engine PAGE TWIPS (y-up, x in [0, width], y in [-height, 0]).
    ///
    /// The composed page top (twips y = 0) is pinned to the view/image top; the
    /// view is `isFlipped` so its y grows DOWNWARD from the top, while page
    /// twips grow UPWARD (0 at top, -height at bottom). CGCanvas maps a twips
    /// coord to device y `((hPt + y/20) * scale)` (CGCanvas.swift:79-80) — i.e.
    /// twips y=0 -> image top, y=-height -> image bottom, exactly the view's
    /// top-to-bottom axis. So the conversion is a pure scale + sign flip:
    ///   xTwips =  round(viewX * 20)
    ///   yTwips = -round(viewY * 20)
    /// (20 twips/point). A pinned example: a click at the flipped view's
    /// (100 pt, 50 pt) is page twips (2000, -1000).
    private func pageTwips(from viewPoint: NSPoint) -> (x: Int32, y: Int32) {
        let x = Int32((viewPoint.x * 20.0).rounded())
        let y = Int32((-viewPoint.y * 20.0).rounded())
        return (x, y)
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        // Only respond to clicks inside the drawn page bounds; a click in the
        // scroll view's empty area beyond the document is not a hit-test.
        guard bounds.contains(p), let model else {
            super.mouseDown(with: event)
            return
        }
        let (xTwips, yTwips) = pageTwips(from: p)
        model.hitTestNick(atTwips: xTwips, yTwips) { [weak self] nick in
            // Main thread (hitTestNick's completion contract). Toggle the
            // clicked avatar's nick in the talk-to selection via the callback.
            if let nick { self?.onAvatarClick?(nick) }
        }
        // Do NOT forward to super — a hit-test click shouldn't also begin a
        // text/scroll drag; a miss simply does nothing (no selection change).
    }

    // MARK: - balloon tooltip (hover-settle)

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let existing = toolTipTrackingArea { removeTrackingArea(existing) }
        let ta = NSTrackingArea(rect: bounds,
                                options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                owner: self, userInfo: nil)
        addTrackingArea(ta)
        toolTipTrackingArea = ta
    }

    override func mouseMoved(with event: NSEvent) {
        super.mouseMoved(with: event)
        let p = convert(event.locationInWindow, from: nil)
        // Coalesce: only re-query when the cursor has actually moved to a new
        // integer-point cell (a settled cursor fires exactly one engine hop).
        if abs(p.x - lastTooltipPoint.x) < 1 && abs(p.y - lastTooltipPoint.y) < 1 { return }
        lastTooltipPoint = p
        guard bounds.contains(p), let model else {
            toolTip = nil
            return
        }
        let (xTwips, yTwips) = pageTwips(from: p)
        model.hitTestBalloonText(atTwips: xTwips, yTwips) { [weak self] text in
            // Main thread. Set the view's tooltip to the balloon text under the
            // cursor (nil clears it) — a simple whole-view NSView.toolTip, the
            // "query once on hover-settle" approach the plan sketches.
            self?.toolTip = text
        }
    }

    override func mouseExited(with event: NSEvent) {
        super.mouseExited(with: event)
        toolTip = nil
        lastTooltipPoint = NSPoint(x: -1, y: -1)
    }

    // MARK: - panel export/copy context menu (Plan 4b Batch D)

    /// The 0-based panel index under `viewPoint` (this view's own coords), or
    /// `nil` if the point misses every panel / no strip has composed yet.
    /// Reads `model.panelGeometry`/`panelCount` via their existing
    /// `engineQueue.sync` read-through accessors (the same synchronous
    /// posture `ChatSessionModel` already documents for those two properties)
    /// and hands the page-twips point to `PanelFit.panelIndex` — pure Swift
    /// math, no engine call.
    private func panelIndex(at viewPoint: NSPoint) -> Int32? {
        guard let model, let geo = model.panelGeometry else { return nil }
        let (xTwips, yTwips) = pageTwips(from: viewPoint)
        return PanelFit.panelIndex(atTwips: xTwips, yTwips,
                                   unitW: geo.unitW, unitH: geo.unitH, perRow: geo.perRow,
                                   hInterstice: geo.hInter, vInterstice: geo.vInter,
                                   panelCount: model.panelCount)
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        let p = convert(event.locationInWindow, from: nil)
        let clickedPanel = bounds.contains(p) ? panelIndex(at: p) : nil

        let menu = NSMenu()

        let copyPanelItem = NSMenuItem(title: "Copy Panel as Image",
                                       action: #selector(copyPanelAction(_:)), keyEquivalent: "")
        copyPanelItem.target = self
        copyPanelItem.isEnabled = clickedPanel != nil
        copyPanelItem.representedObject = clickedPanel
        menu.addItem(copyPanelItem)

        let savePanelItem = NSMenuItem(title: "Save Panel as PNG…",
                                       action: #selector(savePanelAction(_:)), keyEquivalent: "")
        savePanelItem.target = self
        savePanelItem.isEnabled = clickedPanel != nil
        savePanelItem.representedObject = clickedPanel
        menu.addItem(savePanelItem)

        menu.addItem(.separator())

        let copyStripItem = NSMenuItem(title: "Copy Strip as Image",
                                       action: #selector(copyStripAction(_:)), keyEquivalent: "")
        copyStripItem.target = self
        copyStripItem.isEnabled = onCopyStrip != nil && canCopyStrip
        menu.addItem(copyStripItem)

        return menu
    }

    @objc private func copyPanelAction(_ sender: NSMenuItem) {
        guard let index = sender.representedObject as? Int32 else { return }
        onCopyPanel?(index)
    }

    @objc private func savePanelAction(_ sender: NSMenuItem) {
        guard let index = sender.representedObject as? Int32 else { return }
        onSavePanel?(index)
    }

    @objc private func copyStripAction(_ sender: NSMenuItem) {
        onCopyStrip?()
    }
}

/// Renders the composed strip image inside a scroll view.
///
/// Observation contract: this view does NOT read `AppState` via
/// `@Environment` — there is no documented guarantee that SwiftUI
/// re-invokes `updateNSView` purely because an `@Observable` dependency
/// read inside `updateNSView` changed. Instead, `image`/`sizePoints`/`model`/
/// `onAvatarClick` are plain stored properties. The parent must construct this
/// view from observed reads in its own `body`, e.g.:
///
///     ComicStripView(image: appState.stripImage,
///                     sizePoints: appState.stripSizePoints,
///                     model: appState.model,
///                     onAvatarClick: { nick in appState.toggleTalkTo(nick) })
///
/// Because the parent's `body` is what reads the observed properties,
/// SwiftUI's struct-diffing of `ComicStripView`'s stored properties is what
/// reliably drives `updateNSView` — that diffing (not `@Environment`
/// observation inside the representable) is the documented-reliable path.
struct ComicStripView: NSViewRepresentable {
    let image: CGImage?
    let sizePoints: CGSize
    let model: ChatSessionModel?
    /// Comic hit-testing (Plan 4b): called on the MAIN thread with the nick of
    /// the avatar the user clicked in the strip. The parent toggles it in
    /// `AppState.selectedMembers` (the existing talk-to selection state — the
    /// member grid highlight then updates for free, since both read that set).
    var onAvatarClick: ((String) -> Void)? = nil
    /// Panel export/copy (Plan 4b Batch D): right-click menu actions. `nil`
    /// (the default) disables the whole affordance — `TranscriptViewerWindow`'s
    /// reuse of this view (`model: nil`, a static transcript render) leaves
    /// these unset, so its context menu shows no items with a live target.
    var onCopyPanel: ((Int32) -> Void)? = nil
    var onSavePanel: ((Int32) -> Void)? = nil
    var onCopyStrip: (() -> Void)? = nil
    /// Whether "Copy Strip as Image" should show enabled — the caller passes
    /// `appState.stripImage != nil` (mirrors `AppCommands`'s existing
    /// `.disabled(appState.stripImage == nil)` gate on Export as PNG/PDF).
    var canCopyStrip: Bool = false

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject {
        let docView = StripDocumentView(frame: .zero)
        weak var scrollView: NSScrollView?
        var wasAtBottom = true
        var resizeDebounce: Timer?
        /// Width (points) the pending/last-fired reflow was scheduled for.
        /// Gates the debounce so pure image swaps at a constant width don't
        /// keep re-arming a perpetual 250 ms recompose timer.
        var lastRequestedWidth: CGFloat = -1

        @objc func boundsDidChange(_ note: Notification) {
            guard let sv = scrollView else { return }
            // Recompute "at bottom" on every user scroll, BEFORE any growth
            // (the original re-caches m_bAtBottom in OnVScroll).
            let visible = sv.contentView.bounds
            let docH = docView.frame.height
            wasAtBottom = docH <= visible.height ||
                          visible.maxY >= docH - 2.0   // tolerance, matches AtBottom's intent
        }
        func scrollToBottom() {
            guard let sv = scrollView else { return }
            let y = max(0, docView.frame.height - sv.contentView.bounds.height)
            docView.scroll(NSPoint(x: 0, y: y))
        }
    }

    func makeNSView(context: Context) -> NSScrollView {
        let sv = NSScrollView()
        sv.hasVerticalScroller = true
        sv.documentView = context.coordinator.docView
        sv.contentView.postsBoundsChangedNotifications = true
        context.coordinator.scrollView = sv
        NotificationCenter.default.addObserver(context.coordinator,
            selector: #selector(Coordinator.boundsDidChange(_:)),
            name: NSView.boundsDidChangeNotification, object: sv.contentView)
        return sv
    }

    func updateNSView(_ sv: NSScrollView, context: Context) {
        let co = context.coordinator
        let scale = sv.window?.backingScaleFactor ?? 2.0
        // Keep the doc view's hit-test wiring in sync with the live model +
        // click callback (a reconnect swaps in a fresh model).
        co.docView.model = model
        co.docView.onAvatarClick = onAvatarClick
        co.docView.onCopyPanel = onCopyPanel
        co.docView.onSavePanel = onSavePanel
        co.docView.onCopyStrip = onCopyStrip
        co.docView.canCopyStrip = canCopyStrip
        if let img = image {
            let stick = co.wasAtBottom                    // captured BEFORE growth
            co.docView.present(image: img, sizePoints: sizePoints, scale: scale)
            if stick { co.scrollToBottom() }
        }
        // Debounced reflow on width change (the original's posted
        // WM_AUTOFITPANELS after resize — pageview.cpp:1379-1420). Live
        // resize does NOTHING per frame; content letterboxes. Width-gated:
        // pure image swaps at an unchanged width must not re-arm the timer,
        // or every incoming message would drive a perpetual 250 ms
        // recompose loop via ChatSessionModel.setViewport.
        let width = sv.contentSize.width
        if abs(width - co.lastRequestedWidth) > 0.5 {
            co.resizeDebounce?.invalidate()
            co.resizeDebounce = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: false) {
                [weak co, weak model] _ in
                co?.lastRequestedWidth = width
                model?.setViewport(widthPoints: width, scale: scale)
            }
        }
    }

    static func dismantleNSView(_ nsView: NSScrollView, coordinator: Coordinator) {
        NotificationCenter.default.removeObserver(coordinator,
            name: NSView.boundsDidChangeNotification, object: nsView.contentView)
        coordinator.resizeDebounce?.invalidate()
    }
}
