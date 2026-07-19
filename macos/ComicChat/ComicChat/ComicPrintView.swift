import AppKit

/// The Print… / Export as PDF… drawing surface (Plan 4b Task 10): draws one
/// composed strip `CGImage` scaled to fit the page WIDTH, with vertical
/// pagination handled by AppKit's automatic slicing on tall NSView instances
/// during `NSPrintOperation` printing — NSView's default behavior slices
/// successive vertical pages automatically (no manual page-break math needed,
/// matching the brief's "vertical pagination free via NSPrintInfo").
///
/// Two PDF output paths with different behaviors:
///   - `NSPrintOperation` printing: AppKit auto-paginates the tall view into
///     successive vertical pages. The print panel's "Save as PDF…" button uses
///     this flow, which is the brief's PDF export half.
///   - `dataWithPDF(inside: bounds)` direct export: intentionally produces one
///     tall single-page PDF (the full strip in one output), a legitimate strip
///     export mode for Export-as-PDF command-line saves.
///
/// Used two ways:
///   - `AppState.printTranscript()`: wrapped in an interactive
///     `NSPrintOperation(view:printInfo:)` — the print panel ALSO offers its
///     own "Save as PDF…" button.
///   - `AppState.exportPDF()`: `dataWithPDF(inside: bounds)` directly, no
///     panel.
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
}
