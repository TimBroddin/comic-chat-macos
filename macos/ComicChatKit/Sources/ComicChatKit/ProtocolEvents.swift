import Foundation
import cchat_engine

/// Mirrors `cc_annotations` (comicchat.h) — a decoded comic "User Display
/// Info" block (pose/emotion/mode/addressees). Values are indices, not the
/// `+'0'` wire bytes (the engine already decodes those). `addressees` are
/// display-ready nick strings (already run through `WireCodec`).
public struct Annotations: Sendable, Equatable {
    public var gesturePose: Int32
    public var gestureEmotion: Int32
    public var gestureIntensity: Int32
    public var facePose: Int32
    public var faceEmotion: Int32
    public var faceIntensity: Int32
    public var requested: Bool
    public var mode: Int32
    public var addressees: [String]
    /// Both intensity fields were present on the wire (`cc_annotations.cooked`).
    public var cooked: Bool

    public init(gesturePose: Int32 = 0, gestureEmotion: Int32 = 0, gestureIntensity: Int32 = 0,
                facePose: Int32 = 0, faceEmotion: Int32 = 0, faceIntensity: Int32 = 0,
                requested: Bool = false, mode: Int32 = 0, addressees: [String] = [],
                cooked: Bool = false) {
        self.gesturePose = gesturePose
        self.gestureEmotion = gestureEmotion
        self.gestureIntensity = gestureIntensity
        self.facePose = facePose
        self.faceEmotion = faceEmotion
        self.faceIntensity = faceIntensity
        self.requested = requested
        self.mode = mode
        self.addressees = addressees
        self.cooked = cooked
    }

    /// Build from the raw C struct, decoding the fixed-size addressee byte
    /// arrays via `WireCodec`.
    init(cAnnotations ann: cc_annotations, encoding: WireEncoding) {
        gesturePose = ann.gesture_pose
        gestureEmotion = ann.gesture_emotion
        gestureIntensity = ann.gesture_intensity
        facePose = ann.face_pose
        faceEmotion = ann.face_emotion
        faceIntensity = ann.face_intensity
        requested = ann.requested != 0
        mode = ann.mode
        cooked = ann.cooked != 0
        let count = Int(ann.addressee_count)
        // cc_annotations.addressees is `char[CC_MAX_ADDRESSEES][64]`, a flat
        // contiguous C array — walk it via raw pointer arithmetic rather than
        // fighting Swift's tuple-of-tuples import of nested C arrays.
        addressees = Annotations.decodeAddressees(ann, count: count, encoding: encoding)
    }

    private static func decodeAddressees(_ ann: cc_annotations, count: Int,
                                         encoding: WireEncoding) -> [String] {
        var result: [String] = []
        withUnsafePointer(to: ann.addressees) { arrPtr in
            arrPtr.withMemoryRebound(to: CChar.self, capacity: 5 * 64) { flat in
                for i in 0..<min(count, 5) {
                    let rowPtr = flat + (i * 64)
                    result.append(WireCodec.decode(rowPtr, encoding: encoding))
                }
            }
        }
        return result
    }

    /// Build the raw C struct for an outbound call (encoding addressees back
    /// to wire bytes via `WireCodec`). `addressees` beyond `CC_MAX_ADDRESSEES`
    /// (5) are silently dropped, matching the original's clip-at-5 (see
    /// state-and-codec.md §2.1).
    func toCAnnotations(encoding: WireEncoding) -> cc_annotations {
        var ann = cc_annotations()
        ann.gesture_pose = gesturePose
        ann.gesture_emotion = gestureEmotion
        ann.gesture_intensity = gestureIntensity
        ann.face_pose = facePose
        ann.face_emotion = faceEmotion
        ann.face_intensity = faceIntensity
        ann.requested = requested ? 1 : 0
        ann.mode = mode
        ann.cooked = cooked ? 1 : 0
        let clipped = Array(addressees.prefix(5))
        ann.addressee_count = Int32(clipped.count)
        withUnsafeMutablePointer(to: &ann.addressees) { arrPtr in
            arrPtr.withMemoryRebound(to: CChar.self, capacity: 5 * 64) { flat in
                for i in 0..<clipped.count {
                    var bytes = WireCodec.encode(clipped[i], encoding: encoding)
                    if bytes.count > 63 { bytes = Array(bytes.prefix(63)) }
                    let rowPtr = flat + (i * 64)
                    for (j, b) in bytes.enumerated() {
                        rowPtr[j] = CChar(bitPattern: b)
                    }
                    rowPtr[bytes.count] = 0
                }
            }
        }
        return ann
    }
}

