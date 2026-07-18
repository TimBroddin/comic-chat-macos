import SwiftUI
import ComicChatKit

/// The emotion wheel (original CBodyCam, bodycam.cpp): bulls-eye + 8 face
/// icons at 2π·i/8 (y-up, happy=east; order = lg_icons, bodycam.cpp:49-59)
/// + the self avatar's live pose preview behind it. Drag inside the bull
/// radius sets emotion (direction) × intensity (distance, 0.2 detente).
struct BodyCamView: View {
    var poseImage: CGImage?
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
            let bullRadius = side * 0.28
            let iconOffset = bullRadius + side * 0.14
            ZStack {
                if let poseImage {
                    Image(decorative: poseImage, scale: 2).resizable()
                        .aspectRatio(contentMode: .fit).frame(width: side*0.5).opacity(0.9)
                }
                Circle().stroke(.secondary).frame(width: bullRadius*2, height: bullRadius*2)
                Circle().stroke(.secondary.opacity(0.5)).frame(width: bullRadius*0.4, height: bullRadius*0.4)
                ForEach(0..<8, id: \.self) { i in
                    let angle = 2 * .pi * Double(i) / 8
                    let pos = CGPoint(x: center.x + iconOffset * cos(angle),
                                      y: center.y - iconOffset * sin(angle))   // y-up -> AppKit y-down
                    if i < Self.icons.count {
                        Image(nsImage: Self.icons[i]).position(pos)
                    }
                }
            }
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
        .frame(minWidth: 120, minHeight: 120)
    }
}
