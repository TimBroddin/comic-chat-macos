import Foundation

/// Viewport-fit math for panel geometry: how many columns fit "comfortably"
/// in a given viewport width, and what unit panel size (square, twips) that
/// implies.
///
/// This is a FRESH Swift PORT, not a lift: the original `CPageView` view code
/// this mirrors (`v2.5-beta-1-modern/pageview.cpp`) was never brought into the
/// lifted `engine/` tree (there is no view layer in the headless engine), so
/// there is nothing to characterize against here -- this reimplements the
/// arithmetic faithfully in Swift, citing the exact original lines for every
/// borrowed expression, the same discipline the lifted C++ bridge code uses
/// for its own citations.
///
/// Ported from:
///   - `CPageView::FitPanelsWide`            pageview.cpp:1394-1404
///   - `CPageView::GetProspectivePanelWidth`  pageview.cpp:1143-1158
///   - `CPageView::SetPanelsWide`             pageview.cpp:1110-1125 (square-panels rule)
///
/// CALLER OWNS SCROLLBAR/INSET SUBTRACTION: the original's
/// `GetProspectivePanelWidth` computes `xWidth` from the CLIENT area width
/// MINUS a fixed scrollbar allowance (`SCROLLWIDTH` = 16, pageview.cpp:1108,
/// 1146: `(r.right - SCROLLWIDTH) * m_dpiConvx`) before ANY of the column
/// arithmetic below runs. This port takes a plain viewport-twips width as
/// input and performs none of that subtraction -- the caller (the eventual
/// AppKit view, Task 11) is responsible for converting its content-rect width
/// to twips and subtracting any scrollbar/inset allowance BEFORE calling
/// `columns`/`unitPanelTwips`. This keeps the port pure (no DC/DPI/scrollbar
/// concepts) and testable without an AppKit view.
///
/// HEIGHT TERM DROPPED: the original's `GetProspectivePanelWidth` also reduces
/// `goalPanelWidth` so an integral number of panels fit the CLIENT HEIGHT
/// (pageview.cpp:1149-1156: computes `nHigh` from `yHeight`, then
/// `goalPanelWidth = min(goalPanelWidth, goalPanelHeight)`). This port has no
/// height input at all (`columns`/`unitPanelTwips` take only a viewport WIDTH),
/// so that reduction cannot be reproduced here -- this is a WIDTH-ONLY fit,
/// matching exactly what the Task 5 brief's Step 4 specifies (a formula with
/// no height term). A future task that wants the height-driven reduction too
/// would need to add a height parameter and port that branch separately.
public enum PanelFit {
    /// pageview.cpp:55 `#define COMFORTABLEPANELWIDTH 3000` -- twips (~2.1"),
    /// the minimum panel width FitPanelsWide considers still readable.
    public static let comfortablePanelWidth: Int32 = 3000

    /// panel.h:165 `#define MINUNITPANELWIDTH 2300` -- the absolute floor
    /// SetPanelsWide clamps to (pageview.cpp:1113 `max(nGoalPanelWidth,
    /// MINUNITPANELWIDTH)`), regardless of how the arithmetic above worked out.
    public static let minUnitPanelWidth: Int32 = 2300

    /// The interstice between panels, in twips. Mirrors the
    /// `CUnitPanelPage::m_vInterstice`/`m_hInterstice` statics (panel.cpp:75-76),
    /// which this port hardcodes as a constant rather than reading live engine
    /// state: PanelFit is pure math with no `cc_strip` dependency (the Task 5
    /// brief's PanelFitTests run with no serialized tree / no engine state at
    /// all), and the statics' value (144) has been fixed since the original
    /// shipped -- there is no live setter for it in either the original or the
    /// lifted engine (Task 5's own `cc_strip_get_panel_geometry` getter only
    /// reads it, it does not let a caller change it).
    public static let interstice: Int32 = 144

    /// Largest column count in 1...5 whose resulting unit panel width (from
    /// `unitPanelTwips`, BEFORE the `minUnitPanelWidth` clamp) is still
    /// `>= comfortablePanelWidth`, else 1.
    ///
    /// Ported from `FitPanelsWide` (pageview.cpp:1394-1404):
    /// ```cpp
    /// int best = 1;
    /// for (int n = 1; n <= 5; n++) {
    ///     if (GetProspectivePanelWidth(n) >= COMFORTABLEPANELWIDTH)
    ///         best = n;
    ///     else
    ///         break;      // BREAKS at the first uncomfortable n -- does not
    ///     }                // keep scanning higher n hoping for a rebound.
    /// return best;
    /// ```
    /// Width is monotonically non-increasing in `n` (more columns => smaller
    /// share of the same viewport, modulo interstice rounding), so break-on-first
    /// -miss and scan-to-best-of-5 agree in practice, but this port matches the
    /// original's control flow exactly rather than relying on that monotonicity.
    public static func columns(forViewportWidthTwips viewport: Int32) -> Int32 {
        var best: Int32 = 1
        for n in Int32(1)...5 {
            if prospectiveWidth(viewport: viewport, columns: n) >= comfortablePanelWidth {
                best = n
            } else {
                break
            }
        }
        return best
    }

    /// The unit panel size (twips) for `columns` columns across `viewport`
    /// twips of width, clamped to `>= minUnitPanelWidth`. Panels are SQUARE in
    /// the original (`SetPanelsWide`, pageview.cpp:1114-1115: `SetUnitPanelWidth`
    /// and `SetUnitPanelHeight` both receive the same `nGoalPanelWidth`) -- so
    /// this single value is meant for BOTH width and height.
    ///
    /// Ported from `SetPanelsWide`'s clamp (pageview.cpp:1112-1113):
    /// ```cpp
    /// int nGoalPanelWidth = ... GetProspectivePanelWidth(nWide);
    /// nGoalPanelWidth = max(nGoalPanelWidth, MINUNITPANELWIDTH);
    /// ```
    public static func unitPanelTwips(viewportWidthTwips viewport: Int32, columns: Int32) -> Int32 {
        max(prospectiveWidth(viewport: viewport, columns: columns), minUnitPanelWidth)
    }

    /// Ported from `GetProspectivePanelWidth`'s width term only (pageview.cpp:
    /// 1143-1147; the height-driven reduction at :1149-1156 is dropped, see the
    /// type-level doc comment above):
    /// ```cpp
    /// int goalPanelWidth = (xWidth + m_vInterstice*(1-nWide)) / nWide;
    /// ```
    /// Integer division truncates toward zero in C++; Swift's `/` on `Int32`
    /// does the same for the positive operands this function is called with
    /// (viewport/columns are always >= 1 in practice), so no explicit
    /// floor/truncation adjustment is needed.
    private static func prospectiveWidth(viewport: Int32, columns: Int32) -> Int32 {
        precondition(columns >= 1, "columns must be >= 1")
        return (viewport + interstice * (1 - columns)) / columns
    }
}