/// The message "kind" carried by `CC_EV_TEXT` (mirrors the engine's `MT_*`
/// constants — say/whisper/think/action/shout as classified by the payload
/// stage). Kept as a raw Int32 passthrough since the engine is the source of
/// truth for the exact MT_* values; Swift doesn't need to interpret them to
/// route the event, only to display it.
public typealias MessageKind = Int32

/// Swift mirror of `cc_proto_event_type` / `cc_proto_event` (comicchat.h).
/// One case per non-NONE `cc_proto_event_type` value (34 total) — a strict
/// 1:1 with the C union, completeness-is-the-deliverable (matching Task 5a's
/// contract on the C side). All string fields have already been transcoded
/// from wire bytes via `WireCodec` by the time they reach this type.
public enum ProtocolEvent: Sendable {
    // connection lifecycle
    case loggedIn(nick: String)
    case serverCaps(ircx: Bool, maxMsgLen: Int32)
    case disconnectedHint(text: String)

    // membership
    case selfJoined(channel: String)
    case selfParted(channel: String)
    case userJoined(nick: String, ident: String)
    case userParted(nick: String, reason: String)
    case userQuit(nick: String, reason: String)
    case kicked(kicker: String, kickee: String, reason: String, channel: String)
    case invited(by: String, ident: String, channel: String)
    case names(channel: String, nicks: [String])
    case endOfNames(channel: String)
    case nickChanged(oldNick: String, newNick: String, isSelf: Bool)

    // messages (the core comic events)
    case text(nick: String, ident: String, target: String, text: String,
              kind: MessageKind, annotations: Annotations?)
    case data(nick: String, annotations: Annotations)
    case whisper(nick: String, ident: String, text: String, annotations: Annotations?)
    case action(nick: String, text: String, annotations: Annotations?)
    case sound(nick: String, file: String, text: String)
    case awayPeer(nick: String, message: String)
    case appearsAs(nick: String, avatarName: String, url: String)

    // room state
    case topicChanged(channel: String, topic: String)
    case channelMode(channel: String, modes: String, arg: String)
    case userMode(nick: String, modes: String)
    case roomProp(key: String, value: String)
    case roomListBegin(truncated: Bool)
    case roomListItem(name: String, users: Int32, topic: String)
    case roomListEnd(truncated: Bool)
    case whoisResult(nick: String, user: String, host: String, real: String, purpose: Int32)
    case whoResult(nick: String, user: String, host: String, channel: String, purpose: Int32)
    case motd(luser: String, motd: String)

    // errors & prompts (Swift owns retry/prompt)
    case error(code: Int32, text: String)
    case nickRejected(kind: Int32, badNick: String)
    case authUnsupported
    case statusLine(text: String)
}

/// A `ProtocolEvent` paired with the CHANNEL it is scoped to (Plan 4b Task 7:
/// true multi-room). `ProtocolSession.events` yields these so a consumer that
/// has joined N rooms on one connection can route each event to the right
/// per-room transcript.
///
/// `channel` is `nil` for SESSION-scoped events — anything the engine emitted
/// with `room_token == CC_ROOM_TOKEN_NONE` (0): connection lifecycle
/// (`.loggedIn`/`.serverCaps`), server-wide notices (`.statusLine`/`.error`/
/// `.motd`/room-list items), a plain-IRC private whisper (`PRIVMSG <ourNick>`,
/// which has no channel target), quits (`.userQuit` is server-wide — the peer
/// left every room at once), and IRCX out-of-band `DATA` lines. The channel is
/// resolved ON the engine queue at emit time via `cc_session_room_channel`
/// (the token→channel table the session already maintains) — see
/// `ProtocolSession.emit`.
///
/// Note the token→channel resolution is authoritative for ROUTING; several
/// event payloads ALSO carry a `channel` field of their own (`.selfJoined`,
/// `.names`, `.topicChanged`, …) for display — those two always agree for a
/// registered room, and `channel` here is derived from the same parsed channel
/// the token was resolved from (`ccSessionRoomTokenForChannel`, ircsock.cpp).
public struct ScopedEvent: Sendable {
    public let event: ProtocolEvent
    /// The channel this event is scoped to, or `nil` for session-scoped events
    /// (token 0). See the type doc comment for the full session-scoped list.
    public let channel: String?

