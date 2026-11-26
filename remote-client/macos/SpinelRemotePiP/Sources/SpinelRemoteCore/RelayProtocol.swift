import CryptoKit
import Darwin
import Foundation
import Network
import Security

public struct RelayConfig: Codable, Sendable {
    public var serverAddr: String = ""
    public var udpPort: Int = 7444
    public var serverFP: String = ""
    public var account: String = ""
    public var token: String = ""
    public var deviceID: String = ""
    public var deviceName: String = ""
    public var mapCommandToCtrl: Bool = true

    public init() {}

    public var isLoggedIn: Bool {
        !serverAddr.isEmpty && !account.isEmpty && !token.isEmpty
    }
}

public struct RelayPeerDevice: Codable, Identifiable, Sendable {
    public var id: String { deviceID }
    public let deviceID: String
    public let name: String
    public let pubKeyHex: String
    public let online: Bool
    public let allowRemote: Bool
    public let sandboxRunning: Bool
    public let lanAddrs: [String]
    public let ipv6Addrs: [String]
    public let udpMapped: String
    public let lastSeen: Int64

    enum CodingKeys: String, CodingKey {
        case deviceID = "device_id"
        case name
        case pubKeyHex = "pub_key_hex"
        case online
        case allowRemote = "allow_remote"
        case sandboxRunning = "sandbox_running"
        case lanAddrs = "lan_addrs"
        case ipv6Addrs = "ipv6_addrs"
        case udpMapped = "udp_mapped"
        case lastSeen = "last_seen"
    }

    public init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        deviceID = try c.decodeIfPresent(String.self, forKey: .deviceID) ?? ""
        name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
        pubKeyHex = try c.decodeIfPresent(String.self, forKey: .pubKeyHex) ?? ""
        online = try c.decodeIfPresent(Bool.self, forKey: .online) ?? false
        allowRemote = try c.decodeIfPresent(Bool.self, forKey: .allowRemote) ?? false
        sandboxRunning = try c.decodeIfPresent(Bool.self, forKey: .sandboxRunning) ?? false
        lanAddrs = try c.decodeIfPresent([String].self, forKey: .lanAddrs) ?? []
        ipv6Addrs = try c.decodeIfPresent([String].self, forKey: .ipv6Addrs) ?? []
        udpMapped = try c.decodeIfPresent(String.self, forKey: .udpMapped) ?? ""
        lastSeen = try c.decodeIfPresent(Int64.self, forKey: .lastSeen) ?? 0
    }
}

public enum RelayError: LocalizedError, Sendable {
    case message(String)

    public var errorDescription: String? {
        switch self {
        case .message(let text): return text
        }
    }
}

public enum RelayStore {
    public static func appSupportDir() -> URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent("Library/Application Support")
        let dir = base.appendingPathComponent("SpinelRemotePiP", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    public static func configURL() -> URL {
        appSupportDir().appendingPathComponent("config.json")
    }

    public static func keyURL() -> URL {
        appSupportDir().appendingPathComponent("ed25519.key")
    }

    public static func loadConfig() -> RelayConfig {
        var cfg = RelayConfig()
        if let data = try? Data(contentsOf: configURL()),
           let decoded = try? JSONDecoder().decode(RelayConfig.self, from: data) {
            cfg = decoded
        }
        if cfg.deviceID.isEmpty {
            var rnd = [UInt8](repeating: 0, count: 6)
            _ = SecRandomCopyBytes(kSecRandomDefault, rnd.count, &rnd)
            cfg.deviceID = "mac-" + SpinelCrypto.hexEncode(Data(rnd))
        }
        if cfg.deviceName.isEmpty {
            cfg.deviceName = Host.current().localizedName ?? "MacBook"
        }
        if cfg.udpPort <= 0 || cfg.udpPort > 65535 {
            cfg.udpPort = 7444
        }
        return cfg
    }

    public static func saveConfig(_ cfg: RelayConfig) {
        var norm = cfg
        if norm.deviceID.isEmpty { norm.deviceID = loadConfig().deviceID }
        if norm.deviceName.isEmpty { norm.deviceName = Host.current().localizedName ?? "MacBook" }
        if let data = try? JSONEncoder().encode(norm) {
            try? data.write(to: configURL(), options: [.atomic])
            chmod(configURL().path, 0o600)
        }
    }

    public static func ensureDeviceKey() throws -> (priv: Curve25519.Signing.PrivateKey, pubHex: String) {
        let url = keyURL()
        if let raw = try? Data(contentsOf: url), raw.count == 32,
           let priv = try? Curve25519.Signing.PrivateKey(rawRepresentation: raw) {
            return (priv, SpinelCrypto.hexEncode(priv.publicKey.rawRepresentation))
        }
        let priv = Curve25519.Signing.PrivateKey()
        try priv.rawRepresentation.write(to: url, options: [.atomic])
        chmod(url.path, 0o600)
        return (priv, SpinelCrypto.hexEncode(priv.publicKey.rawRepresentation))
    }
}

public func splitHostPort(_ raw: String, defaultPort: Int = 7443) -> (host: String, port: UInt16)? {
    let s = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !s.isEmpty else { return nil }
    if s.hasPrefix("["), let close = s.firstIndex(of: "]") {
        let host = String(s[s.index(after: s.startIndex)..<close])
        let rest = s[s.index(after: close)...]
        if rest.hasPrefix(":"), let p = UInt16(rest.dropFirst()), p > 0 {
            return (host, p)
        }
        return (host, UInt16(defaultPort))
    }
    if let firstColon = s.firstIndex(of: ":"), let lastColon = s.lastIndex(of: ":"), firstColon == lastColon {
        let host = String(s[..<firstColon])
        if let p = UInt16(s[s.index(after: firstColon)...]), p > 0, !host.isEmpty {
            return (host, p)
        }
        return nil
    }
    return (s, UInt16(defaultPort))
}

private final class LockedBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var storage: T

