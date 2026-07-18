import Testing
import Foundation
import cchat_engine
@testable import ComicChatKit

// Plan 4b Task 2: the emotion-wheel engine surface, exercised through the
// typed `Strip` wrapper (setSelfEmotion/previewSelfText/selfPoseIndex/
// selfAnnotations) rather than the raw C entry points (the C-level coverage
// lives in cc_selftest.cpp's cc_selftest_self_emotion, driven from
// BodyDrawTests.swift).
//
// SERIALIZATION: like every other Strip-driving suite, this mutates the
// process-global avatar registry/session/metrics canvas -- nested inside
// EngineGlobalStateSelfTests (.serialized) per StripTests.swift's doc
// comment, so it never races the C selftest or any other Strip test.
extension EngineGlobalStateSelfTests {
  @Suite(.serialized)
  struct SelfEmotionTests {
    // (1) Strip wrapper round-trip: setSelfEmotion(angle: 0, intensity: 1)
    // then selfAnnotations() has cooked == true and nonzero face/gesture
    // emotion+intensity wire fields. Uses the armando.avb fixture path
    // (matching the C selftest's fixture choice).
    @Test func setSelfEmotionThenAnnotationsIsCookedWithNonzeroFields() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let strip = try Strip()
            let avatar = fixture("armando.avb")
            let self_ = try strip.addParticipant(nick: "Self", avbPath: avatar)
            try strip.setSelf(self_)

            try strip.setSelfEmotion(angle: 0.0, intensity: 1.0)
            let ann = try strip.selfAnnotations()

            #expect(ann.cooked == true)
            // cc_annotations stores RAW indices, NOT EmotionToBytes' +'0' wire
            // bytes (comicchat.h's field comment: "Values are indices, NOT the
            // +'0' wire bytes"; cc_strip_self_annotations unwraps via
            // ByteToIndex before storing -- review Critical fix, see
            // cc_compose.cpp). Intensity index range is 0...10
            // (BYTE)(m_intensity*10) and emotion index range is 0...17
            // (avatario.cpp's emFloats table, 18 entries) -- the same
            // structural check the C selftest makes
            // (cc_selftest.cpp's cc_selftest_self_emotion).
            #expect(ann.faceIntensity >= 0 && ann.faceIntensity <= 10)
            #expect(ann.gestureIntensity >= 0 && ann.gestureIntensity <= 10)
            #expect(ann.faceEmotion >= 0 && ann.faceEmotion <= 17)
            #expect(ann.gestureEmotion >= 0 && ann.gestureEmotion <= 17)

            // Round-trip pin: setSelfEmotion(angle: 0, intensity: 1.0) drives
            // GetBodyFromEmotion's nearest-neighbor snap to the closest
            // (angle, intensity) body record the armando.avb fixture actually
            // has (avatar.cpp's CAvatarComplex::GetBodyFromEmotion /
            // CAvatarSimple::GetBodyFromEmotion, both a nearest-match search,
            // NOT a verbatim store of the requested intensity) -- so the
            // resulting CEmotion.m_intensity GetEmotions later reads back is
            // whatever discrete value that nearest record carries, which can
            // differ between the face and torso/gesture groups depending on
            // what records the fixture defines for each. Verified empirically
            // for this fixture: the FACE group's nearest record is an exact
            // intensity-1.0 match, so its raw intensity index is exactly 10
            // per EmotionToBytes' quantization (BYTE)(m_intensity * 10)
            // (avatario.cpp:83, truncating, no rounding) -- pinning that
            // exact value here catches a re-introduced wire-byte bug (which
            // would produce 58 = '0'+10, not 10) even if the range checks
            // above were loosened by mistake. The gesture/torso group is
            // NOT pinned to an exact value (only the range check above)
            // because this fixture's torso records don't include an
            // intensity-1.0 match near angle 0, so its raw value is fixture-
            // specific, not derivable from the formula alone.
            #expect(ann.faceIntensity == 10)
        }
    }

    // (2) previewSelfText changes selfPoseIndex() vs. a neutral baseline.
    // "Hello there, friend!" fires ID_RULE_WAVE's CheckStart* clause
    // (EM_WAVE, intensity 1.0, priority 5 -- verified in cc_selftest_textpose
    // case 4, cc_selftest.cpp), a different emotion than the neutral (angle
    // 0, intensity 0) baseline set just before, so the resulting pose must
    // differ.
    @Test func previewSelfTextChangesSelfPose() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let strip = try Strip()
            let avatar = fixture("armando.avb")
            let self_ = try strip.addParticipant(nick: "Self", avbPath: avatar)
            try strip.setSelf(self_)

            try strip.setSelfEmotion(angle: 0.0, intensity: 0.0)   // neutral-ish baseline
            let prePose = try strip.selfPoseIndex()

            try strip.previewSelfText("Hello there, friend!")
            let postPose = try strip.selfPoseIndex()

            #expect(postPose != prePose)
        }
    }

    // (3) A cooked addLineCooked with the annotations built from current
    // avatar state composes without error (smoke test: panelCount grows).
    @Test func cookedAddLineWithSelfAnnotationsComposes() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let strip = try Strip()
            let avatar = fixture("armando.avb")
            let self_ = try strip.addParticipant(nick: "Self", avbPath: avatar)
            try strip.setSelf(self_)

            try strip.setSelfEmotion(angle: 0.0, intensity: 1.0)
            var ann = try strip.selfAnnotations()
            ann.mode = 1   // SM_SAY, wire mode (Task 2's report: cc_annotations.mode
                           // carries the raw SM_* value 1..5, not CC_MODE_*/BM_*)

            let before = strip.panelCount
            try strip.addLineCooked(speaker: self_, text: "Hello there", modes: .say,
                                     addressees: [], annotations: ann)
            let after = strip.panelCount

            #expect(after > before)

            let recorder = RecordingCanvas()
            try strip.compose(onto: recorder)
            #expect(!recorder.log.isEmpty)
        }
    }

    // (4) All four Strip wrapper calls throw on a strip with no self set.
    @Test func requiresSelfSet() throws {
        let metricsCanvas = RecordingCanvas()
        let metricsBox = CanvasBox(metricsCanvas)
        cc_set_metrics_canvas(metricsBox.handle)

        try withExtendedLifetime(metricsBox) {
            let strip = try Strip()
            // No addParticipant/setSelf at all.
            #expect(throws: (any Error).self) {
                try strip.setSelfEmotion(angle: 0.0, intensity: 1.0)
            }
            #expect(throws: (any Error).self) {
                try strip.previewSelfText("Hello there, friend!")
            }
            #expect(throws: (any Error).self) {
                try strip.selfPoseIndex()
            }
            #expect(throws: (any Error).self) {
                try strip.selfAnnotations()
            }
        }
    }
  }
}
