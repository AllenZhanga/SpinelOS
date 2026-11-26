import AppKit
import CoreGraphics
import Foundation
import SpinelRemoteCore

private final class PipPanelWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

private final class PipVideoCanvasView: NSView {
    var takeover: Bool = false {
        didSet { window?.invalidateCursorRects(for: self) }
    }
    var mapCommandToCtrl: Bool = true
    var onToggleTakeover: ((Bool) -> Void)?
    var onSendInput: ((SandboxMsgType, Data) -> Void)?

    private var frameWidth: Int = 0
    private var frameHeight: Int = 0
    private var lastRenderRect: NSRect = .zero
    private var trackingArea: NSTrackingArea?
    private var currentFlags: NSEvent.ModifierFlags = []

    override var acceptsFirstResponder: Bool { true }
    override var mouseDownCanMoveWindow: Bool { !takeover }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor(calibratedWhite: 0.08, alpha: 1.0).cgColor
        layer?.contentsGravity = .resizeAspect
        layer?.cornerRadius = 10
        layer?.masksToBounds = true
    }

    required init?(coder: NSCoder) {
        nil
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let existing = trackingArea {
            removeTrackingArea(existing)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseMoved, .activeAlways, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
        trackingArea = area
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: takeover ? .arrow : .pointingHand)
    }

    func updateFrame(width: Int, height: Int, bgra: Data) {
        guard width > 0, height > 0, bgra.count >= width * height * 4 else { return }
        frameWidth = width
        frameHeight = height

        let cs = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue
        guard let provider = CGDataProvider(data: bgra as CFData),
              let cgImg = CGImage(
                width: width,
                height: height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: width * 4,
                space: cs,
                bitmapInfo: CGBitmapInfo(rawValue: bitmapInfo),
                provider: provider,
                decode: nil,
                shouldInterpolate: true,
                intent: .defaultIntent
              ) else {
            return
        }
        layer?.contents = cgImg
        recomputeAspectRect()
    }

    private func recomputeAspectRect() {
        guard frameWidth > 0, frameHeight > 0, bounds.width > 0, bounds.height > 0 else {
            lastRenderRect = bounds
            return
        }
        let scale = min(bounds.width / CGFloat(frameWidth), bounds.height / CGFloat(frameHeight))
        let rw = CGFloat(frameWidth) * scale
        let rh = CGFloat(frameHeight) * scale
        lastRenderRect = NSRect(
            x: (bounds.width - rw) * 0.5,
            y: (bounds.height - rh) * 0.5,
            width: rw,
            height: rh
        )
    }

    private func normalizedPointer(from event: NSEvent) -> (UInt16, UInt16)? {
        recomputeAspectRect()
        let pt = convert(event.locationInWindow, from: nil)
        guard lastRenderRect.width > 1, lastRenderRect.height > 1 else { return nil }
        let rx = max(0.0, min(1.0, (pt.x - lastRenderRect.minX) / lastRenderRect.width))
        // AppKit origin is bottom-left; Wayland surface origin is top-left.
        let ryTopDown = max(0.0, min(1.0, 1.0 - (pt.y - lastRenderRect.minY) / lastRenderRect.height))
        let nx = UInt16((rx * 10000.0).rounded())
        let ny = UInt16((ryTopDown * 10000.0).rounded())
        return (nx, ny)
    }

    private func sendMouseMove(_ event: NSEvent) {
        guard takeover, let (nx, ny) = normalizedPointer(from: event) else { return }
        var p = Data(capacity: 4)
        p.append(UInt8((nx >> 8) & 0xFF))
        p.append(UInt8(nx & 0xFF))
        p.append(UInt8((ny >> 8) & 0xFF))
        p.append(UInt8(ny & 0xFF))
        onSendInput?(.mouseMove, p)
    }

    private func sendMouseButton(_ evdevBtn: UInt32, pressed: Bool, event: NSEvent) {
        sendMouseMove(event)
        var p = Data(capacity: 5)
        var bBE = evdevBtn.bigEndian
        withUnsafeBytes(of: &bBE) { p.append(contentsOf: $0) }
        p.append(pressed ? 1 : 0)
        onSendInput?(.mouseBtn, p)
    }

    override func mouseMoved(with event: NSEvent) {
        sendMouseMove(event)
    }

    override func mouseDragged(with event: NSEvent) {
        if takeover {
            sendMouseMove(event)
        } else {
            super.mouseDragged(with: event)
        }
    }

    override func rightMouseDragged(with event: NSEvent) {
        if takeover { sendMouseMove(event) }
    }

    override func otherMouseDragged(with event: NSEvent) {
        if takeover { sendMouseMove(event) }
    }

    override func mouseDown(with event: NSEvent) {
        if !takeover {
            onToggleTakeover?(true)
            return
        }
        window?.makeFirstResponder(self)
        sendMouseButton(272, pressed: true, event: event) // BTN_LEFT
    }

    override func mouseUp(with event: NSEvent) {
        if takeover {
            sendMouseButton(272, pressed: false, event: event)
        }
    }

    override func rightMouseDown(with event: NSEvent) {
        if takeover {
            window?.makeFirstResponder(self)
            sendMouseButton(273, pressed: true, event: event) // BTN_RIGHT
        }
    }

    override func rightMouseUp(with event: NSEvent) {
        if takeover {
            sendMouseButton(273, pressed: false, event: event)
        }
    }

    override func otherMouseDown(with event: NSEvent) {
        if takeover {
            window?.makeFirstResponder(self)
            sendMouseButton(274, pressed: true, event: event) // BTN_MIDDLE
        }
    }

    override func otherMouseUp(with event: NSEvent) {
        if takeover {
            sendMouseButton(274, pressed: false, event: event)
        }
    }

    override func scrollWheel(with event: NSEvent) {
        guard takeover else { return }
        // Invert scrollingDelta to match Linux natural wheel direction
        let dx = -event.scrollingDeltaX
        let dy = -event.scrollingDeltaY
        let scale: Double = event.hasPreciseScrollingDeltas ? 1.5 : 15.0
        let fdx = Int32((dx * scale * 256.0).rounded())
        let fdy = Int32((dy * scale * 256.0).rounded())
        guard fdx != 0 || fdy != 0 else { return }

        var p = Data(capacity: 8)
        var dxBE = UInt32(bitPattern: fdx).bigEndian
        var dyBE = UInt32(bitPattern: fdy).bigEndian
        withUnsafeBytes(of: &dxBE) { p.append(contentsOf: $0) }
        withUnsafeBytes(of: &dyBE) { p.append(contentsOf: $0) }
        onSendInput?(.mouseAxis, p)
    }

    private func sendKey(evdev: UInt32, pressed: Bool, mods: UInt32) {
        var p = Data(capacity: 9)
        var kBE = evdev.bigEndian
        withUnsafeBytes(of: &kBE) { p.append(contentsOf: $0) }
        p.append(pressed ? 1 : 0)
        var mBE = mods.bigEndian
        withUnsafeBytes(of: &mBE) { p.append(contentsOf: $0) }
        onSendInput?(.keyEvent, p)
    }

    override func keyDown(with event: NSEvent) {
        guard takeover else { return }
        if event.keyCode == 53 { // Escape exits takeover cleanly
            onToggleTakeover?(false)
            return
        }
        guard let evdev = MacToLinuxKeymap.evdevCode(for: event.keyCode, mapCommandToCtrl: mapCommandToCtrl) else {
            return
        }
        let mods = MacToLinuxKeymap.xkbModifiers(from: event.modifierFlags, mapCommandToCtrl: mapCommandToCtrl)
        sendKey(evdev: evdev, pressed: true, mods: mods)
    }

    override func keyUp(with event: NSEvent) {
        guard takeover,
              let evdev = MacToLinuxKeymap.evdevCode(for: event.keyCode, mapCommandToCtrl: mapCommandToCtrl) else {
            return
        }
        let mods = MacToLinuxKeymap.xkbModifiers(from: event.modifierFlags, mapCommandToCtrl: mapCommandToCtrl)
        sendKey(evdev: evdev, pressed: false, mods: mods)
    }

    override func flagsChanged(with event: NSEvent) {
        guard takeover else {
            currentFlags = event.modifierFlags
            return
        }
        let newFlags = event.modifierFlags
        let mods = MacToLinuxKeymap.xkbModifiers(from: newFlags, mapCommandToCtrl: mapCommandToCtrl)
        if let evdev = MacToLinuxKeymap.evdevCode(for: event.keyCode, mapCommandToCtrl: mapCommandToCtrl) {
            let pressed = newFlags.rawValue > currentFlags.rawValue
            sendKey(evdev: evdev, pressed: pressed, mods: mods)
        }
        currentFlags = newFlags
    }
}