    init(_ value: T) {
        self.storage = value
    }

    func withLock<R>(_ body: (inout T) -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body(&storage)
    }

    var value: T {
        get { withLock { $0 } }
        set { withLock { $0 = newValue } }
    }
}

public final class TlsRelayConnection: @unchecked Sendable {
    private let conn: NWConnection
    private let ioQueue = DispatchQueue(label: "spinel.relay.tls.io")
    private let readLock = NSLock()
    private var readBuf = Data()
    private var readWaiters: [(id: UInt64, count: Int, cb: (Result<Data, Error>) -> Void)] = []
    private var nextWaiterID: UInt64 = 1
    private var fatalError: Error?

    public private(set) var negotiatedFingerprint: String = ""

    private init(conn: NWConnection) {
        self.conn = conn
    }

    public static func connect(serverAddr: String, pinnedFP: String) throws -> (TlsRelayConnection, String) {
        guard let (host, port) = splitHostPort(serverAddr, defaultPort: 7443),
              let nwPort = NWEndpoint.Port(rawValue: port) else {
            throw RelayError.message("服务器地址格式无效，请使用 IP:端口，例如 1.2.3.4:7443")
        }

        let fpBox = LockedBox<(fp: String, rejected: Bool)>((fp: "", rejected: false))

        let tlsOptions = NWProtocolTLS.Options()
        sec_protocol_options_set_min_tls_protocol_version(tlsOptions.securityProtocolOptions, .TLSv13)
        sec_protocol_options_set_verify_block(tlsOptions.securityProtocolOptions, { _, trust, complete in
            let secTrust = sec_trust_copy_ref(trust).takeRetainedValue()
            guard let chain = SecTrustCopyCertificateChain(secTrust) as? [SecCertificate],
                  let leaf = chain.first else {
                complete(false)
                return
            }
            let der = SecCertificateCopyData(leaf) as Data
            let fp = "sha256-" + SpinelCrypto.hexEncode(Data(SHA256.hash(data: der)))
            let ok = fpBox.withLock { state in
                state.fp = fp
                let expected = pinnedFP.trimmingCharacters(in: .whitespacesAndNewlines)
                let match = expected.isEmpty || expected == fp
                if !match { state.rejected = true }
                return match
            }
            complete(ok)
        }, DispatchQueue.global(qos: .userInitiated))

        let tcpOptions = NWProtocolTCP.Options()
        tcpOptions.noDelay = true
        let params = NWParameters(tls: tlsOptions, tcp: tcpOptions)
        let nwConn = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: params)
        let wrapper = TlsRelayConnection(conn: nwConn)

        let readySem = DispatchSemaphore(value: 0)
        let readyState = LockedBox<(done: Bool, err: Error?)>((done: false, err: nil))

        nwConn.stateUpdateHandler = { state in
            switch state {
            case .ready:
                let shouldSignal = readyState.withLock { st -> Bool in
                    guard !st.done else { return false }
                    st.done = true
                    return true
                }
                if shouldSignal { readySem.signal() }
            case .failed(let err):
                let shouldSignal = readyState.withLock { st -> Bool in
                    guard !st.done else { return false }
                    st.done = true
                    st.err = err
                    return true
                }
                if shouldSignal { readySem.signal() }
                wrapper.failAll(err)
            case .cancelled:
                wrapper.failAll(RelayError.message("连接已关闭"))
            default:
                break
            }
        }
        nwConn.start(queue: wrapper.ioQueue)

        if readySem.wait(timeout: .now() + 8) == .timedOut {
            nwConn.cancel()
            throw RelayError.message("连接云服务器超时（\(host):\(port)），请检查安全组 7443 端口")
        }

        let (fp, rejected) = fpBox.value
        let readyErr = readyState.value.err

        if rejected {
            nwConn.cancel()
            throw RelayError.message("服务器 TLS 指纹不匹配，连接被拒绝（可在面板清除指纹后重试）")
        }
        if let readyErr {
            nwConn.cancel()
            throw RelayError.message("TLS 连接失败: \(readyErr.localizedDescription)")
        }

