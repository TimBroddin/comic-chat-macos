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
            // EmotionToBytes packs IndexToByte(v) = v + '0' bytes; intensity
            // index range is 0...10 (BYTE)(m_intensity*10), so both intensity
            // fields must land in ['0', '0'+10] -- the same structural check
            // the C selftest makes (cc_selftest.cpp's cc_selftest_self_emotion).
            let zero = Int32(Character("0").asciiValue!)
            #expect(ann.faceIntensity >= zero && ann.faceIntensity <= zero + 10)
            #expect(ann.gestureIntensity >= zero && ann.gestureIntensity <= zero + 10)
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
