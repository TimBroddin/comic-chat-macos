import SwiftUI
import AppKit
import ComicChatKit

/// The comic strip document view: a flipped, layer-backed NSView whose layer
/// contents is the composed strip CGImage (D2 §5 option (a)). Scrolling is
/// NSScrollView's; stick-to-bottom replicates CPageView::m_bAtBottom
/// (pageview.cpp:355-403): auto-scroll on growth ONLY if the user was already
/// at the bottom before the growth.
final class StripDocumentView: NSView {
    override var isFlipped: Bool { true }
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
}

/// Renders the composed strip image inside a scroll view.
///
/// Observation contract: this view does NOT read `AppState` via
/// `@Environment` — there is no documented guarantee that SwiftUI
/// re-invokes `updateNSView` purely because an `@Observable` dependency
/// read inside `updateNSView` changed. Instead, `image`/`sizePoints`/`model`
/// are plain stored properties. The parent must construct this view from
/// observed reads in its own `body`, e.g.:
///
///     ComicStripView(image: appState.stripImage,
///                     sizePoints: appState.stripSizePoints,
///                     model: appState.model)
///
/// Because the parent's `body` is what reads the observed properties,
/// SwiftUI's struct-diffing of `ComicStripView`'s stored properties is what
/// reliably drives `updateNSView` — that diffing (not `@Environment`
/// observation inside the representable) is the documented-reliable path.
struct ComicStripView: NSViewRepresentable {
    let image: CGImage?
    let sizePoints: CGSize
    let model: ChatSessionModel?

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
                [weak model] _ in
                co.lastRequestedWidth = width
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