        wrapper.negotiatedFingerprint = fp
        wrapper.startReceiveLoop()
        return (wrapper, fp)
    }

    public func close() {
        conn.cancel()
        failAll(RelayError.message("连接已断开"))
    }

    private func startReceiveLoop() {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data, !data.isEmpty {
                self.appendData(data)
            }
            if let error {
                self.failAll(error)
                return
            }
            if isComplete {
                self.failAll(RelayError.message("对端已断开连接"))
                return
            }
            self.startReceiveLoop()
        }
    }

    private func appendData(_ chunk: Data) {
        var satisfied: [((Result<Data, Error>) -> Void, Data)] = []
        readLock.lock()
        readBuf.append(chunk)
        while let first = readWaiters.first, readBuf.count >= first.count {
            let need = first.count
            let cb = first.cb
            readWaiters.removeFirst()
            let slice = Data(readBuf.prefix(need))
            readBuf.removeFirst(need)
            satisfied.append((cb, slice))
        }
        readLock.unlock()
        for (cb, slice) in satisfied {
            cb(.success(slice))
        }
    }

    private func failAll(_ err: Error) {
        readLock.lock()
        if fatalError == nil { fatalError = err }
        let waiters = readWaiters
        readWaiters.removeAll()
        readLock.unlock()
        for waiter in waiters {
            waiter.cb(.failure(err))
        }
    }

    private func readExact(_ count: Int, timeout: TimeInterval) throws -> Data {
        if count == 0 { return Data() }
        let sem = DispatchSemaphore(value: 0)
        let outcome = LockedBox<Result<Data, Error>?>(nil)

        readLock.lock()
        if let fatalError {
            readLock.unlock()
            throw fatalError
        }
        if readWaiters.isEmpty && readBuf.count >= count {
            let out = Data(readBuf.prefix(count))
            readBuf.removeFirst(count)
            readLock.unlock()
            return out
        }
        let wid = nextWaiterID
        nextWaiterID += 1
        readWaiters.append((wid, count, { res in
            outcome.value = res
            sem.signal()
        }))
        readLock.unlock()

        if sem.wait(timeout: .now() + timeout) == .timedOut {
            readLock.lock()
            readWaiters.removeAll { $0.id == wid }
            readLock.unlock()
            throw RelayError.message("等待服务端数据超时")
        }
        switch outcome.value {
        case .success(let d): return d
        case .failure(let e): throw e
        case .none: throw RelayError.message("读取数据被中断")
        }
    }

    public func sendFrame(type: UInt8, payload: Data) throws {
        guard payload.count <= 8 * 1024 * 1024 else {
            throw RelayError.message("数据包超出 8MB 上限")
        }
        var packet = Data(capacity: 5 + payload.count)
        packet.append(type)
        var lenBE = UInt32(payload.count).bigEndian
        withUnsafeBytes(of: &lenBE) { packet.append(contentsOf: $0) }
        packet.append(payload)

        let sem = DispatchSemaphore(value: 0)
        let sendErrBox = LockedBox<NWError?>(nil)
        conn.send(content: packet, completion: .contentProcessed { err in
            sendErrBox.value = err
            sem.signal()
        })
        if sem.wait(timeout: .now() + 8) == .timedOut {
            throw RelayError.message("发送控制帧超时")
        }
        if let sendErr = sendErrBox.value {
            throw RelayError.message("网络发送失败: \(sendErr.localizedDescription)")
        }
    }

    public func recvFrame(timeout: TimeInterval = 8.0) throws -> (type: UInt8, payload: Data) {
        let hdr = try readExact(5, timeout: timeout)
        let ftype = hdr[hdr.startIndex]
        let length = (UInt32(hdr[hdr.startIndex + 1]) << 24) |
                     (UInt32(hdr[hdr.startIndex + 2]) << 16) |
                     (UInt32(hdr[hdr.startIndex + 3]) << 8) |
                     UInt32(hdr[hdr.startIndex + 4])
        guard length <= 8 * 1024 * 1024 else {
            throw RelayError.message("收到异常超长帧: \(length)")
        }
        let body = length > 0 ? try readExact(Int(length), timeout: timeout) : Data()
        return (ftype, body)
    }

    public func sendControlJSON(_ dict: [String: Any]) throws {
        let data = try JSONSerialization.data(withJSONObject: dict)
        try sendFrame(type: 0x01, payload: data)
    }

    public func rpc(_ dict: [String: Any]) throws -> [String: Any] {
        let reqID = (dict["req_id"] as? String) ?? ""
        try sendControlJSON(dict)
        let deadline = Date().addingTimeInterval(8.0)
        while Date() < deadline {
            let remaining = max(0.2, deadline.timeIntervalSinceNow)
            let frame = try recvFrame(timeout: remaining)
            guard frame.type == 0x01,
                  let obj = try? JSONSerialization.jsonObject(with: frame.payload) as? [String: Any] else {
                continue
            }
            if (obj["type"] as? String) == "resp" {
                let rid = obj["req_id"] as? String ?? ""
                if reqID.isEmpty || rid == reqID {
                    if (obj["ok"] as? Bool) == true {
                        return obj
                    }
                    let msg = (obj["error"] as? String) ?? "服务端请求失败"
                    throw RelayError.message(msg)
                }
            }
        }
        throw RelayError.message("等待服务端 RPC 响应超时")
    }
}

public enum NetworkCandidates {
    public static func collectLocal() -> (lanV4: [String], globalV6: [String]) {
        var lan: [String] = []
        var v6: [String] = []
        var ifap: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&ifap) == 0, let first = ifap else { return ([], []) }
        defer { freeifaddrs(first) }

