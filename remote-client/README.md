# SpinelOS 跨平台远程沙箱客户端 (`remote-client/`)

用于从其他设备通过 `spinel-relay`（TLS 1.3 + UDP STUN/打洞 + X25519/ChaCha20-Poly1305 E2EE）画中画接管 SpinelOS 台式机的平行沙箱：

- [`macos/SpinelRemotePiP/`](macos/SpinelRemotePiP/) — 原生 Swift（SwiftUI 菜单栏 + AppKit 无边框毛玻璃画中画浮窗 + `CryptoKit` + `zlib`），编译命令：
  ```bash
  ./remote-client/macos/SpinelRemotePiP/scripts/build-macos-app.sh
  open out/SpinelRemotePiP.app
  ```
- `linux/` — （预留）独立轻量化 GTK4 / Wayland 远程画中画查看端（面向非 SpinelOS 的通用 Linux 桌面）。
- `windows/` — （预留）Windows 原生 WinUI / OpenSSL / Direct2D 远程画中画查看端。
