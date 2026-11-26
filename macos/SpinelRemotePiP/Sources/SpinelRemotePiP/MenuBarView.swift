import SpinelRemoteCore
import SwiftUI

struct MenuBarView: View {
    @ObservedObject var model: RelayAppModel
    let closePopover: @MainActor @Sendable () -> Void
    let quitApp: @MainActor @Sendable () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("Spinel 远程沙箱", systemImage: "sparkles.rectangle.stack")
                    .font(.headline)
                Spacer()
                if let active = model.connectedPeerName {
                    Text("已连: \(active)")
                        .font(.caption2)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(Color.green.opacity(0.18), in: Capsule())
                        .foregroundStyle(.green)
                } else if model.config.isLoggedIn {
                    Text(model.config.account)
                        .font(.caption2)
                        .padding(.horizontal, 7)
                        .padding(.vertical, 3)
                        .background(Color.accentColor.opacity(0.15), in: Capsule())
                }
            }

            if !model.config.isLoggedIn {
                serverFields
                authSection
            } else {
                onlineHeader
                deviceListSection
            }

            if !model.statusText.isEmpty {
                Text(model.statusText)
                    .font(.caption)
                    .foregroundStyle(model.isError ? .red : .secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            Divider()

            HStack {
                Toggle("将 Command 映射为 Ctrl", isOn: Binding(
                    get: { model.config.mapCommandToCtrl },
                    set: { model.toggleCommandToCtrl($0) }
                ))
                .toggleStyle(.checkbox)
                .font(.caption)

                Spacer()

                Button("退出") {
                    quitApp()
                }
                .buttonStyle(.borderless)
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .frame(width: 360)
    }

    private var serverFields: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                TextField("服务器 IP:端口 (例 1.2.3.4:7443)", text: $model.serverAddrInput)
                    .textFieldStyle(.roundedBorder)
                TextField("UDP", text: $model.udpPortInput)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 64)
            }
            HStack(spacing: 6) {
                TextField("TLS 指纹 (留空首次自动锁定)", text: $model.serverFPInput)
                    .textFieldStyle(.roundedBorder)
                    .font(.caption)
                if !model.serverFPInput.isEmpty {
                    Button("清除") { model.resetFingerprint() }
                        .controlSize(.small)
                }
            }
        }
    }

    private var authSection: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 6) {
                TextField("Spinel 账号", text: $model.accountInput)
                    .textFieldStyle(.roundedBorder)
                SecureField("密码 (≥6位)", text: $model.passwordInput)
                    .textFieldStyle(.roundedBorder)
            }
            HStack(spacing: 8) {
                Button("登录") { model.auth(isRegister: false) }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.busy)
                Button("注册并绑定") { model.auth(isRegister: true) }
                    .disabled(model.busy)
            }

            Divider()

            HStack(spacing: 6) {
                TextField("输入 6 位配对码快速加入", text: $model.pairCodeInput)
                    .textFieldStyle(.roundedBorder)
                Button("配对加入") { model.claimPairCode() }
                    .disabled(model.busy)
            }
        }
    }

    private var onlineHeader: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("服务器: \(model.config.serverAddr)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text("本机 ID: \(model.config.deviceID)")
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
                Spacer()
                Button {
                    model.refreshDevices()
                } label: {
                    Image(systemName: "arrow.clockwise")
                }
                .controlSize(.small)
                .disabled(model.busy)

                Button("退出账号") {
                    model.logout()
                }
                .controlSize(.small)
            }

            HStack(spacing: 8) {
                Button("生成配对码") {
                    model.createPairCode()
                }
                .controlSize(.small)
                .disabled(model.busy)

                if !model.createdPairCode.isEmpty {
                    Text(model.createdPairCode)
                        .font(.system(.callout, design: .monospaced).bold())
                        .padding(.horizontal, 8)
                        .padding(.vertical, 2)
                        .background(Color.accentColor.opacity(0.15), in: RoundedRectangle(cornerRadius: 6))
                }
                Spacer()
            }
        }
    }

    private var deviceListSection: some View {
        let peers = model.devices.filter { $0.deviceID != model.config.deviceID }
        return VStack(alignment: .leading, spacing: 6) {
            Text("同账号可接管设备 (\(peers.count))")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)

            if peers.isEmpty {
                Text("暂无其他已绑定设备，可在设备 A 开启接力后点击右上角刷新。")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .padding(.vertical, 10)
            } else {
                ScrollView {
                    VStack(spacing: 6) {
                        ForEach(peers) { peer in
                            HStack(spacing: 8) {
                                Circle()
                                    .fill(peer.online ? Color.green : Color.gray.opacity(0.5))
                                    .frame(width: 8, height: 8)

                                VStack(alignment: .leading, spacing: 2) {
                                    Text(peer.name.isEmpty ? peer.deviceID : peer.name)
                                        .font(.callout.weight(.medium))
                                    HStack(spacing: 6) {
                                        Text(peer.online ? "在线" : "离线")
                                        Text("·")
                                        Text(peer.allowRemote ? "允许接管" : "未开启接管")
                                        if peer.sandboxRunning {
                                            Text("· 沙箱运行中")
                                                .foregroundStyle(.green)
                                        }
                                    }
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                }

                                Spacer()

                                Button("画中画") {
                                    model.connectPeer(peer, closePopover: closePopover)
                                }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                                .disabled(model.busy || !peer.online || !peer.allowRemote)
                            }
                            .padding(8)
                            .background(Color.primary.opacity(0.05), in: RoundedRectangle(cornerRadius: 8))
                        }
                    }
                }
                .frame(maxHeight: 200)
            }
        }
    }
}
