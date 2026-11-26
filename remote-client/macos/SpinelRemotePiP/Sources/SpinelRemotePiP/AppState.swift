import Foundation
import SpinelRemoteCore
import SwiftUI

@MainActor
final class RelayAppModel: ObservableObject {
    @Published var config: RelayConfig
    @Published var serverAddrInput: String
    @Published var udpPortInput: String
    @Published var serverFPInput: String
    @Published var accountInput: String
    @Published var passwordInput: String = ""
    @Published var pairCodeInput: String = ""
    @Published var createdPairCode: String = ""

    @Published var devices: [RelayPeerDevice] = []
    @Published var busy: Bool = false
    @Published var statusText: String = ""
    @Published var isError: Bool = false
    @Published var connectedPeerName: String?

    private var pipController: PipWindowController?

    init() {
        let cfg = RelayStore.loadConfig()
        self.config = cfg
        self.serverAddrInput = cfg.serverAddr
        self.udpPortInput = String(cfg.udpPort)
        self.serverFPInput = cfg.serverFP
        self.accountInput = cfg.account
        if cfg.isLoggedIn {
            refreshDevices()
        }
    }

    private func showNotice(_ text: String, error: Bool = false) {
        statusText = text
        isError = error
    }

    func toggleCommandToCtrl(_ enabled: Bool) {
        config.mapCommandToCtrl = enabled
        RelayStore.saveConfig(config)
        pipController?.setMapCommandToCtrl(enabled)
    }

    func resetFingerprint() {
        serverFPInput = ""
        config.serverFP = ""
        RelayStore.saveConfig(config)
        showNotice("已清除服务器指纹，下次连接将重新自动信任并锁定")
    }

    func auth(isRegister: Bool) {
        let addr = serverAddrInput.trimmingCharacters(in: .whitespacesAndNewlines)
        let udp = Int(udpPortInput) ?? 7444
        let fp = serverFPInput.trimmingCharacters(in: .whitespacesAndNewlines)
        let acc = accountInput.trimmingCharacters(in: .whitespacesAndNewlines)
        let pwd = passwordInput
        guard !addr.isEmpty, !acc.isEmpty, pwd.count >= 6 else {
            showNotice("请填写服务器地址、账号名及至少 6 位密码", error: true)
            return
        }
        busy = true
        showNotice(isRegister ? "正在注册 Spinel ID..." : "正在登录...")
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let (saved, list) = try RelayRPC.auth(
                    isRegister: isRegister,
                    serverAddr: addr,
                    udpPort: udp,
                    serverFP: fp,
                    account: acc,
                    password: pwd
                )
                Task { @MainActor in
                    self.busy = false
                    self.config = saved
                    self.serverFPInput = saved.serverFP
                    self.passwordInput = ""
                    self.devices = list
                    self.showNotice(isRegister ? "注册并绑定成功" : "已登录，同账号设备已刷新")
                }
            } catch {
                Task { @MainActor in
                    self.busy = false
                    self.showNotice(error.localizedDescription, error: true)
                }
            }
        }
    }

    func claimPairCode() {
        let addr = serverAddrInput.trimmingCharacters(in: .whitespacesAndNewlines)
        let udp = Int(udpPortInput) ?? 7444
        let fp = serverFPInput.trimmingCharacters(in: .whitespacesAndNewlines)
        let code = pairCodeInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !addr.isEmpty, code.count >= 6 else {
            showNotice("请填写服务器地址与 6 位配对码", error: true)
            return
        }
        busy = true
        showNotice("正在通过配对码加入账号...")
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let (saved, list) = try RelayRPC.claimPairCode(
                    serverAddr: addr,
                    udpPort: udp,
                    serverFP: fp,
                    pairCode: code
                )
                Task { @MainActor in
                    self.busy = false
                    self.config = saved
                    self.accountInput = saved.account
                    self.serverFPInput = saved.serverFP
                    self.pairCodeInput = ""
                    self.devices = list
                    self.showNotice("配对成功，已加入账号 \(saved.account)")
                }
            } catch {
                Task { @MainActor in
                    self.busy = false
                    self.showNotice(error.localizedDescription, error: true)
                }
            }
        }
    }

    func createPairCode() {
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let code = try RelayRPC.createPairCode()
                Task { @MainActor in
                    self.busy = false
                    self.createdPairCode = code
                    self.showNotice("已生成配对码（10 分钟内有效）")
                }
            } catch {
                Task { @MainActor in
                    self.busy = false
                    self.showNotice(error.localizedDescription, error: true)
                }
            }
        }
    }

    func refreshDevices(showNoticeOnSuccess: Bool = false) {
        guard config.isLoggedIn else { return }
        busy = true
        DispatchQueue.global(qos: .userInitiated).async {
            do {
                let list = try RelayRPC.fetchDevices()
                Task { @MainActor in
                    self.busy = false
                    self.devices = list
                    if showNoticeOnSuccess {
                        let fmt = DateFormatter()
                        fmt.dateFormat = "HH:mm:ss"
                        self.showNotice("已刷新（\(fmt.string(from: Date()))）")
                    }
                }
            } catch {
                Task { @MainActor in
                    self.busy = false
                    self.showNotice(error.localizedDescription, error: true)
                }
            }
        }
    }

    func logout() {
        disconnectPiP()
        config.token = ""
        RelayStore.saveConfig(config)
        devices = []
        createdPairCode = ""
        showNotice("已退出 Spinel ID")
    }

    func connectPeer(_ peer: RelayPeerDevice, closePopover: @escaping @MainActor @Sendable () -> Void) {
        disconnectPiP()
        busy = true
        showNotice("正在加密连接 \(peer.name)...")
        let peerID = peer.deviceID
        let peerName = peer.name.isEmpty ? peer.deviceID : peer.name
        let cmdToCtrl = config.mapCommandToCtrl

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            do {
                let tunnel = try E2eeTunnel.dialRemoteHost(targetDeviceID: peerID)
                Task { @MainActor [weak self] in
                    guard let self else {
                        tunnel.close()
                        return
                    }
                    self.busy = false
                    self.showNotice("已连接 \(peerName)（\(tunnel.transportLabel)）")
                    self.connectedPeerName = peerName
                    let ctrl = PipWindowController(
                        peerName: peerName,
                        tunnel: tunnel,
                        mapCommandToCtrl: cmdToCtrl
                    ) { [weak self] in
                        self?.connectedPeerName = nil
                        self?.pipController = nil
                    }
                    self.pipController = ctrl
                    ctrl.showWindow(nil)
                    closePopover()
                }
            } catch {
                Task { @MainActor [weak self] in
                    self?.busy = false
                    self?.showNotice(error.localizedDescription, error: true)
                }
            }
        }
    }

    func disconnectPiP() {
        pipController?.disconnectAndClose()
        pipController = nil
        connectedPeerName = nil
    }
}
