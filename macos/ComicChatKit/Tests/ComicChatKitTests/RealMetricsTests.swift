import Testing
import Foundation
import CoreGraphics
import cchat_engine
@testable import ComicChatKit

// Plan 4a Task 4: real CoreText layout metrics. Nested inside
// EngineGlobalStateSelfTests (.serialized, BodyDrawTests.swift) for the same
// reason every other engine-global-state suite is (StripTests.swift's doc
// comment): `cc_set_metrics_canvas`/`cc_strip_create` mutate shared
// process-global engine state with no internal locking, so these tests must
// never run concurrently with the C selftests or any other Strip-driving
// suite.
extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct RealMetricsTests {
    // (a) Tolerance-based value test: CoreTextMetrics.metrics(for:) against
    // Comic Sans MS 12pt (LOGFONT lfHeight -240 twips), measured directly on
    // this machine (`/System/Library/Fonts/Supplemental/Comic Sans MS.ttf`,
    // unitsPerEm=2048) via CTFontGetAscent/Descent/Leading and raw OS/2/hhea
    // table reads -- see the task report for the exact measured values this
    // range brackets.
    //
    // NOTE on aveCharWidth's range (widened from the brief's original
    // (115...155)): this machine's Comic Sans MS OS/2 table has
    // xAvgCharWidth=959 at unitsPerEm=2048, which scales to ~112.4 twips at
    // 12pt -- OUTSIDE (115...155). That table value is real (verified against
    // the raw font-file bytes directly, bypassing CoreText entirely), not a
    // CoreText quirk: OS/2 xAvgCharWidth is an English-letter-frequency
    // weighted average per the OS/2 spec, not a plain arithmetic mean over
    // a-z, so it can legitimately sit below the simple lowercase-average
    // fallback (~124.9 twips, which the brief's range was centered on). Per
    // Task 4's own convention (table-read primary, measurement fallback only
    // when the table is absent/unreadable), the table value is authoritative
    // here and the table read must not be abandoned just because a fallback
    // computation would have landed in a tighter band. The range below covers
    // both this machine's real table value and the brief's original
    // measured-fallback figure, so it stays meaningful if run against a
    // Comic Sans MS build whose OS/2 table is closer to the letter-average.
    @Test func comicSansRealMetrics() {
        let m = CoreTextMetrics.metrics(for: FontSpec(face: "Comic Sans MS", height: -240,
                                                      weight: 400, italic: false,
                                                      underline: false, strikeout: false, charset: 0))
        #expect((260...270).contains(m.ascent))          // measured 264.5 tw
        #expect((66...74).contains(m.descent))           // measured 70.0
        #expect(m.height == m.ascent + m.descent)        // 334.5
        #expect((88...101).contains(m.internalLeading))  // height − |lfHeight| ≈ 94.5
        #expect(m.externalLeading == 0)                  // Comic Sans has no line gap
        #expect((108...155).contains(m.aveCharWidth))    // OS/2 xAvgCharWidth ≈ 112.4 (this build) / measured fallback ≈ 124.9
        #expect((240...310).contains(m.maxCharWidth))    // hhea advanceWidthMax ≈ 297.9
    }

    // (b) Layout-effect test: the SAME script composed twice, once under the
    // fake RecordingCanvas metrics (measure_text = len*120 x 240, fontMetrics
    // height=240 -- ~207 twip line cell incl. leading) and once under real
    // CTMetricsCanvas metrics (Comic Sans 12pt real line height ~334.5 tw,
    // per (a) above) -- both must compose without error.
    //
    // WHY A LONG LINE, NOT THE SHORT 2-LINE DEMO (discovery finding, binding):
    // panels are FIXED 2300x2300-twip unit squares (verified against
    // Fixtures/strip-golden.txt) -- `strip.size`/`cc_strip_get_size`
    // (panel.cpp CUnitPanelPage::GetBBox) is a pure function of PANEL COUNT
    // and fixed geometry constants, never of any balloon bbox directly. Real
    // metrics only reach `strip.size` THROUGH the panel-break threshold:
    // CLabel::SplitHeight (balloon.cpp) computes
    // `iMaxLines = (iHeight - BORDERFUDGE) / m_fontI->m_lineHeight` -- a
    // taller real line height lowers iMaxLines for the SAME fixed free-rect
    // height, so a long single utterance that fits one panel under fake
    // metrics' short line height can overflow into a second panel under real
    // metrics' taller one. A short "Hello there"/"Hi yourself" script (as
    // used by StripTests' golden fixture) never crosses that threshold under
    // EITHER metrics regime, so it would assert nothing here (confirmed: that
    // exact script produces h=2300 under both canvases). This test instead
    // uses one long single-speaker line engineered to sit exactly at that
    // threshold -- fitting one panel under fake's ~207 tw line cell, but
    // overflowing to two panels (one extra row -> +2444 tw height, panel.cpp's
    // m_unitHeight+m_hInterstice) under real's ~334.5 tw line cell.
    //
    // Each strip gets its own metrics canvas installed before ITS OWN
    // cc_strip_create (metrics are measured once per strip, at create/layout
    // time -- brief's own framing), sequentially so there is never more than
    // one live cc_strip/metrics-canvas pair at once (comicchat.h's "ONE STRIP
    // AT A TIME" contract).
    @Test func realMetricsGrowLineHeight() throws {
        let avatar = fixture("anna.avb")
        let backdrop = fixture("field.bgb")
        // Long enough to wrap into several lines within one balloon; short
        // words (no forced mid-word breaks) so wrapping is metrics-driven.
        let longLine = "Hello there my good friend it is certainly a very " +
                       "fine and pleasant day today for a nice long chat " +
                       "about all sorts of interesting and wonderful things " +
                       "do you not agree with me on this fine point"

        func composeStrip(metricsCanvas: Canvas) throws -> (w: Int32, h: Int32, panels: Int32) {
            let metricsBox = CanvasBox(metricsCanvas)
            cc_set_metrics_canvas(metricsBox.handle)
            return try withExtendedLifetime(metricsBox) {
                let strip = try Strip()
                let a = try strip.addParticipant(nick: "Anna", avbPath: avatar)
                let b = try strip.addParticipant(nick: "Boris", avbPath: avatar)
                try strip.setBackdrop(backdrop)
                try strip.addLine(speaker: a, text: longLine, modes: .say, addressees: [b])

                let (w, h) = strip.size
                #expect(w > 0 && h > 0)

                // Compose onto a throwaway CGCanvas -- both metrics-canvas
                // variants must compose without engine error, matching the
                // brief's "both compose without error" requirement. The point
                // of this test is the LAYOUT size (from `strip.size`, computed
                // before compose is even called), not the composed pixels.
                let drawing = CGCanvas(widthTwips: w, heightTwips: h, scale: 1.0)
                try strip.compose(onto: drawing)

                return (w, h, strip.panelCount)
            }
        }

        let fakeSize = try composeStrip(metricsCanvas: RecordingCanvas())
        let realSize = try composeStrip(metricsCanvas: CTMetricsCanvas())

        #expect(realSize.h > fakeSize.h,
            "real CoreText metrics (Comic Sans 12pt line height ~334.5 tw) must grow the composed page height past the fake RecordingCanvas metrics (~207 tw line cell) once the long line crosses the panel-break threshold -- got fake=\(fakeSize.h) (panels=\(fakeSize.panels)) real=\(realSize.h) (panels=\(realSize.panels))")
    }
  }
}