private final class StopToken: @unchecked Sendable {
    private let lock = NSLock()
    private var stopped = false
    var isStopped: Bool {
        lock.lock()
        defer { lock.unlock() }
        return stopped
    }
    func stop() {
        lock.lock()
        stopped = true
        lock.unlock()
    }
}

@MainActor
final class PipWindowController: NSWindowController, NSWindowDelegate {
    private let tunnel: E2eeTunnel
    private let onClosed: () -> Void
    private let rxQueue = DispatchQueue(label: "spinel.mac.pip.rx")
    private let stopToken = StopToken()

    private let videoView = PipVideoCanvasView(frame: .zero)
    private let modeButton = NSButton(title: "接管键鼠", target: nil, action: nil)
    private let appButton = NSButton(title: "+ 应用", target: nil, action: nil)
    private let clipButton = NSButton(title: "同步剪贴板", target: nil, action: nil)
    private let minButton = NSButton(title: "收起", target: nil, action: nil)
    private let closeButton = NSButton(title: "断开", target: nil, action: nil)
    private let statusLabel = NSTextField(labelWithString: "")

    private var minimized = false
    private var takeover = false

    init(peerName: String, tunnel: E2eeTunnel, mapCommandToCtrl: Bool, onClosed: @escaping () -> Void) {
        self.tunnel = tunnel
        self.onClosed = onClosed

        let screenFrame = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        let initRect = NSRect(
            x: screenFrame.maxX - 480,
            y: screenFrame.minY + 48,
            width: 440,
            height: 296
        )
        let win = PipPanelWindow(
            contentRect: initRect,
            styleMask: [.borderless, .resizable],
            backing: .buffered,
            defer: false
        )
        win.isOpaque = false
        win.backgroundColor = .clear
        win.level = .floating
        win.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        win.isMovableByWindowBackground = true
        win.hasShadow = true
        win.minSize = NSSize(width: 320, height: 210)
        win.title = "Spinel 远程沙箱 - \(peerName)"

        super.init(window: win)
        win.delegate = self

        setupUI(peerName: peerName, mapCommandToCtrl: mapCommandToCtrl)
        startRxLoop()
        sendControl(.keyframeReq, Data())
    }

