import Foundation

/// Downloads a peer's custom avatar over `URLSession` (Plan 4b Task 6 — D4
/// §4: unknown-name-WITH-URL announces enter this path). Mirrors the
/// original's `CChatFileRequest` auto-download pipeline (chat.cpp): fetch ->
/// size-cap -> validate-by-parse -> land in the destination directory under a
/// sanitized filename. One retry on failure, matching chat.cpp:2361-2363's
/// `GetRetryCount() < 1` check (exactly one extra attempt, never more).
///
/// `Sendable`: holds only an immutable `URLSession` + `Int` cap, so a single
/// instance may be shared/reused across concurrent `fetch` calls (each call
/// is independent — no shared mutable state).
public struct AvatarDownloader: Sendable {
    private let session: URLSession
    private let maxBytes: Int

    public init(session: URLSession = .shared, maxBytes: Int = 2_097_152) {
        self.session = session
        self.maxBytes = maxBytes
    }

    public enum DownloadError: Error, CustomStringConvertible {
        case oversize(Int)
        case invalidAvatar
        case transferFailed(any Error)

        public var description: String {
            switch self {
            case .oversize(let bytes):
                return "avatar download exceeded size cap (\(bytes) bytes)"
            case .invalidAvatar:
                return "downloaded file failed avatar validation"
            case .transferFailed(let error):
                return "avatar transfer failed: \(error)"
            }
        }
    }

    /// Downloads `url`, validates it parses as an `.avb` (`AvatarFile(path:)`
    /// parse success — the validator, per the brief), and moves the validated
    /// file to `dir/<sanitized name>.avb`. One retry on a transport failure
    /// (matching the original's single-retry policy); a size-cap or
    /// validation failure is NOT retried (retrying can't fix a file that IS
    /// what it is — same posture as the original, which only requeues on
    /// `CInternetRequest::statusFailed`, not on a post-download content
    /// rejection).
    ///
    /// - Parameters:
    ///   - name: the announced avatar name (`.appearsAs`'s `avatarName`,
    ///     already resolved to display form by the caller) — sanitized here
    ///     by stripping path separators and leading dots, so a hostile/odd
    ///     name (e.g. `"../evil"`) cannot escape `dir`.
    ///   - url: the avatar's source URL (http/https in production; `file://`
    ///     is exercised directly by this task's tests — `URLSession` handles
    ///     both transparently).
    ///   - dir: destination directory; must already exist (callers create it
    ///     via `ChatSessionModel.userCharactersDir`, itself created on first
    ///     use).
    /// - Returns: the final `dir/<sanitized>.avb` URL.
    public func fetch(name: String, url: URL, into dir: URL) async throws -> URL {
        let tempFile = try await downloadWithRetry(url: url)
        defer { try? FileManager.default.removeItem(at: tempFile) }

        let attributes = try FileManager.default.attributesOfItem(atPath: tempFile.path)
        let size = (attributes[.size] as? Int) ?? 0
        guard size > 0, size <= maxBytes else {
            throw DownloadError.oversize(size)
        }

        // Validate by parse (the brief's "the validator, parse success ==
        // valid .avb") — open the TEMP file directly rather than the final
        // destination, so a junk download never lands under the sanitized
        // name at all.
        guard (try? AvatarFile(path: tempFile.path)) != nil else {
            throw DownloadError.invalidAvatar
        }

        let destination = dir.appendingPathComponent(Self.sanitizedFileName(for: name))
        try? FileManager.default.removeItem(at: destination)
        try FileManager.default.copyItem(at: tempFile, to: destination)
        return destination
    }

    /// One attempt, then (on a TRANSPORT failure) exactly one retry —
    /// chat.cpp:2361-2363's `GetRetryCount () < 1` -> `IncrementRetryCount ()`
    /// -> requeue mirror. A pre-download size check via `expectedContentLength`
    /// (when the response reports one — matching the brief's "cap: 2MB
    /// pre-check via expectedContentLength when available AND hard post-check
    /// on data size") is raised as `DownloadError.oversize` OUTSIDE the
    /// retry's `catch`, so it is NOT retried — same posture as `fetch`'s own
    /// post-download size/validation checks (retrying can't fix a file that
    /// already reported its own true size). Only genuine transport failures
    /// (`session.download` throwing — timeout, connection refused, 404, ...)
    /// go through the one-retry path.
    private func downloadWithRetry(url: URL) async throws -> URL {
        let first: URL
        do {
            first = try await downloadOnce(url: url)
        } catch let oversize as DownloadError {
            throw oversize
        } catch {
            do {
                return try await downloadOnce(url: url)
            } catch let oversize as DownloadError {
                throw oversize
            } catch {
                throw DownloadError.transferFailed(error)
            }
        }
        return first
    }

    private func downloadOnce(url: URL) async throws -> URL {
        let (fileURL, response) = try await session.download(from: url)
        if let expected = response.expectedContentLength as Int64?, expected > 0,
           expected > Int64(maxBytes) {
            try? FileManager.default.removeItem(at: fileURL)
            throw DownloadError.oversize(Int(expected))
        }
        // `URLSession.download`'s temp file is deleted by the system once
        // this call returns control past its cleanup point on some
        // configurations -- copy it to our OWN temp location immediately so
        // the caller (fetch's cap/validate/move sequence above) has a stable
        // file to work with regardless.
        let ownTemp = FileManager.default.temporaryDirectory
            .appendingPathComponent("cc-avatar-dl-\(UUID().uuidString).tmp")
        try FileManager.default.moveItem(at: fileURL, to: ownTemp)
        return ownTemp
    }

    /// Strips path separators and leading dots from `name` so it cannot
    /// traverse out of the destination directory (`"../evil"` ->
    /// `"evil.avb"`) or resolve to a hidden/relative special file
    /// (`".."`/`"."`) — mirrors the resolver's own bare-name convention
    /// (append `.avb` unless already present).
    static func sanitizedFileName(for name: String) -> String {
        var cleaned = name
        cleaned.removeAll { $0 == "/" || $0 == "\\" }
        while cleaned.hasPrefix(".") { cleaned.removeFirst() }
        if cleaned.isEmpty { cleaned = "avatar" }
        return cleaned.lowercased().hasSuffix(".avb") ? cleaned : "\(cleaned).avb"
    }
}
