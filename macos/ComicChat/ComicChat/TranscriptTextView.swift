import SwiftUI
import AppKit
import ComicChatKit

/// The plain-text transcript view (Plan 4b Task 11, D1 §1.2): a non-editable
/// `NSTextView` in a scroll view, showing the SAME event log
/// `ComicStripView` renders as comic panels — just as scrollback text
/// (`TranscriptTextBuilder`'s output). Toggled in via the View menu
/// (`SettingsStore.comicMode` — "Plain Text view ⌘2") as an alternative to
/// the comic strip, not alongside it (`ChatWindow` swaps one for the other).
///
/// Observation contract: mirrors `ComicStripView`'s own doc comment exactly —
/// this view does NOT read `AppState` via `@Environment` (no documented
/// guarantee `updateNSView` re-runs purely because an `@Observable`
/// dependency read INSIDE it changed). `attributedText` is a plain stored
/// property; the parent's `body` must read `appState.transcriptText` itself
/// and pass it down, so SwiftUI's struct-diffing of this view's stored
/// properties is what reliably drives `updateNSView`:
///
///     TranscriptTextView(attributedText: appState.transcriptText)
struct TranscriptTextView: NSViewRepresentable {
    let attributedText: AttributedString

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject {
        weak var scrollView: NSScrollView?
        weak var textView: NSTextView?
        /// Same stick-to-bottom bookkeeping as `ComicStripView.Coordinator`
        /// (pageview.cpp:355-403's `m_bAtBottom`): auto-scroll on growth ONLY
        /// if the user was already at the bottom before the growth.
        var wasAtBottom = true

        @objc func boundsDidChange(_ note: Notification) {
            guard let sv = scrollView, let tv = textView else { return }
            let visible = sv.contentView.bounds
            let docH = tv.frame.height
            wasAtBottom = docH <= visible.height ||
                          visible.maxY >= docH - 2.0
        }

        func scrollToBottom() {
            guard let sv = scrollView, let tv = textView else { return }
            let y = max(0, tv.frame.height - sv.contentView.bounds.height)
            tv.scroll(NSPoint(x: 0, y: y))
        }
    }

    func makeNSView(context: Context) -> NSScrollView {
        let textView = NSTextView()
        textView.isEditable = false
        textView.isSelectable = true
        textView.drawsBackground = false
        textView.isVerticallyResizable = true
        textView.isHorizontallyResizable = false
        textView.textContainer?.widthTracksTextView = true
        textView.textContainerInset = NSSize(width: 8, height: 8)

        let sv = NSScrollView()
        sv.hasVerticalScroller = true
        sv.documentView = textView
        sv.contentView.postsBoundsChangedNotifications = true

        context.coordinator.scrollView = sv
        context.coordinator.textView = textView
        NotificationCenter.default.addObserver(context.coordinator,
            selector: #selector(Coordinator.boundsDidChange(_:)),
            name: NSView.boundsDidChangeNotification, object: sv.contentView)
        return sv
    }

    func updateNSView(_ sv: NSScrollView, context: Context) {
        let co = context.coordinator
        guard let tv = co.textView else { return }
        let stick = co.wasAtBottom   // captured BEFORE the text swap grows the view
        tv.textStorage?.setAttributedString(NSAttributedString(attributedText))
        if stick { co.scrollToBottom() }
    }

    static func dismantleNSView(_ nsView: NSScrollView, coordinator: Coordinator) {
        NotificationCenter.default.removeObserver(coordinator,
            name: NSView.boundsDidChangeNotification, object: nsView.contentView)
    }
}
