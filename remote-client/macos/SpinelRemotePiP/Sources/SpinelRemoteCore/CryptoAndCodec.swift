import CryptoKit
import Foundation
import zlib

public enum SandboxMsgType: UInt8, Sendable {
    case frameTile = 1
    case mouseMove = 2
    case mouseBtn = 3
    case mouseAxis = 4
    case keyEvent = 5
    case setMode = 6
    case clipText = 7
    case launchApp = 8
    case keyframeReq = 9
}

public enum SpinelCrypto {
    public static func hexEncode(_ data: Data) -> String {
        data.map { String(format: "%02x", $0) }.joined()
    }

    public static func hexDecode(_ hex: String) -> Data? {
        let clean = hex.trimmingCharacters(in: .whitespacesAndNewlines)
        guard clean.count % 2 == 0 else { return nil }
        var out = Data(capacity: clean.count / 2)
        var idx = clean.startIndex
        while idx < clean.endIndex {
            let next = clean.index(idx, offsetBy: 2)
            guard let byte = UInt8(clean[idx..<next], radix: 16) else { return nil }
            out.append(byte)
            idx = next
        }
        return out
    }

    public static func deriveSessionKey(myPriv: Curve25519.KeyAgreement.PrivateKey, peerPubHex: String, sessionID: String) -> SymmetricKey? {
        guard let peerRaw = hexDecode(peerPubHex),
              let peerPub = try? Curve25519.KeyAgreement.PublicKey(rawRepresentation: peerRaw),
              let shared = try? myPriv.sharedSecretFromKeyAgreement(with: peerPub) else {
            return nil
        }
        let sharedData = shared.withUnsafeBytes { Data($0) }
        var hasher = SHA256()
        hasher.update(data: Data("spinel-e2ee-v1|".utf8))
        hasher.update(data: sharedData)
        hasher.update(data: Data(sessionID.utf8))
        let digest = hasher.finalize()
        return SymmetricKey(data: Data(digest))
    }

    public static func makeNonce(seq: UInt64, dirBit: UInt8) -> ChaChaPoly.Nonce? {
        var bytes = [UInt8](repeating: 0, count: 12)
        bytes[0] = dirBit
        var be = seq.bigEndian
        withUnsafeBytes(of: &be) { raw in
            for i in 0..<8 {
                bytes[4 + i] = raw[i]
            }
        }
        return try? ChaChaPoly.Nonce(data: bytes)
    }

    public static func aeadEncrypt(key: SymmetricKey, seq: UInt64, dirBit: UInt8, plaintext: Data) -> Data? {
        guard let nonce = makeNonce(seq: seq, dirBit: dirBit),
              let box = try? ChaChaPoly.seal(plaintext, using: key, nonce: nonce) else {
            return nil
        }
        var combined = Data(capacity: box.ciphertext.count + box.tag.count)
        combined.append(box.ciphertext)
        combined.append(box.tag)
        return combined
    }

    public static func aeadDecrypt(key: SymmetricKey, seq: UInt64, dirBit: UInt8, combined: Data) -> Data? {
        guard combined.count >= 16,
              let nonce = makeNonce(seq: seq, dirBit: dirBit) else {
            return nil
        }
        let split = combined.count - 16
        let ct = Data(combined.prefix(split))
        let tag = Data(combined.suffix(16))
        guard let box = try? ChaChaPoly.SealedBox(nonce: nonce, ciphertext: ct, tag: tag) else {
            return nil
        }
        return try? ChaChaPoly.open(box, using: key)
    }

    public static func signEd25519(privKey: Curve25519.Signing.PrivateKey, message: String) -> String? {
        guard let sig = try? privKey.signature(for: Data(message.utf8)) else { return nil }
        return hexEncode(sig)
    }

    public static func verifyEd25519(pubHex: String, message: String, sigHex: String) -> Bool {
        guard let pubRaw = hexDecode(pubHex),
              let sigRaw = hexDecode(sigHex),
              let pubKey = try? Curve25519.Signing.PublicKey(rawRepresentation: pubRaw) else {
            return false
        }
        return pubKey.isValidSignature(sigRaw, for: Data(message.utf8))
    }
}

public final class TileDecoder {
    public private(set) var width: Int = 0
    public private(set) var height: Int = 0
    public private(set) var bgra = Data()

    public init() {}

    public func reset() {
        width = 0
        height = 0
        bgra.removeAll(keepingCapacity: false)
    }

    public func decodeTileFrame(_ packet: Data) -> Bool {
        guard packet.count >= 13 else { return false }
        let bytes = [UInt8](packet)
        let w = Int((UInt16(bytes[0]) << 8) | UInt16(bytes[1]))
        let h = Int((UInt16(bytes[2]) << 8) | UInt16(bytes[3]))
        let flags = bytes[4]
        let ts = Int((UInt16(bytes[5]) << 8) | UInt16(bytes[6]))
        let count = Int((UInt16(bytes[7]) << 8) | UInt16(bytes[8]))
        let rawLen = (UInt32(bytes[9]) << 24) |
                     (UInt32(bytes[10]) << 16) |
                     (UInt32(bytes[11]) << 8) |
                     UInt32(bytes[12])

        guard w > 0, h > 0, w <= 4096, h <= 4096, ts > 0,
              rawLen > 0, rawLen <= 64 * 1024 * 1024 else {
            return false
        }

        var raw = [UInt8](repeating: 0, count: Int(rawLen))
        var dstLen = uLongf(rawLen)
        let zrc = bytes.withUnsafeBufferPointer { srcBuf -> Int32 in
            guard let base = srcBuf.baseAddress else { return Z_DATA_ERROR }
            return raw.withUnsafeMutableBufferPointer { dstBuf in
                uncompress(dstBuf.baseAddress, &dstLen, base + 13, uLong(bytes.count - 13))
            }
        }
        guard zrc == Z_OK, Int(dstLen) == Int(rawLen) else { return false }

        let expectedSize = w * h * 4
        if (flags & 1) != 0 || width != w || height != h || bgra.count != expectedSize {
            width = w
            height = h
            bgra = Data(repeating: 0, count: expectedSize)
        }

        let cols = (w + ts - 1) / ts
        let rows = (h + ts - 1) / ts
        var roff = 0
        let ok = bgra.withUnsafeMutableBytes { framePtr -> Bool in
            guard let frameBase = framePtr.bindMemory(to: UInt8.self).baseAddress else { return false }
            for _ in 0..<count {
                guard roff + 4 <= raw.count else { return false }
                let tx = Int((UInt16(raw[roff]) << 8) | UInt16(raw[roff + 1]))
                let ty = Int((UInt16(raw[roff + 2]) << 8) | UInt16(raw[roff + 3]))
                roff += 4
                guard tx < cols, ty < rows else { return false }
                let x0 = tx * ts
                let y0 = ty * ts
                let tw = min(ts, w - x0)
                let th = min(ts, h - y0)
                let tileBytes = tw * th * 4
                guard roff + tileBytes <= raw.count else { return false }
                for r in 0..<th {
                    let dstOffset = ((y0 + r) * w + x0) * 4
                    let srcOffset = roff + r * tw * 4
                    raw.withUnsafeBufferPointer { rbuf in
                        if let srcBase = rbuf.baseAddress {
                            memcpy(frameBase + dstOffset, srcBase + srcOffset, tw * 4)
                        }
                    }
                }
                roff += tileBytes
            }
            return true
        }
        return ok
    }
}