    required init?(coder: NSCoder) {
        nil
    }

    func setMapCommandToCtrl(_ enabled: Bool) {
        videoView.mapCommandToCtrl = enabled
    }

    private func setupUI(peerName: String, mapCommandToCtrl: Bool) {
        guard let win = window else { return }

        let glass = NSVisualEffectView(frame: win.contentView?.bounds ?? .zero)
        glass.autoresizingMask = [.width, .height]
        glass.material = .hudWindow
        glass.state = .active
        glass.wantsLayer = true
        glass.layer?.cornerRadius = 14
        glass.layer?.masksToBounds = true
        glass.layer?.borderWidth = 1
        glass.layer?.borderColor = NSColor.white.withAlphaComponent(0.18).cgColor
        win.contentView = glass

        let dot = NSView()
        dot.wantsLayer = true
        dot.layer?.backgroundColor = NSColor.systemGreen.cgColor
        dot.layer?.cornerRadius = 4
        dot.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([
            dot.widthAnchor.constraint(equalToConstant: 8),
            dot.heightAnchor.constraint(equalToConstant: 8)
        ])

        statusLabel.stringValue = "\(peerName) · \(tunnel.transportLabel)"
        statusLabel.font = .systemFont(ofSize: 12, weight: .semibold)
        statusLabel.textColor = .labelColor
        statusLabel.lineBreakMode = .byTruncatingTail

        for btn in [modeButton, appButton, clipButton, minButton, closeButton] {
            btn.bezelStyle = .rounded
            btn.controlSize = .small
        }
        modeButton.target = self
        modeButton.action = #selector(onToggleModeClicked)
        appButton.target = self
        appButton.action = #selector(onLaunchAppClicked)
        clipButton.target = self
        clipButton.action = #selector(onSyncClipboardClicked)
        minButton.target = self
        minButton.action = #selector(onToggleMinimizeClicked)
        closeButton.target = self
        closeButton.action = #selector(onDisconnectClicked)
        closeButton.contentTintColor = .systemRed

        let header = NSStackView(views: [dot, statusLabel, modeButton, appButton, clipButton, minButton, closeButton])
        header.orientation = .horizontal
        header.alignment = .centerY
        header.spacing = 6
        header.translatesAutoresizingMaskIntoConstraints = false

        videoView.translatesAutoresizingMaskIntoConstraints = false
        videoView.mapCommandToCtrl = mapCommandToCtrl
        videoView.onToggleTakeover = { [weak self] on in
            self?.setTakeover(on)
        }
        videoView.onSendInput = { [weak self] type, data in
            self?.sendControl(type, data)
        }

        glass.addSubview(header)
        glass.addSubview(videoView)

        NSLayoutConstraint.activate([
            header.topAnchor.constraint(equalTo: glass.topAnchor, constant: 8),
            header.leadingAnchor.constraint(equalTo: glass.leadingAnchor, constant: 12),
            header.trailingAnchor.constraint(equalTo: glass.trailingAnchor, constant: -12),
            header.heightAnchor.constraint(equalToConstant: 26),

            videoView.topAnchor.constraint(equalTo: header.bottomAnchor, constant: 8),
            videoView.leadingAnchor.constraint(equalTo: glass.leadingAnchor, constant: 8),
            videoView.trailingAnchor.constraint(equalTo: glass.trailingAnchor, constant: -8),
            videoView.bottomAnchor.constraint(equalTo: glass.bottomAnchor, constant: -8)
        ])
    }