        var ptr: UnsafeMutablePointer<ifaddrs>? = first
        while let ifa = ptr {
            defer { ptr = ifa.pointee.ifa_next }
            let flags = Int32(ifa.pointee.ifa_flags)
            guard let addr = ifa.pointee.ifa_addr,
                  (flags & IFF_UP) != 0,
                  (flags & IFF_LOOPBACK) == 0 else {
                continue
            }
            let family = Int32(addr.pointee.sa_family)
            if family == AF_INET {
                var sin = sockaddr_in()
                memcpy(&sin, addr, MemoryLayout<sockaddr_in>.size)
                var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                if inet_ntop(AF_INET, &sin.sin_addr, &buf, socklen_t(INET_ADDRSTRLEN)) != nil {
                    let ip = String(decoding: buf.prefix(while: { $0 != 0 }).map(UInt8.init(bitPattern:)), as: UTF8.self)
                    let host = UInt32(bigEndian: sin.sin_addr.s_addr)
                    let isPriv = ((host >> 24) == 10) ||
                                 ((host >> 20) == ((172 << 4) | 1)) ||
                                 ((host >> 16) == ((192 << 8) | 168))
                    if isPriv && !lan.contains(ip) {
                        lan.append(ip)
                    }
                }
            } else if family == AF_INET6 {
                var sin6 = sockaddr_in6()
                memcpy(&sin6, addr, MemoryLayout<sockaddr_in6>.size)
                let firstByte = sin6.sin6_addr.__u6_addr.__u6_addr8.0
                if (firstByte & 0xE0) == 0x20 {
                    var buf = [CChar](repeating: 0, count: Int(INET6_ADDRSTRLEN))
                    if inet_ntop(AF_INET6, &sin6.sin6_addr, &buf, socklen_t(INET6_ADDRSTRLEN)) != nil {
                        let ip6 = String(decoding: buf.prefix(while: { $0 != 0 }).map(UInt8.init(bitPattern:)), as: UTF8.self)
                        if !v6.contains(ip6) {
                            v6.append(ip6)
                        }
                    }
                }
            }
        }
        return (lan, v6)
    }

    public static func createDualStackUDPSocket() -> (fd: Int32, port: UInt16) {
        func tune(_ fd: Int32) {
            var sz: Int32 = 4 * 1024 * 1024
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, socklen_t(MemoryLayout<Int32>.size))
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, socklen_t(MemoryLayout<Int32>.size))
        }
        var fd = Darwin.socket(AF_INET6, SOCK_DGRAM, 0)
        if fd >= 0 {
            var off: Int32 = 0
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, socklen_t(MemoryLayout<Int32>.size))
            tune(fd)
            var sa = sockaddr_in6()
            sa.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            sa.sin6_family = sa_family_t(AF_INET6)
            sa.sin6_addr = in6addr_any
            sa.sin6_port = 0
            let bound = withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in6>.size))
                }
            }
            if bound == 0 {
                var slen = socklen_t(MemoryLayout<sockaddr_in6>.size)
                let got = withUnsafeMutablePointer(to: &sa) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        Darwin.getsockname(fd, $0, &slen)
                    }
                }
                if got == 0 {
                    return (fd, UInt16(bigEndian: sa.sin6_port))
                }
            }
            Darwin.close(fd)
        }

        fd = Darwin.socket(AF_INET, SOCK_DGRAM, 0)
        if fd >= 0 {
            tune(fd)
            var sa = sockaddr_in()
            sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
            sa.sin_family = sa_family_t(AF_INET)
            sa.sin_addr = in_addr(s_addr: INADDR_ANY)
            sa.sin_port = 0
            let bound = withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    Darwin.bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
            if bound == 0 {
                var slen = socklen_t(MemoryLayout<sockaddr_in>.size)
                let got = withUnsafeMutablePointer(to: &sa) {
                    $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                        Darwin.getsockname(fd, $0, &slen)
                    }
                }
                if got == 0 {
                    return (fd, UInt16(bigEndian: sa.sin_port))
                }
            }
            Darwin.close(fd)
        }
        return (-1, 0)
    }

    public static func sockaddrFromHostPort(_ hp: String) -> sockaddr_storage? {
        guard let (host, port) = splitHostPort(hp, defaultPort: 0), port > 0 else { return nil }
        var ss = sockaddr_storage()
        var in4 = in_addr()
        if inet_pton(AF_INET, host, &in4) == 1 {
            var sa6 = sockaddr_in6()
            sa6.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            sa6.sin6_family = sa_family_t(AF_INET6)
            sa6.sin6_port = port.bigEndian
            withUnsafeMutableBytes(of: &sa6.sin6_addr) { raw in
                for i in 0..<10 { raw[i] = 0 }
                raw[10] = 0xFF
                raw[11] = 0xFF
                withUnsafeBytes(of: in4) { v4 in
                    for i in 0..<4 { raw[12 + i] = v4[i] }
                }
            }
            memcpy(&ss, &sa6, MemoryLayout<sockaddr_in6>.size)
            return ss
        }
        var in6 = in6_addr()
        if inet_pton(AF_INET6, host, &in6) == 1 {
            var sa6 = sockaddr_in6()
            sa6.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size)
            sa6.sin6_family = sa_family_t(AF_INET6)
            sa6.sin6_port = port.bigEndian
            sa6.sin6_addr = in6
            memcpy(&ss, &sa6, MemoryLayout<sockaddr_in6>.size)
            return ss
        }
        return nil
    }

    public static func stunProbe(udpFD: Int32, serverAddr: String, udpPort: Int) -> String {
        guard udpFD >= 0, let (host, _) = splitHostPort(serverAddr, defaultPort: 7443) else { return "" }
        var hints = addrinfo()
        hints.ai_family = AF_UNSPEC
        hints.ai_socktype = SOCK_DGRAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, String(udpPort), &hints, &res) == 0, let first = res else { return "" }
        defer { freeaddrinfo(first) }

        var pkt = Data("SPINELST".utf8)
        var nonce = [UInt8](repeating: 0, count: 4)
        _ = SecRandomCopyBytes(kSecRandomDefault, 4, &nonce)
        pkt.append(contentsOf: nonce)

        var ai: UnsafeMutablePointer<addrinfo>? = first
        while let cur = ai {
            _ = pkt.withUnsafeBytes { raw in
                Darwin.sendto(udpFD, raw.baseAddress, pkt.count, 0, cur.pointee.ai_addr, cur.pointee.ai_addrlen)
            }
            ai = cur.pointee.ai_next
        }

        var pfd = pollfd(fd: udpFD, events: Int16(POLLIN), revents: 0)
        guard Darwin.poll(&pfd, 1, 700) > 0 else { return "" }
        var buf = [UInt8](repeating: 0, count: 256)
        let n = Darwin.recvfrom(udpFD, &buf, buf.count, 0, nil, nil)
        guard n > 12,
              String(bytes: buf[0..<8], encoding: .utf8) == "SPINELOK",
              Array(buf[8..<12]) == nonce else {
            return ""
        }
        return String(bytes: buf[12..<n], encoding: .utf8) ?? ""
    }

    public static func buildCandidates(udpFD: Int32, localPort: UInt16, cfg: RelayConfig) -> [String] {
        let (lan, v6) = collectLocal()
        var cands: [String] = []
        for ip in lan { cands.append("\(ip):\(localPort)") }
        for ip6 in v6 { cands.append("[\(ip6)]:\(localPort)") }
        let mapped = stunProbe(udpFD: udpFD, serverAddr: cfg.serverAddr, udpPort: cfg.udpPort)
        if !mapped.isEmpty && !cands.contains(mapped) {
            cands.append(mapped)
        }
        return cands
    }

    public static func classifyAddrLabel(_ ss: sockaddr_storage) -> String {
        if Int32(ss.ss_family) == AF_INET6 {
            var sa6 = sockaddr_in6()
            var copy = ss
            memcpy(&sa6, &copy, MemoryLayout<sockaddr_in6>.size)
            let bytes = withUnsafeBytes(of: sa6.sin6_addr) { Array($0) }
            let isMapped = bytes[0..<10].allSatisfy { $0 == 0 } && bytes[10] == 0xFF && bytes[11] == 0xFF
            if isMapped {
                let ip = (UInt32(bytes[12]) << 24) | (UInt32(bytes[13]) << 16) | (UInt32(bytes[14]) << 8) | UInt32(bytes[15])
                let priv = ((ip >> 24) == 10) || ((ip >> 20) == ((172 << 4) | 1)) || ((ip >> 16) == ((192 << 8) | 168))
                return priv ? "局域网直连" : "UDP穿透"
            }
            return "IPv6直连"
        }
        return "UDP穿透"
    }
}

