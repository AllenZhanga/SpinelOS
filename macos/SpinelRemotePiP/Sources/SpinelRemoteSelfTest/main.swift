import CryptoKit
import Foundation
import SpinelRemoteCore
import zlib

func check(_ cond: @autoclosure () -> Bool, _ msg: String) {
    if !cond() {
        fputs("FAIL: \(msg)\n", stderr)
        exit(1)
    }
}

@main
enum SpinelRemoteSelfTest {
    static func main() {
        // 1. Ed25519 sign/verify
        let priv = Curve25519.Signing.PrivateKey()
        let pubHex = SpinelCrypto.hexEncode(priv.publicKey.rawRepresentation)
        guard let sig = SpinelCrypto.signEd25519(privKey: priv, message: "sess01|ephpub") else {
            fputs("FAIL: signEd25519\n", stderr)
            exit(1)
        }
        check(SpinelCrypto.verifyEd25519(pubHex: pubHex, message: "sess01|ephpub", sigHex: sig), "ed25519 valid")
        check(!SpinelCrypto.verifyEd25519(pubHex: pubHex, message: "sess01|tampered", sigHex: sig), "ed25519 reject tamper")

        // 2. X25519 + SHA256 KDF + ChaCha20-Poly1305 AEAD
        let alice = Curve25519.KeyAgreement.PrivateKey()
        let bob = Curve25519.KeyAgreement.PrivateKey()
        let aliceHex = SpinelCrypto.hexEncode(alice.publicKey.rawRepresentation)
        let bobHex = SpinelCrypto.hexEncode(bob.publicKey.rawRepresentation)
        guard let k1 = SpinelCrypto.deriveSessionKey(myPriv: alice, peerPubHex: bobHex, sessionID: "sid123"),
              let k2 = SpinelCrypto.deriveSessionKey(myPriv: bob, peerPubHex: aliceHex, sessionID: "sid123") else {
            fputs("FAIL: deriveSessionKey\n", stderr)
            exit(1)
        }
        check(k1.withUnsafeBytes { Data($0) } == k2.withUnsafeBytes { Data($0) }, "shared session keys match")

        let plain = Data([1, 2, 3, 4, 5, 6, 7])
        guard let ct = SpinelCrypto.aeadEncrypt(key: k1, seq: 9, dirBit: 1, plaintext: plain),
              let pt = SpinelCrypto.aeadDecrypt(key: k2, seq: 9, dirBit: 1, combined: ct) else {
            fputs("FAIL: AEAD encrypt/decrypt\n", stderr)
            exit(1)
        }
        check(pt == plain, "AEAD roundtrip")
        var bad = Data(ct)
        bad[bad.startIndex] ^= 0xFF
        check(SpinelCrypto.aeadDecrypt(key: k2, seq: 9, dirBit: 1, combined: bad) == nil, "AEAD tamper reject")

        // 3. Tile frame decode roundtrip
        let w: UInt16 = 64
        let h: UInt16 = 64
        var rawTile = Data([0, 0, 0, 0])
        var pixels = Data(repeating: 0, count: 64 * 64 * 4)
        for i in stride(from: 0, to: pixels.count, by: 4) {
            pixels[i] = 0x11
            pixels[i + 1] = 0x22
            pixels[i + 2] = 0x33
            pixels[i + 3] = 0xFF
        }
        rawTile.append(pixels)
        let bound = Int(compressBound(uLong(rawTile.count)))
        var comp = [UInt8](repeating: 0, count: bound)
        var compLen = uLongf(bound)
        let zrc = rawTile.withUnsafeBytes { src in
            compress2(&comp, &compLen, src.bindMemory(to: UInt8.self).baseAddress, uLong(rawTile.count), 1)
        }
        check(zrc == Z_OK, "zlib compress2")
        var pkt = Data([UInt8(w >> 8), UInt8(w & 0xFF), UInt8(h >> 8), UInt8(h & 0xFF), 1, 0, 64, 0, 1])
        let rawLen = UInt32(rawTile.count).bigEndian
        withUnsafeBytes(of: rawLen) { pkt.append(contentsOf: $0) }
        pkt.append(contentsOf: comp.prefix(Int(compLen)))

        let dec = TileDecoder()
        check(dec.decodeTileFrame(pkt), "decodeTileFrame")
        check(dec.width == 64 && dec.height == 64 && dec.bgra == pixels, "decoded BGRA match")

        // 4. Host/port & macOS->Linux keycode map
        let v4 = splitHostPort("123.45.67.89:7443")
        check(v4?.host == "123.45.67.89" && v4?.port == 7443, "splitHostPort v4")
        let v6 = splitHostPort("[2001:db8::1]:7444")
        check(v6?.host == "2001:db8::1" && v6?.port == 7444, "splitHostPort v6")
        check(MacToLinuxKeymap.evdevCode(for: 0, mapCommandToCtrl: true) == 30, "key A")
        check(MacToLinuxKeymap.evdevCode(for: 55, mapCommandToCtrl: true) == 29, "Cmd->Ctrl")
        check(MacToLinuxKeymap.evdevCode(for: 55, mapCommandToCtrl: false) == 125, "Cmd->Meta")

        print("All SpinelRemoteCore self-tests passed.")
    }
}
