import Testing
import Foundation
@testable import ComicChatKit

// PanelFit is pure math (no engine-global state touched), so these need no
// serialization -- unlike EngineGlobalStateSelfTests's descendants, and unlike
// the cc_strip_* geometry API this task also adds (which IS serialized,
// BodyDrawTests.swift's panelGeometrySelfTestPasses). See PanelFit.swift for
// the provenance of every constant/formula ported here (pageview.cpp
// FitPanelsWide / GetProspectivePanelWidth / SetPanelsWide, none of which were
// lifted into the engine -- this is a fresh Swift port of view code, not a
// characterization of lifted C++).

@Test func fitColumns() {
    // viewport twips -> expected columns: largest 1...5 whose panel width is
    // still "comfortable" (>= COMFORTABLEPANELWIDTH, 3000 twips). Values
    // verified by hand against the original's integer-division arithmetic
    // (pageview.cpp:1147 `(xWidth + vInterstice*(1-n)) / n`, vInterstice=144):
    //   n=1: (2500+0)/1=2500; (6200+0)/1=6200; (9400+0)/1=9400; (40000+0)/1=40000
    //   n=2: (v-144)/2            -> 6200: 3028 (>=3000, keep going)
    //   n=3: (v-288)/3            -> 9400: 3037 (>=3000, keep going)
    //   n=4: (40000-432)/4=9892; n=5: (40000-576)/5=7884 (both still >=3000,
    //        so FitPanelsWide's loop runs to n=5 and stops there -- capped).
    #expect(PanelFit.columns(forViewportWidthTwips: 2500) == 1)   // below comfortable → 1
    #expect(PanelFit.columns(forViewportWidthTwips: 6200) == 2)
    #expect(PanelFit.columns(forViewportWidthTwips: 9400) == 3)
    #expect(PanelFit.columns(forViewportWidthTwips: 40000) == 5)  // capped at 5
    #expect(PanelFit.unitPanelTwips(viewportWidthTwips: 6200, columns: 2) >= 2300)
}

// Additional derivation checks (not in the brief's guessed table, added while
// verifying the ported arithmetic honestly matches the original -- see the
// Task 5 report for the full derivation).
@Test func fitColumnsBreaksAtFirstUncomfortableWidth() {
    // FitPanelsWide (pageview.cpp:1394-1404) BREAKS the loop at the first n
    // whose width drops below COMFORTABLEPANELWIDTH -- it does not keep
    // scanning higher n hoping for a rebound. n=1 width is always the full
    // viewport (no interstice subtracted), so it's comfortable down to 3000
    // twips; below that, best stays at its initial value of 1.
    #expect(PanelFit.columns(forViewportWidthTwips: 3000) == 1)
    #expect(PanelFit.columns(forViewportWidthTwips: 2999) == 1)
}

@Test func unitPanelTwipsClampsToMinimum() {
    // MINUNITPANELWIDTH (panel.h:165) = 2300. A tiny viewport at 1 column
    // would otherwise produce an unusably small (or even negative-trending)
    // panel; SetPanelsWide (pageview.cpp:1113) clamps with max(..., MINUNITPANELWIDTH).
    #expect(PanelFit.unitPanelTwips(viewportWidthTwips: 100, columns: 1) == 2300)
}

@Test func unitPanelTwipsIsSquare() {
    // pageview.cpp:1114-1115: SetUnitPanelWidth and SetUnitPanelHeight are both
    // set to the SAME goalPanelWidth -- panels are square in the original.
    // PanelFit.unitPanelTwips returns that one shared value; callers use it for
    // both width and height (documented on the function).
    let w = PanelFit.unitPanelTwips(viewportWidthTwips: 9400, columns: 3)
    #expect(w == 3037)
}

// Panel export/copy (Plan 4b Batch D): PanelFit.panelIndex(atTwips:...) is the
// SAME grid arithmetic cc_compose.cpp's hitTestWalk uses (rowNum = pNum/perRow,
// colNum = pNum%perRow, ulx = colNum*(unitW+vInterstice), uly =
// -rowNum*(unitH+hInterstice), panel spans x in [ulx,ulx+unitW], y in
// [uly-unitH,uly]) — reimplemented in pure Swift so the strip view's
// right-click handler can compute "which panel did I click" locally, without
// an engine-queue hop (the view already has panelGeometry from a prior
// engine-queue read). Uses the SAME fixed geometry as StripTests/
// cc_selftest_strip's frozen 2x4 conversation: unitW=unitH=2300, perRow=2,
// interstices=144 -- so panel 0 origin (0,0), panel 1 origin (2444,0), panel 2
// origin (0,-2444), panel 3 origin (2444,-2444) (cc_compose.cpp's own
// documented per-panel origins for this exact geometry).
@Test func panelIndexMatchesEngineGridArithmetic() {
    let unitW: Int32 = 2300, unitH: Int32 = 2300, perRow: Int32 = 2
    let hInter: Int32 = 144, vInter: Int32 = 144
    let panelCount: Int32 = 4

    func idx(_ x: Int32, _ y: Int32) -> Int32? {
        PanelFit.panelIndex(atTwips: x, y, unitW: unitW, unitH: unitH, perRow: perRow,
                            hInterstice: hInter, vInterstice: vInter, panelCount: panelCount)
    }

    // Panel 0: x in [0,2300], y in [-2300,0]. Center hits panel 0.
    #expect(idx(1150, -1150) == 0)
    // Panel 1: origin (2444,0) -> x in [2444,4744], y in [-2300,0].
    #expect(idx(2444 + 1150, -1150) == 1)
    // Panel 2: origin (0,-2444) -> x in [0,2300], y in [-4744,-2444].
    #expect(idx(1150, -2444 - 1150) == 2)
    // Panel 3: origin (2444,-2444) -> x in [2444,4744], y in [-4744,-2444].
    #expect(idx(2444 + 1150, -2444 - 1150) == 3)

    // A point in the interstice GAP between panel 0 and panel 1 (x in
    // (2300,2444)) is in NO panel's slot.
    #expect(idx(2372, -1150) == nil)
    // A point off the page entirely (below the last row) is in no panel.
    #expect(idx(1150, -20000) == nil)
    // A point whose GRID SLOT would exist but panelCount doesn't reach it
    // (e.g. a 3-panel strip's slot 3, which would be the row/col the grid
    // arithmetic computes but no CPanel actually occupies) -> nil, not a
    // stale/out-of-range index.
    #expect(PanelFit.panelIndex(atTwips: 2444 + 1150, -2444 - 1150, unitW: unitW, unitH: unitH,
                                perRow: perRow, hInterstice: hInter, vInterstice: vInter,
                                panelCount: 3) == nil)
}
