import SwiftUI
import ComicChatKit

/// The emotion wheel (original CBodyCam, bodycam.cpp): a COMPACT bulls-eye +
/// detente ring + 8 face icons ringed TIGHTLY just outside the bull, at
/// 2π·i/8 (y-up, happy=east; order = lg_icons, bodycam.cpp:49-59). Drag inside
/// the bull radius sets emotion (direction) × intensity (distance, 0.2
/// detente).
///
/// Live-fix 7 (original-style right column): this view is now WHEEL-ONLY — the
/// posed self-avatar preview moved OUT to `PosePreviewPane`, the large
/// full-body pane above the wheel, matching the original `CBodyCam` layout
/// (the posed avatar occupies the tall upper pane; the bulls-eye sits in a
/// short pane beneath it, chatview.cpp:333-378). The wheel therefore renders
/// no pose image and packs the icons tightly around a small bulls-eye so it
/// fits the ~1/4-column-height pane the restructured column gives it.
struct BodyCamView: View {
    var onEmotion: (Double, Double) -> Void       // (angleRadians, intensity01)

    private static let iconFiles = ["fc_hap_l", "fc_coy_l", "fc_bor_l", "fc_sca_l",
                                    "fc_sad_l", "fc_ang_l", "fc_sho_l", "fc_laf_l"]
    private static let icons: [NSImage] = iconFiles.compactMap {
        Bundle.main.url(forResource: $0, withExtension: "bmp", subdirectory: "wheel")
            .flatMap { NSImage(contentsOf: $0) }
    }

    var body: some View {
        GeometryReader { geo in
            let side = min(geo.size.width, geo.size.height)
            let center = CGPoint(x: geo.size.width/2, y: geo.size.height/2)
            // Compact bulls-eye: a small central target with the 8 icons ringed
            // just outside it (tight spacing, matching the original's compact
            // pane). The icons sit at `bullRadius + a small gap`, close to the
            // bull rather than flung to the pane edges.
            let bullRadius = side * 0.30
            let iconOffset = bullRadius + side * 0.13
            ZStack {
                // Outer bull ring + inner detente ring (the 0.2 center detente
                // target, bodycam.cpp:404).
                Circle().stroke(.secondary).frame(width: bullRadius*2, height: bullRadius*2)
                Circle().stroke(.secondary.opacity(0.5))
                    .frame(width: bullRadius*0.4, height: bullRadius*0.4)
                ForEach(0..<8, id: \.self) { i in
                    let angle = 2 * .pi * Double(i) / 8
                    let pos = CGPoint(x: center.x + iconOffset * cos(angle),
                                      y: center.y - iconOffset * sin(angle))   // y-up -> AppKit y-down
                    if i < Self.icons.count {
                        Image(nsImage: Self.icons[i]).position(pos)
                    }
                }
            }
            .clipped()
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { g in
                let vx = g.location.x - center.x
                let vy = center.y - g.location.y                     // back to y-up
                var intensity = min(sqrt(vx*vx + vy*vy) / bullRadius, 1.0)
                if intensity < 0.2 { intensity = 0 }                  // the detente (bodycam.cpp:404)
                let angle = intensity == 0 ? 0 : atan2(vy, vx)        // bodycam.cpp:405
                onEmotion(angle, intensity)
            })
        }
        .frame(minHeight: 120)
    }
}

/// The large self-pose pane (original `CBodyCam`'s upper region, bodycam.cpp:
/// draws the posed avatar full-body above the bulls-eye): the user's own
/// avatar standing FULL-BODY (head + torso composited by the engine's
/// `DrawBody`, via `Strip.selfPreviewImage` — live-fix 7), aspect-fit in a tall
/// bordered pane. A plain-value `Image` from `AppState.selfPoseImage` (the
/// parameter-passing contract: the parent reads `appState`, this child takes
/// the plain `CGImage?`). Shows nothing until a self pose has been emitted.
struct PosePreviewPane: View {
    var poseImage: CGImage?

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 6)
                .fill(Color(nsColor: .textBackgroundColor))
            RoundedRectangle(cornerRadius: 6)
                .stroke(.secondary.opacity(0.3), lineWidth: 1)
            if let poseImage {
                // Full-body, aspect-fit, pinned to the bottom of the pane like
                // the original (the standing figure rests on the pane floor).
                Image(decorative: poseImage, scale: 2)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
                    .padding(6)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
