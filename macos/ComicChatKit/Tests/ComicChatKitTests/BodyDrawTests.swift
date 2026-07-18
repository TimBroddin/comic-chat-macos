import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// These C selftests drive the process-global avatar registry (InitializeAvatars
// / IndexAvatar / GetAvatar over the shared `avatars` array) AND the shared
// engine context/session + metrics canvas. They therefore must NOT run
// concurrently with each other -- Swift Testing runs tests in parallel by
// default, and two of these racing on that global state crashes (SIGSEGV) and
// corrupts avatar-id assignment. A serialized suite forces them one at a time.
// (The AvatarFile/BackdropFile wrappers in ArtTests/PNGExport load standalone
// art without touching the global registry, so they need no serialization.)
@Suite(.serialized)
struct EngineGlobalStateSelfTests {
    // The C++ selftest battery (cc_run_selftests) mutates the same process-global
    // engine context/session + metrics-canvas + font statics (cc_selftest_balloon
    // sets CUnitPanelPage's CFontInfo statics + m_unitWidth), so it must be
    // serialized WITH the avatar-registry tests below -- otherwise it races the
    // panel test's reads of those statics and balloon sizing goes nondeterministic.
    @Test func engineSelfTestsPass() {
        #expect(cc_run_selftests() == 0)
    }

    // Plan 2 Task 7: the CBody draw path (bodycam.cpp CBody* methods, now LIVE).
    // Opens a real avatar, builds a body, draws it through a recording canvas,
    // and asserts the image-blit count + dest rects match GetBodyBox with no
    // ASSERT traps. Needs the fixture path (cc_run_selftests takes none).
    @Test func bodyDrawSelfTestPasses() {
        let path = fixture("anna.avb")
        #expect(cc_run_bodydraw_selftest(path) == 0)
    }

    // Plan 2 Task 8: the panel orchestrator + body-placement camera (panel.cpp,
    // now LIVE). Loads the anna.avb fixture (twice -> two avatars A/B, plus four
    // more for the panel-break/pull-in cases), wires session users + the talk-to
    // graph, drives LayoutAvatars/AddLine, and characterizes camera facing/order,
    // the panel-break rules, orchestration (panel count + balloon bboxes inside
    // panel unit rects), Establishing()'s zoom gate, and the 3-party absent-
    // addressee pull-in (the userFromTalkTo reconstruction path).
    @Test func panelSelfTestPasses() {
        let path = fixture("anna.avb")
        #expect(cc_run_panel_selftest(path) == 0)
    }

    // Plan 2 Task 10: the cc_strip session API + headless compositor -- the task
    // that drives the entire lifted layout engine end-to-end for the first time.
    // Opens two participants (anna.avb twice), sets a backdrop (field.bgb),
    // ingests a fixed 2x4 alternating conversation through the panel
    // orchestrator, and composites the finished page onto a recording canvas via
    // cc_strip_compose (the R16 replacement for CUnitPanelPage::Draw). Asserts
    // panel_count, get_size==GetBBox, and the FULL frozen compose-log snapshot.
    // Serialized here because it mutates the same process-global registry +
    // session + font statics + backdrop registries as the tests above.
    @Test func stripSelfTestPasses() {
        let avatar = fixture("anna.avb")
        let backdrop = fixture("field.bgb")
        #expect(cc_run_strip_selftest(avatar, backdrop) == 0)
    }

    // Plan 4a Task 5: the panel geometry API (cc_strip_set_panel_geometry /
    // cc_strip_get_panel_geometry) -- thin wrappers over
    // CUnitPanelPage::SetUnitPanelWidth/SetUnitPanelHeight/SetUnitPanelsPerRow +
    // the interstice statics. Exercises the create-time default, a set-then-get
    // round trip, cc_strip_get_size reflecting the new arithmetic once real
    // lines are added, and the FRESH STRIP ONLY reject once a line exists.
    // Needs the fixture path (one real participant, so AddLine's
    // FetchSpeaker->GetAvatar->m_body chain doesn't dereference null).
    @Test func panelGeometrySelfTestPasses() {
        let avatar = fixture("anna.avb")
        #expect(cc_run_panel_geometry_selftest(avatar) == 0)
    }

    // Plan 4a Task 6: the avatar API (cc_avatar_icon_image +
    // cc_strip_set_participant_avatar). Checks the icon-pose decode on a
    // standalone cc_avatar, then a strip with one participant whose avatar is
    // switched mid-strip (bad participant id rejected, good one succeeds,
    // panel count grows, and the strip still composes cleanly afterward).
    @Test func avatarApiSelfTestPasses() {
        let avatar = fixture("anna.avb")
        let other = fixture("armando.avb")
        #expect(cc_run_avatar_api_selftest(avatar, other) == 0)
    }
}
