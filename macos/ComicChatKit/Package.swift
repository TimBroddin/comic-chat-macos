// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "ComicChatKit",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "ComicChatKit", targets: ["ComicChatKit"]),
        .executable(name: "cc-dumpart", targets: ["cc-dumpart"]),
    ],
    targets: [
        .target(
            name: "cchat-engine",
            path: "Sources/cchat-engine",
            publicHeadersPath: "include",
            cxxSettings: [
                .headerSearchPath("shim"),
                .headerSearchPath("engine"),
                .headerSearchPath("bridge"),
                // CC_NO_RENDER retired in Plan 2 Task 7: the CDIB::Draw /
                // CBackDrop::Draw bodies (dib.cpp/backdrop.cpp) and the CBody
                // draw path (bodycam.cpp) are now live against the CDC adapter.
                .define("CC_NO_DIRSCAN"),
                .define("CC_NO_UI"),
                // CC_NO_PROTOCOL retired in Plan 3 Task 3: EmotionToBytes/
                // BytesToEmotion (avatario.cpp) and the annotation codec
                // (protsupp.cpp) are now live.
                .unsafeFlags(["-Wno-tautological-undefined-compare"]),
            ],
            linkerSettings: [
                .linkedLibrary("z")
            ]
        ),
        .target(
            name: "ComicChatKit",
            dependencies: ["cchat-engine"],
            path: "Sources/ComicChatKit"
        ),
        .testTarget(
            name: "ComicChatKitTests",
            dependencies: ["ComicChatKit"],
            resources: [.copy("Fixtures")]
        ),
        .executableTarget(
            name: "cc-dumpart",
            dependencies: ["ComicChatKit"],
            path: "Sources/cc-dumpart"
        ),
    ],
    cxxLanguageStandard: .cxx17
)
