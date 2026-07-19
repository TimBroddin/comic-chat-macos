import AppKit

/// The Print… / Export as PDF… drawing surface (Plan 4b Task 10): draws one
/// composed strip `CGImage` scaled to fit the page WIDTH, with vertical
/// pagination coming free from `NSPrintOperation`/`NSPrintInfo` simply
/// because this view's own `frame` is taller than one page — AppKit's
/// printing machinery slices a tall view into successive pages on its own
/// (no manual page-break math needed here, matching the brief's "vertical
/// pagination free via NSPrintInfo").
///
/// Used two ways:
///   - `AppState.printTranscript()`: wrapped in an interactive
///     `NSPrintOperation(view:printInfo:)` — the print panel ALSO offers its
///     own "Save as PDF…" button, which is the brief's "PDF export = the
///     print panel's PDF button" half of the PDF story.
///   - `AppState.exportPDF()`: `dataWithPDF(inside: bounds)` directly, no
///     panel — the brief's "plus a direct `dataWithPDF(inside:)` save for
///     Export as PDF" half.
final class ComicPrintView: NSView {
    private let image: CGImage
    private let scaledSize: CGSize

    /// - Parameters:
    ///   - image: the composed strip to print/export.
    ///   - pageWidthPoints: the page's PRINTABLE width (paper size minus
    ///     margins) — the image is scaled (preserving aspect ratio) so its
    ///     width exactly fills this, and the view's own frame height follows
    ///     from that scale, however tall the resulting image is. A tall
    ///     result is exactly what makes multi-page pagination happen — see
    ///     the type's own doc comment.
    init(image: CGImage, pageWidthPoints: CGFloat) {
        self.image = image
        let imageWidthPoints = CGFloat(image.width)
        let imageHeightPoints = CGFloat(image.height)
        let scale = imageWidthPoints > 0 ? pageWidthPoints / imageWidthPoints : 1.0
        self.scaledSize = CGSize(width: imageWidthPoints * scale, height: imageHeightPoints * scale)
        super.init(frame: NSRect(origin: .zero, size: scaledSize))
    }

    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        // `isFlipped == true` puts (0,0) at the top-left with y increasing
        // downward, matching how a print page's first page should show the
        // TOP of a tall comic strip first. CGContext.draw always expects the
        // rect it's given in the context's OWN (possibly-flipped) coordinate
        // space, so this single full-bounds draw is correct either way.
        ctx.draw(image, in: NSRect(origin: .zero, size: scaledSize))
    }

    override func knowsPageRange(_ range: NSRangePointer) -> Bool {
        range.pointee = NSRange(location: 1, length: 1)
        return true
    }

    override func rectForPage(_ page: Int) -> NSRect {
        bounds
    }
}