public enum RelayRPC {
    private static func metaPayload(type: String, reqID: String, cfg: RelayConfig, pubHex: String, extra: [String: Any]) -> [String: Any] {
        let (lan, v6) = NetworkCandidates.collectLocal()
        var out: [String: Any] = [
            "type": type,
            "req_id": reqID,
            "device_id": cfg.deviceID,
            "device_name": cfg.deviceName,
            "pub_key_hex": pubHex,
            "allow_remote": false,
            "ephemeral": true,
            "lan_addrs": lan,
            "ipv6_addrs": v6
        ]
        for (k, v) in extra { out[k] = v }
        return out
    }

    private static func decodeDevices(from resp: [String: Any]) -> [RelayPeerDevice] {
        guard let arr = resp["devices"],
              let data = try? JSONSerialization.data(withJSONObject: arr),
              let list = try? JSONDecoder().decode([RelayPeerDevice].self, from: data) else {
            return []
        }
        return list
    }

    public static func auth(isRegister: Bool, serverAddr: String, udpPort: Int, serverFP: String, account: String, password: String) throws -> (RelayConfig, [RelayPeerDevice]) {
        var cfg = RelayStore.loadConfig()
        cfg.serverAddr = serverAddr.trimmingCharacters(in: .whitespacesAndNewlines)
        cfg.udpPort = udpPort
        cfg.serverFP = serverFP.trimmingCharacters(in: .whitespacesAndNewlines)
        let (_, pubHex) = try RelayStore.ensureDeviceKey()

        let (conn, fp) = try TlsRelayConnection.connect(serverAddr: cfg.serverAddr, pinnedFP: cfg.serverFP)
        defer { conn.close() }
        if cfg.serverFP.isEmpty { cfg.serverFP = fp }

        let req = metaPayload(
            type: isRegister ? "register" : "login",
            reqID: "auth1",
            cfg: cfg,
            pubHex: pubHex,
            extra: [
                "account": account.trimmingCharacters(in: .whitespacesAndNewlines),
                "password": password
            ]
        )
        let resp = try conn.rpc(req)
        cfg.account = (resp["account"] as? String) ?? account.trimmingCharacters(in: .whitespacesAndNewlines)
        cfg.token = (resp["token"] as? String) ?? ""
        RelayStore.saveConfig(cfg)
        return (cfg, decodeDevices(from: resp))
    }

    public static func claimPairCode(serverAddr: String, udpPort: Int, serverFP: String, pairCode: String) throws -> (RelayConfig, [RelayPeerDevice]) {
        var cfg = RelayStore.loadConfig()
        cfg.serverAddr = serverAddr.trimmingCharacters(in: .whitespacesAndNewlines)
        cfg.udpPort = udpPort
        cfg.serverFP = serverFP.trimmingCharacters(in: .whitespacesAndNewlines)
        let (_, pubHex) = try RelayStore.ensureDeviceKey()

        let (conn, fp) = try TlsRelayConnection.connect(serverAddr: cfg.serverAddr, pinnedFP: cfg.serverFP)
        defer { conn.close() }
        if cfg.serverFP.isEmpty { cfg.serverFP = fp }

        let req = metaPayload(
            type: "pair_claim",
            reqID: "pc1",
            cfg: cfg,
            pubHex: pubHex,
            extra: ["pair_code": pairCode.trimmingCharacters(in: .whitespacesAndNewlines)]
        )
        let resp = try conn.rpc(req)
        cfg.account = (resp["account"] as? String) ?? ""
        cfg.token = (resp["token"] as? String) ?? ""
        RelayStore.saveConfig(cfg)
        return (cfg, decodeDevices(from: resp))
    }

    public static func fetchDevices() throws -> [RelayPeerDevice] {
        let cfg = RelayStore.loadConfig()
        guard cfg.isLoggedIn else { throw RelayError.message("尚未登录 Spinel ID") }
        let (_, pubHex) = try RelayStore.ensureDeviceKey()
        let (conn, _) = try TlsRelayConnection.connect(serverAddr: cfg.serverAddr, pinnedFP: cfg.serverFP)
        defer { conn.close() }
        let req = metaPayload(type: "login", reqID: "dl1", cfg: cfg, pubHex: pubHex, extra: [
            "account": cfg.account,
            "token": cfg.token
        ])
        let resp = try conn.rpc(req)
        return decodeDevices(from: resp)
    }

    public static func createPairCode() throws -> String {
        let cfg = RelayStore.loadConfig()
        guard cfg.isLoggedIn else { throw RelayError.message("尚未登录 Spinel ID") }
        let (_, pubHex) = try RelayStore.ensureDeviceKey()
        let (conn, _) = try TlsRelayConnection.connect(serverAddr: cfg.serverAddr, pinnedFP: cfg.serverFP)
        defer { conn.close() }
        let loginReq = metaPayload(type: "login", reqID: "p1", cfg: cfg, pubHex: pubHex, extra: [
            "account": cfg.account,
            "token": cfg.token
        ])
        _ = try conn.rpc(loginReq)
        let resp = try conn.rpc(["type": "pair_create", "req_id": "p2"])
        return (resp["pair_code"] as? String) ?? ""
    }
}