    private func sendControl(_ type: SandboxMsgType, _ data: Data) {
        let tun = tunnel
        DispatchQueue.global(qos: .userInitiated).async {
            _ = tun.sendMsg(type: type, payload: data)
        }
    }

    private func setTakeover(_ enabled: Bool) {
        guard takeover != enabled else { return }
        takeover = enabled
        videoView.takeover = enabled
        modeButton.title = enabled ? "退出接管(Esc)" : "接管键鼠"
        sendControl(.setMode, Data([enabled ? 1 : 0]))

        guard let win = window else { return }
        if enabled {
            if minimized {
                minimized = false
                videoView.isHidden = false
                minButton.title = "收起"
            }
            var frame = win.frame
            let targetW: CGFloat = 920
            let targetH: CGFloat = 580
            frame.origin.x -= (targetW - frame.width) * 0.5
            frame.origin.y -= (targetH - frame.height) * 0.5
            frame.size = NSSize(width: targetW, height: targetH)
            if let vis = win.screen?.visibleFrame {
                frame.origin.x = max(vis.minX + 12, min(frame.origin.x, vis.maxX - frame.width - 12))
                frame.origin.y = max(vis.minY + 12, min(frame.origin.y, vis.maxY - frame.height - 12))
            }
            win.setFrame(frame, display: true, animate: true)
            win.makeKeyAndOrderFront(nil)
            win.makeFirstResponder(videoView)
        } else {
            var frame = win.frame
            let targetW: CGFloat = 440
            let targetH: CGFloat = 296
            frame.origin.x += (frame.width - targetW)
            frame.origin.y += (frame.height - targetH)
            frame.size = NSSize(width: targetW, height: targetH)
            win.setFrame(frame, display: true, animate: true)
        }
    }