    public init(event: ProtocolEvent, channel: String?) {
        self.event = event
        self.channel = channel
    }
}

extension ProtocolEvent {
    /// Build a `ProtocolEvent` from the raw C event, decoding every string
    /// field per `encoding`. Returns `nil` for `CC_EV_NONE` (not a real
    /// event — the placeholder `cc_session_test_echo` emits one). The
    /// `room_token` (0 if the event isn't room-scoped) is returned alongside
    /// the payload since it sits outside the tagged union in the C struct and
    /// an enum can't carry an extra stored field.
    static func from(_ ev: cc_proto_event, encoding: WireEncoding) -> (event: ProtocolEvent, roomToken: UInt32)? {
        func str(_ p: UnsafePointer<CChar>?) -> String { WireCodec.decode(p, encoding: encoding) }
        func ann(_ a: cc_annotations) -> Annotations { Annotations(cAnnotations: a, encoding: encoding) }

        var result: ProtocolEvent?
        switch ev.type {
        case CC_EV_NONE:
            return nil
        case CC_EV_LOGGED_IN:
            result = .loggedIn(nick: str(ev.u.logged_in.nick))
        case CC_EV_SERVER_CAPS:
            result = .serverCaps(ircx: ev.u.server_caps.ircx != 0,
                                 maxMsgLen: ev.u.server_caps.max_msg_len)
        case CC_EV_DISCONNECTED_HINT:
            result = .disconnectedHint(text: str(ev.u.disconnected_hint.text))
        case CC_EV_SELF_JOINED:
            result = .selfJoined(channel: str(ev.u.self_joined.channel))
        case CC_EV_SELF_PARTED:
            result = .selfParted(channel: str(ev.u.self_parted.channel))
        case CC_EV_USER_JOINED:
            result = .userJoined(nick: str(ev.u.user_joined.nick), ident: str(ev.u.user_joined.ident))
        case CC_EV_USER_PARTED:
            result = .userParted(nick: str(ev.u.user_parted.nick), reason: str(ev.u.user_parted.reason))
        case CC_EV_USER_QUIT:
            result = .userQuit(nick: str(ev.u.user_quit.nick), reason: str(ev.u.user_quit.reason))
        case CC_EV_KICKED:
            result = .kicked(kicker: str(ev.u.kicked.kicker), kickee: str(ev.u.kicked.kickee),
                             reason: str(ev.u.kicked.reason), channel: str(ev.u.kicked.channel))
        case CC_EV_INVITED:
            result = .invited(by: str(ev.u.invited.by), ident: str(ev.u.invited.ident),
                              channel: str(ev.u.invited.channel))
        case CC_EV_NAMES:
            let nicksStr = str(ev.u.names.nicks)
            let nicks = nicksStr.split(separator: " ").map(String.init)
            result = .names(channel: str(ev.u.names.channel), nicks: nicks)
        case CC_EV_END_OF_NAMES:
            result = .endOfNames(channel: str(ev.u.end_of_names.channel))
        case CC_EV_NICK_CHANGED:
            result = .nickChanged(oldNick: str(ev.u.nick_changed.old_nick),
                                  newNick: str(ev.u.nick_changed.new_nick),
                                  isSelf: ev.u.nick_changed.is_self != 0)
        case CC_EV_TEXT:
            let hasAnn = ev.u.text.has_annotations != 0
            result = .text(nick: str(ev.u.text.nick), ident: str(ev.u.text.ident),
                          target: str(ev.u.text.target), text: str(ev.u.text.text),
                          kind: ev.u.text.kind,
                          annotations: hasAnn ? ann(ev.u.text.annotations) : nil)
        case CC_EV_DATA:
            result = .data(nick: str(ev.u.data.nick), annotations: ann(ev.u.data.annotations))
        case CC_EV_WHISPER:
            let hasAnn = ev.u.whisper.has_annotations != 0
            result = .whisper(nick: str(ev.u.whisper.nick), ident: str(ev.u.whisper.ident),
                             text: str(ev.u.whisper.text),
                             annotations: hasAnn ? ann(ev.u.whisper.annotations) : nil)
        case CC_EV_ACTION:
            let hasAnn = ev.u.action.has_annotations != 0
            result = .action(nick: str(ev.u.action.nick), text: str(ev.u.action.text),
                            annotations: hasAnn ? ann(ev.u.action.annotations) : nil)
        case CC_EV_SOUND:
            result = .sound(nick: str(ev.u.sound.nick), file: str(ev.u.sound.file),
                           text: str(ev.u.sound.text))
        case CC_EV_AWAY_PEER:
            result = .awayPeer(nick: str(ev.u.away_peer.nick), message: str(ev.u.away_peer.message))
        case CC_EV_APPEARS_AS:
            result = .appearsAs(nick: str(ev.u.appears_as.nick),
                               avatarName: str(ev.u.appears_as.avatar_name),
                               url: str(ev.u.appears_as.url))
        case CC_EV_TOPIC_CHANGED:
            result = .topicChanged(channel: str(ev.u.topic_changed.channel),
                                  topic: str(ev.u.topic_changed.topic))
        case CC_EV_CHANNEL_MODE:
            result = .channelMode(channel: str(ev.u.channel_mode.channel),
                                 modes: str(ev.u.channel_mode.modes),
                                 arg: str(ev.u.channel_mode.arg))
        case CC_EV_USER_MODE:
            result = .userMode(nick: str(ev.u.user_mode.nick), modes: str(ev.u.user_mode.modes))
        case CC_EV_ROOM_PROP:
            result = .roomProp(key: str(ev.u.room_prop.key), value: str(ev.u.room_prop.value))
        case CC_EV_ROOM_LIST_BEGIN:
            result = .roomListBegin(truncated: ev.u.room_list_begin.truncated != 0)
        case CC_EV_ROOM_LIST_ITEM:
            result = .roomListItem(name: str(ev.u.room_list_item.name),
                                  users: ev.u.room_list_item.users,
                                  topic: str(ev.u.room_list_item.topic))
        case CC_EV_ROOM_LIST_END:
            result = .roomListEnd(truncated: ev.u.room_list_end.truncated != 0)
        case CC_EV_WHOIS_RESULT:
            result = .whoisResult(nick: str(ev.u.whois_result.nick), user: str(ev.u.whois_result.user),
                                 host: str(ev.u.whois_result.host), real: str(ev.u.whois_result.real),
                                 purpose: ev.u.whois_result.purpose)
        case CC_EV_WHO_RESULT:
            result = .whoResult(nick: str(ev.u.who_result.nick), user: str(ev.u.who_result.user),
                               host: str(ev.u.who_result.host), channel: str(ev.u.who_result.channel),
                               purpose: ev.u.who_result.purpose)
        case CC_EV_MOTD:
            result = .motd(luser: str(ev.u.motd.luser), motd: str(ev.u.motd.motd))
        case CC_EV_ERROR:
            result = .error(code: ev.u.error.code, text: str(ev.u.error.text))
        case CC_EV_NICK_REJECTED:
            result = .nickRejected(kind: ev.u.nick_rejected.kind, badNick: str(ev.u.nick_rejected.bad_nick))
        case CC_EV_AUTH_UNSUPPORTED:
            result = .authUnsupported
        case CC_EV_STATUS_LINE:
            result = .statusLine(text: str(ev.u.status_line.text))
        default:
            return nil
        }
        guard let result else { return nil }
        return (result, ev.room_token)
    }
}