public final class E2eeTunnel: @unchecked Sendable {
    private static let udpMTUChunk = 1200
    private static let pktPunchPing: UInt8 = 0xFE
    private static let pktPunchPong: UInt8 = 0xFD

    private struct FragAccum {
        var total: UInt16
        var received: UInt16 = 0
        var parts: [Data?]
    }

    private let sid16: Data
    private let key: SymmetricKey
    private let sendLock = NSLock()
    private var sendSeq: UInt64 = 1

    private let stateLock = NSLock()
    private var closed = false
    private let useUDP: Bool
    private var udpFD: Int32
    private let peerAddr: sockaddr_storage
    private let relayConn: TlsRelayConnection?
    private var frags: [UInt64: FragAccum] = [:]

    public let transportLabel: String

    private init(sid16: Data, key: SymmetricKey, useUDP: Bool, udpFD: Int32, peerAddr: sockaddr_storage, relayConn: TlsRelayConnection?, transportLabel: String) {
        self.sid16 = sid16
        self.key = key
        self.useUDP = useUDP
        self.udpFD = udpFD
        self.peerAddr = peerAddr
        self.relayConn = relayConn
        self.transportLabel = transportLabel
    }

    deinit {
        close()
    }

    public var isOpen: Bool {
        stateLock.lock()
        let c = closed
        stateLock.unlock()
        return !c
    }

    public func close() {
        stateLock.lock()
        if closed {
            stateLock.unlock()
            return
        }
        closed = true
        let ufd = udpFD
        udpFD = -1
        let rconn = relayConn
        stateLock.unlock()

        if ufd >= 0 { Darwin.close(ufd) }
        rconn?.close()
    }

    public static func dialRemoteHost(targetDeviceID: String) throws -> E2eeTunnel {
        let cfg = RelayStore.loadConfig()
        guard cfg.isLoggedIn else {
            throw RelayError.message("请先在菜单栏登录 Spinel ID")
        }
        let (privKey, pubHex) = try RelayStore.ensureDeviceKey()
        let (ctrl, _) = try TlsRelayConnection.connect(serverAddr: cfg.serverAddr, pinnedFP: cfg.serverFP)
        defer { ctrl.close() }

        let (lan, v6) = NetworkCandidates.collectLocal()
        let loginReq: [String: Any] = [
            "type": "login",
            "req_id": "d1",
            "account": cfg.account,
            "token": cfg.token,
            "device_id": cfg.deviceID,
            "device_name": cfg.deviceName,
            "pub_key_hex": pubHex,
            "allow_remote": false,
            "ephemeral": true,
            "lan_addrs": lan,
            "ipv6_addrs": v6
        ]
        let loginResp = try ctrl.rpc(loginReq)
        guard let devObj = loginResp["devices"],
              let devData = try? JSONSerialization.data(withJSONObject: devObj),
              let peers = try? JSONDecoder().decode([RelayPeerDevice].self, from: devData),
              let peer = peers.first(where: { $0.deviceID == targetDeviceID }) else {
            throw RelayError.message("找不到目标设备 \(targetDeviceID)")
        }
        guard peer.online else { throw RelayError.message("目标设备当前不在线") }
        guard peer.allowRemote else { throw RelayError.message("目标设备未开启「允许远程接管平行沙箱」") }

        let (ufd, uport) = NetworkCandidates.createDualStackUDPSocket()
        let myCands = NetworkCandidates.buildCandidates(udpFD: ufd, localPort: uport, cfg: cfg)

        var sidRaw = [UInt8](repeating: 0, count: 16)
        _ = SecRandomCopyBytes(kSecRandomDefault, 16, &sidRaw)
        let sid16 = Data(sidRaw)
        let sidHex = SpinelCrypto.hexEncode(sid16)

        let ephPriv = Curve25519.KeyAgreement.PrivateKey()
        let ephPubHex = SpinelCrypto.hexEncode(ephPriv.publicKey.rawRepresentation)
        guard let sigHex = SpinelCrypto.signEd25519(privKey: privKey, message: "\(sidHex)|\(ephPubHex)") else {
            if ufd >= 0 { Darwin.close(ufd) }
            throw RelayError.message("生成会话签名失败")
        }

        try ctrl.sendControlJSON([
            "type": "punch_offer",
            "req_id": "o1",
            "target_device_id": targetDeviceID,
            "session_id": sidHex,
            "candidates": myCands,
            "ephemeral_pub": ephPubHex,
            "signature": sigHex
        ])

        var ans: [String: Any]?
        let deadline = Date().addingTimeInterval(8.0)
        while Date() < deadline {
            let rem = max(0.2, deadline.timeIntervalSinceNow)
            let frame = try ctrl.recvFrame(timeout: min(1.5, rem))
            guard frame.type == 0x01,
                  let obj = try? JSONSerialization.jsonObject(with: frame.payload) as? [String: Any] else {
                continue
            }
            let tp = obj["type"] as? String ?? ""
            if tp == "resp", (obj["req_id"] as? String) == "o1", (obj["ok"] as? Bool) != true {
                if ufd >= 0 { Darwin.close(ufd) }
                throw RelayError.message((obj["error"] as? String) ?? "对方拒绝连接或离线")
            }
            if tp == "punch_answer", (obj["session_id"] as? String) == sidHex {
                ans = obj
                break
            }
        }
        guard let ans else {
            if ufd >= 0 { Darwin.close(ufd) }
            throw RelayError.message("等待远程设备响应超时")
        }

        let peerEph = ans["ephemeral_pub"] as? String ?? ""
        let peerSig = ans["signature"] as? String ?? ""
        if !peer.pubKeyHex.isEmpty && !SpinelCrypto.verifyEd25519(pubHex: peer.pubKeyHex, message: "\(sidHex)|\(peerEph)", sigHex: peerSig) {
            if ufd >= 0 { Darwin.close(ufd) }
            throw RelayError.message("远程设备 Ed25519 签名验证失败")
        }

        guard let sessionKey = SpinelCrypto.deriveSessionKey(myPriv: ephPriv, peerPubHex: peerEph, sessionID: sidHex) else {
            if ufd >= 0 { Darwin.close(ufd) }
            throw RelayError.message("端到端密钥协商失败")
        }

        let peerCands = (ans["candidates"] as? [String]) ?? []
        if let punched = tryUdpPunch(udpFD: ufd, sid16: sid16, key: sessionKey, candidates: peerCands) {
            return E2eeTunnel(
                sid16: sid16,
                key: sessionKey,
                useUDP: true,
                udpFD: ufd,
                peerAddr: punched.addr,
                relayConn: nil,
                transportLabel: punched.label
            )
        }

        if ufd >= 0 { Darwin.close(ufd) }

        let (rconn, _) = try TlsRelayConnection.connect(serverAddr: cfg.serverAddr, pinnedFP: cfg.serverFP)
        _ = try rconn.rpc([
            "type": "relay_join",
            "req_id": "rj",
            "session_id": sidHex,
            "device_id": cfg.deviceID,
            "token": cfg.token,
            "target_device_id": targetDeviceID
        ])

        return E2eeTunnel(
            sid16: sid16,
            key: sessionKey,
            useUDP: false,
            udpFD: -1,
            peerAddr: sockaddr_storage(),
            relayConn: rconn,
            transportLabel: "云中继"
        )
    }

