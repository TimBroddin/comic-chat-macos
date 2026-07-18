import Foundation
import cchat_engine

/// Errors surfaced by the ComicChatKit C bridge. Grows in later plans.
public enum ComicChatError: Error, Equatable {
    case artLoadFailed(path: String)
}

/// A decoded RGBA8 image: straight (non-premultiplied) alpha, top-to-bottom,
/// row-major, 4 bytes per pixel.
public struct ArtImage {
    public let width: Int
    public let height: Int
    public let rgba: Data

    fileprivate init(_ image: cc_image) {
        width = Int(image.width)
        height = Int(image.height)
        if let base = image.rgba, width > 0, height > 0 {
            rgba = Data(bytes: base, count: width * height * 4)
        } else {
            rgba = Data()
        }
    }
}

/// Wraps an opened .avb avatar file (cc_avatar*). Poses are the avatar's
/// displayable gesture/expression images (the icon pose is excluded — see
/// ArtFile.swift's bridge_art.cpp counterpart for why).
public final class AvatarFile {
    private let handle: OpaquePointer

    public init(path: String) throws {
        guard let handle = cc_avatar_open(path) else {
            throw ComicChatError.artLoadFailed(path: path)
        }
        self.handle = handle
    }

    deinit {
        cc_avatar_close(handle)
    }

    public var name: String {
        String(cString: cc_avatar_name(handle))
    }

    public var poseCount: Int {
        Int(cc_avatar_pose_count(handle))
    }

    public func poseName(_ idx: Int) -> String {
        String(cString: cc_avatar_pose_name(handle, Int32(idx)))
    }

    public func poseImage(_ idx: Int) throws -> ArtImage {
        var image = cc_image()
        let rc = cc_avatar_pose_image(handle, Int32(idx), &image)
        guard rc == 0 else {
            throw ComicChatError.artLoadFailed(path: "\(name) pose \(idx)")
        }
        defer { cc_image_free(&image) }
        return ArtImage(image)
    }

    /// The member-list/picker icon pose (excluded from `poseCount`/`poseImage`
    /// by design — see this file's class doc comment).
    public func iconImage() throws -> ArtImage {
        var image = cc_image()
        let rc = cc_avatar_icon_image(handle, &image)
        guard rc == 0 else {
            throw ComicChatError.artLoadFailed(path: "\(name) icon")
        }
        defer { cc_image_free(&image) }
        return ArtImage(image)
    }
}

/// Wraps an opened .bgb (or .bmp) backdrop file (cc_backdrop*). Backdrops
/// have no transparency concept — the decoded image is always fully opaque.
public final class BackdropFile {
    private let handle: OpaquePointer

    public init(path: String) throws {
        guard let handle = cc_backdrop_open(path) else {
            throw ComicChatError.artLoadFailed(path: path)
        }
        self.handle = handle
    }

    deinit {
        cc_backdrop_close(handle)
    }

    public var name: String {
        String(cString: cc_backdrop_name(handle))
    }

    public func image() throws -> ArtImage {
        var image = cc_image()
        let rc = cc_backdrop_image(handle, &image)
        guard rc == 0 else {
            throw ComicChatError.artLoadFailed(path: name)
        }
        defer { cc_image_free(&image) }
        return ArtImage(image)
    }
}