    @objc private func onToggleModeClicked() {
        setTakeover(!takeover)
    }

    @objc private func onLaunchAppClicked() {
        let menu = NSMenu(title: "在远程沙箱中启动")
        for (label, bin) in [
            ("浏览器 (Chromium)", "chromium"),
            ("终端 (Spinel Terminal)", "spinel-terminal"),
            ("文件管理器 (Spinel Files)", "spinel-files"),
            ("VS Code", "code")
        ] {
            let item = NSMenuItem(title: label, action: #selector(onLaunchPreset(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = bin
            menu.addItem(item)
        }
        menu.popUp(positioning: nil, at: NSPoint(x: 0, y: appButton.bounds.height + 4), in: appButton)
    }

    @objc private func onLaunchPreset(_ sender: NSMenuItem) {
        guard let bin = sender.representedObject as? String else { return }
        sendControl(.launchApp, Data(bin.utf8))
    }

    @objc private func onSyncClipboardClicked() {
        if let str = NSPasteboard.general.string(forType: .string), !str.isEmpty {
            sendControl(.clipText, Data(str.utf8))
            let prev = statusLabel.stringValue
            statusLabel.stringValue = "已同步 Mac 剪贴板到远程沙箱"
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.8) { [weak self] in
                if self?.statusLabel.stringValue == "已同步 Mac 剪贴板到远程沙箱" {
                    self?.statusLabel.stringValue = prev
                }
            }
        }
    }

    @objc private func onToggleMinimizeClicked() {
        guard let win = window else { return }
        minimized.toggle()
        if minimized {
            if takeover { setTakeover(false) }
            videoView.isHidden = true
            minButton.title = "展开"
            var frame = win.frame
            let delta = frame.height - 44
            frame.origin.y += delta
            frame.size.height = 44
            win.minSize = NSSize(width: 320, height: 44)
            win.setFrame(frame, display: true, animate: true)
        } else {
            videoView.isHidden = false
            minButton.title = "收起"
            var frame = win.frame
            frame.origin.y -= (296 - frame.height)
            frame.size.height = 296
            win.minSize = NSSize(width: 320, height: 210)
            win.setFrame(frame, display: true, animate: true)
            sendControl(.keyframeReq, Data())
        }
    }

    @objc private func onDisconnectClicked() {
        disconnectAndClose()
    }

    func disconnectAndClose() {
        stopToken.stop()
        let tun = tunnel
        DispatchQueue.global(qos: .userInitiated).async {
            tun.close()
        }
        window?.close()
    }

    func windowWillClose(_ notification: Notification) {
        stopToken.stop()
        let tun = tunnel
        DispatchQueue.global(qos: .userInitiated).async {
            tun.close()
        }
        onClosed()
    }

    private func startRxLoop() {
        let tun = tunnel
        let stop = stopToken
        rxQueue.async { [weak self] in
            let dec = TileDecoder()
            while !stop.isStopped, tun.isOpen {
                guard let msg = tun.recvMsg(timeoutMs: 250) else { continue }
                if msg.type == SandboxMsgType.frameTile.rawValue {
                    if dec.decodeTileFrame(msg.payload) {
                        let w = dec.width
                        let h = dec.height
                        let bgraCopy = dec.bgra
                        Task { @MainActor [weak self] in
                            self?.videoView.updateFrame(width: w, height: h, bgra: bgraCopy)
                        }
                    }
                }
            }
            Task { @MainActor [weak self] in
                guard let self else { return }
                self.statusLabel.stringValue = "连接已断开"
            }
        }
    }
}