    private static func makeSprmDatagram(sid16: Data, seq: UInt64, dirBit: UInt8, fragIdx: UInt16, fragCnt: UInt16, chunk: Data) -> Data {
        var out = Data(capacity: 21 + chunk.count)
        out.append(contentsOf: "SPRM".utf8)
        out.append(sid16.prefix(4))
        var seqBE = seq.bigEndian
        withUnsafeBytes(of: &seqBE) { out.append(contentsOf: $0) }
        out.append(dirBit)
        var idxBE = fragIdx.bigEndian
        withUnsafeBytes(of: &idxBE) { out.append(contentsOf: $0) }
        var cntBE = fragCnt.bigEndian
        withUnsafeBytes(of: &cntBE) { out.append(contentsOf: $0) }
        out.append(chunk)
        return out
    }

    private static func tryUdpPunch(udpFD: Int32, sid16: Data, key: SymmetricKey, candidates: [String]) -> (addr: sockaddr_storage, label: String)? {
        guard udpFD >= 0 else { return nil }
        let addrs = candidates.compactMap { NetworkCandidates.sockaddrFromHostPort($0) }
        guard !addrs.isEmpty else { return nil }
        guard let pingCT = SpinelCrypto.aeadEncrypt(key: key, seq: 0, dirBit: 0, plaintext: Data([pktPunchPing])),
              let pongCT = SpinelCrypto.aeadEncrypt(key: key, seq: 0x8000_0000, dirBit: 0, plaintext: Data([pktPunchPong])) else {
            return nil
        }

        let pingDG = makeSprmDatagram(sid16: sid16, seq: 0, dirBit: 0, fragIdx: 0, fragCnt: 1, chunk: pingCT)
        let pongDG = makeSprmDatagram(sid16: sid16, seq: 0x8000_0000, dirBit: 0, fragIdx: 0, fragCnt: 1, chunk: pongCT)

        let end = Date().addingTimeInterval(1.2)
        var verifiedAddr: sockaddr_storage?
        var buf = [UInt8](repeating: 0, count: 2048)

        while Date() < end {
            for var ss in addrs {
                let len = socklen_t(ss.ss_len)
                _ = pingDG.withUnsafeBytes { raw in
                    withUnsafePointer(to: &ss) { ptr in
                        ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                            Darwin.sendto(udpFD, raw.baseAddress, pingDG.count, 0, saPtr, len)
                        }
                    }
                }
            }
            var pfd = pollfd(fd: udpFD, events: Int16(POLLIN), revents: 0)
            if Darwin.poll(&pfd, 1, 90) > 0 {
                var src = sockaddr_storage()
                var srclen = socklen_t(MemoryLayout<sockaddr_storage>.size)
                let n = withUnsafeMutablePointer(to: &src) { ptr in
                    ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                        Darwin.recvfrom(udpFD, &buf, buf.count, 0, saPtr, &srclen)
                    }
                }
                if n > 21,
                   Array(buf[0..<4]) == Array("SPRM".utf8),
                   Data(buf[4..<8]) == sid16.prefix(4),
                   buf[16] == 1 {
                    var rseqBE: UInt64 = 0
                    memcpy(&rseqBE, Array(buf[8..<16]), 8)
                    let rseq = UInt64(bigEndian: rseqBE)
                    let ct = Data(buf[21..<n])
                    if let pt = SpinelCrypto.aeadDecrypt(key: key, seq: rseq, dirBit: 1, combined: ct),
                       let code = pt.first,
                       code == pktPunchPing || code == pktPunchPong {
                        verifiedAddr = src
                        _ = pongDG.withUnsafeBytes { raw in
                            withUnsafePointer(to: &src) { ptr in
                                ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                                    Darwin.sendto(udpFD, raw.baseAddress, pongDG.count, 0, saPtr, srclen)
                                }
                            }
                        }
                        if code == pktPunchPong {
                            break
                        }
                    }
                }
            }
        }

        if let addr = verifiedAddr {
            return (addr, NetworkCandidates.classifyAddrLabel(addr))
        }
        return nil
    }

    public func sendMsg(type: SandboxMsgType, payload: Data = Data()) -> Bool {
        guard isOpen else { return false }
        var plain = Data(capacity: 1 + payload.count)
        plain.append(type.rawValue)
        plain.append(payload)

        sendLock.lock()
        let seq = sendSeq
        sendSeq += 1
        sendLock.unlock()

        guard let ct = SpinelCrypto.aeadEncrypt(key: key, seq: seq, dirBit: 0, plaintext: plain) else {
            return false
        }

        if useUDP {
            stateLock.lock()
            let ufd = udpFD
            var dst = peerAddr
            stateLock.unlock()
            guard ufd >= 0 else { return false }

            let count = max(1, (ct.count + Self.udpMTUChunk - 1) / Self.udpMTUChunk)
            guard count <= 4096 else { return false }
            let dstLen = socklen_t(dst.ss_len)
            for i in 0..<count {
                let start = i * Self.udpMTUChunk
                let end = min(ct.count, start + Self.udpMTUChunk)
                let dgram = Self.makeSprmDatagram(
                    sid16: sid16,
                    seq: seq,
                    dirBit: 0,
                    fragIdx: UInt16(i),
                    fragCnt: UInt16(count),
                    chunk: Data(ct[start..<end])
                )
                _ = dgram.withUnsafeBytes { raw in
                    withUnsafePointer(to: &dst) { ptr in
                        ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { saPtr in
                            Darwin.sendto(ufd, raw.baseAddress, dgram.count, 0, saPtr, dstLen)
                        }
                    }
                }
            }
            return true
        }

        guard let rconn = relayConn else { return false }
        var frame = Data(capacity: 25 + ct.count)
        frame.append(sid16)
        var seqBE = seq.bigEndian
        withUnsafeBytes(of: &seqBE) { frame.append(contentsOf: $0) }
        frame.append(0) // client dir_bit = 0
        frame.append(ct)
        do {
            try rconn.sendFrame(type: 0x02, payload: frame)
            return true
        } catch {
            close()
            return false
        }
    }

    public func recvMsg(timeoutMs: Int32 = 300) -> (type: UInt8, payload: Data)? {
        guard isOpen else { return nil }

        if useUDP {
            let deadline = Date().addingTimeInterval(Double(max(1, timeoutMs)) / 1000.0)
            var buf = [UInt8](repeating: 0, count: 2048)
            while isOpen {
                let remMs = Int32(deadline.timeIntervalSinceNow * 1000.0)
                guard remMs > 0 else { return nil }
                stateLock.lock()
                let ufd = udpFD
                stateLock.unlock()
                guard ufd >= 0 else { return nil }

                var pfd = pollfd(fd: ufd, events: Int16(POLLIN), revents: 0)
                guard Darwin.poll(&pfd, 1, remMs) > 0 else { return nil }
                let n = Darwin.recvfrom(ufd, &buf, buf.count, 0, nil, nil)
                guard n > 21,
                      Array(buf[0..<4]) == Array("SPRM".utf8),
                      Data(buf[4..<8]) == sid16.prefix(4) else {
                    continue
                }
                var seqBE: UInt64 = 0
                memcpy(&seqBE, Array(buf[8..<16]), 8)
                let seq = UInt64(bigEndian: seqBE)
                let dir = buf[16]
                let fidx = (UInt16(buf[17]) << 8) | UInt16(buf[18])
                let fcnt = (UInt16(buf[19]) << 8) | UInt16(buf[20])
                guard dir == 1, fcnt > 0, fidx < fcnt, fcnt <= 4096 else { continue }
                if seq == 0 || seq == 0x8000_0000 { continue }

                let ctFull: Data
                if fcnt == 1 {
                    ctFull = Data(buf[21..<n])
                } else {
                    var acc = frags[seq] ?? FragAccum(total: fcnt, received: 0, parts: Array(repeating: nil, count: Int(fcnt)))
                    if acc.total != fcnt {
                        acc = FragAccum(total: fcnt, received: 0, parts: Array(repeating: nil, count: Int(fcnt)))
                    }
                    if acc.parts[Int(fidx)] == nil {
                        acc.parts[Int(fidx)] = Data(buf[21..<n])
                        acc.received += 1
                    }
                    if acc.received < acc.total {
                        frags[seq] = acc
                        if frags.count > 16, let firstKey = frags.keys.sorted().first {
                            frags.removeValue(forKey: firstKey)
                        }
                        continue
                    }
                    frags.removeValue(forKey: seq)
                    var merged = Data()
                    for part in acc.parts {
                        if let p = part { merged.append(p) }
                    }
                    ctFull = merged
                }

                if let pt = SpinelCrypto.aeadDecrypt(key: key, seq: seq, dirBit: 1, combined: ctFull),
                   let mtype = pt.first {
                    return (mtype, Data(pt.dropFirst()))
                }
            }
            return nil
        }

        guard let rconn = relayConn else { return nil }
        do {
            let frame = try rconn.recvFrame(timeout: Double(max(50, timeoutMs)) / 1000.0)
            guard frame.type == 0x02, frame.payload.count >= 25 + 16 else { return nil }
            let pl = frame.payload
            guard pl.prefix(16) == sid16 else { return nil }
            var seqBE: UInt64 = 0
            pl.subdata(in: 16..<24).withUnsafeBytes { raw in
                if let b = raw.baseAddress { memcpy(&seqBE, b, 8) }
            }
            let seq = UInt64(bigEndian: seqBE)
            let dir = pl[pl.startIndex + 24]
            guard dir == 1 else { return nil }
            let ct = Data(pl.dropFirst(25))
            guard let pt = SpinelCrypto.aeadDecrypt(key: key, seq: seq, dirBit: 1, combined: ct),
                  let mtype = pt.first else {
                return nil
            }
            return (mtype, Data(pt.dropFirst()))
        } catch {
            if let rerr = error as? RelayError,
               case .message(let msg) = rerr,
               msg.contains("超时") {
                return nil
            }
            close()
            return nil
        }
    }
}
