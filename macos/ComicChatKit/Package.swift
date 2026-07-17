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
                .define("CC_NO_RENDER"),
                .define("CC_NO_DIRSCAN"),
                .define("CC_NO_UI"),
                .define("CC_NO_PROTOCOL"),
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
