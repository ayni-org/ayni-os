import AppKit
import CoreText

// Native font backend for the registered screen Provider. No network or credentials here.
func renderPages(_ text: String) throws -> [String] {
    let font = CTFontCreateWithName("PingFangSC-Regular" as CFString, 17, nil)
    let attributes: [NSAttributedString.Key: Any] = [
        .font: font, .foregroundColor: NSColor.white,
        .paragraphStyle: { let p = NSMutableParagraphStyle(); p.lineSpacing = 4; return p }()
    ]
    let content = NSAttributedString(string: text.isEmpty ? " " : text, attributes: attributes)
    let setter = CTFramesetterCreateWithAttributedString(content)
    var offset = 0, pages: [String] = []
    let width = 240, height = 320
    while offset < content.length {
        var rgba = [UInt8](repeating: 0, count: width * height * 4)
        let bitmap: String = try rgba.withUnsafeMutableBytes { storage in
            guard let context = CGContext(data: storage.baseAddress, width: width, height: height,
                bitsPerComponent: 8, bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
                throw NSError(domain: "Screen", code: 1)
            }
            context.setFillColor(NSColor.black.cgColor)
            context.fill(CGRect(x: 0, y: 0, width: width, height: height))
            context.textMatrix = .identity
            let title = NSAttributedString(string: "AYNI · USB", attributes: [
                .font: CTFontCreateWithName("Helvetica" as CFString, 12, nil),
                .foregroundColor: NSColor.systemTeal])
            context.textPosition = CGPoint(x: 12, y: 299)
            CTLineDraw(CTLineCreateWithAttributedString(title), context)
            let path = CGPath(rect: CGRect(x: 12, y: 24, width: 216, height: 264), transform: nil)
            let frame = CTFramesetterCreateFrame(setter, CFRange(location: offset, length: 0), path, nil)
            CTFrameDraw(frame, context)
            let visible = CTFrameGetVisibleStringRange(frame)
            guard visible.length > 0 else { throw NSError(domain: "Screen", code: 2) }
            offset += visible.length
            let footer = NSAttributedString(string: "第 \(pages.count + 1) 页 · 在 Mac 翻页", attributes: [
                .font: CTFontCreateWithName("PingFangSC-Regular" as CFString, 11, nil),
                .foregroundColor: NSColor.lightGray])
            context.textPosition = CGPoint(x: 12, y: 8)
            CTLineDraw(CTLineCreateWithAttributedString(footer), context)
            var rgb = Data(capacity: width * height * 2)
            let bytes = storage.bindMemory(to: UInt8.self)
            for pixel in 0..<(width * height) {
                let at = pixel * 4
                let value = (UInt16(bytes[at] >> 3) << 11) | (UInt16(bytes[at+1] >> 2) << 5) | UInt16(bytes[at+2] >> 3)
                rgb.append(UInt8(value >> 8)); rgb.append(UInt8(value & 255))
            }
            return rgb.base64EncodedString()
        }
        pages.append(bitmap)
        if pages.count > 32 { throw NSError(domain: "Screen", code: 3) }
    }
    return pages
}

if CommandLine.arguments.contains("--render") {
    do {
        let data = FileHandle.standardInput.readDataToEndOfFile()
        let request = try JSONSerialization.jsonObject(with: data) as! [String: String]
        let pages = try renderPages(request["text"] ?? "")
        let encoded = try JSONSerialization.data(withJSONObject: ["pages": pages])
        FileHandle.standardOutput.write(encoded)
        exit(0)
    } catch { exit(1) }
}

final class App: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var window: NSWindow!
    var process: Process?
    var input: FileHandle?
    var received = Data()
    let serial = NSTextField(string: "/dev/cu.usbmodem5D013686941")
    let url = NSTextField(string: "https://api.deepseek.com")
    let model = NSTextField(string: "deepseek-flash")
    let key = NSSecureTextField(string: "")
    let prompt = NSTextView()
    let output = NSTextView()
    let status = NSTextField(wrappingLabelWithString: "连接 USB 后填写 API Key，再点击连接。密钥仅保留在当前进程内存。")
    var connectButton: NSButton!
    var sendButton: NSButton!
    var page = 0, pageCount = 0
    var connected = false
    let motorLimit = NSTextField(string: "90")
    var motorAlert: NSAlert?
    var closeAlert = false, closing = false, allowTerminate = false
    func dismissMotor() {
        if let alert = motorAlert { window.endSheet(alert.window, returnCode: .abort) }
        motorAlert = nil
    }
    func motorReview(_ value: [String: Any], observe: Bool) {
        guard motorAlert == nil, !closing else { command(["action": "motor_stop"]); return }
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = observe ? "请观察两轮是否已经停下" : "批准这一次电机动作？"
        let first = value["motor1_percent"] as? Int ?? 0
        let second = value["motor2_percent"] as? Int ?? 0
        let duration = value["duration_ms"] as? Int ?? 0
        alert.informativeText = "电机 1：\(first)% PWM；电机 2：\(second)% PWM；\(Double(duration)/1000) 秒。\n" +
            (observe ? "ESP32 已结束输出并读回四脚为低；这不证明轮子静止。请亲眼确认。若仍在转或有异响，立即切断驱动 10V 电源。" :
            "仅限车轮架空的人工台架。确认独立断电开关就手后，才批准本次动作。30 秒未确认则失效。")
        alert.addButton(withTitle: observe ? "两轮已静止" : "不执行")
        alert.addButton(withTitle: observe ? "异常，停止并断电" : "批准本次")
        motorAlert = alert
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self = self else { return }
            self.motorAlert = nil
            if response == .abort { return }
            var answer = value
            answer["action"] = observe ? "motor_observed" : "motor_decision"
            answer["approved"] = observe ? response == .alertFirstButtonReturn : response == .alertSecondButtonReturn
            self.command(answer)
        }
    }
    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 820, height: 850),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "Ayni · ESP32 屏幕、扬声器与电机台架"
        window.delegate = self
        window.minSize = NSSize(width: 820, height: 850)
        let root = NSView(frame: window.contentView!.bounds)
        window.contentView = root
        func label(_ text: String, _ y: CGFloat) {
            let field = NSTextField(labelWithString: text)
            field.frame = NSRect(x: 22, y: y, width: 78, height: 24)
            root.addSubview(field)
        }
        for (text, field, y) in [("USB 串口", serial, CGFloat(680)), ("API 地址", url, 644),
                                  ("模型", model, 608), ("API Key", key, 572)] {
            label(text, y)
            field.frame = NSRect(x: 106, y: y, width: 690, height: 26)
            root.addSubview(field)
        }
        let bench = NSTextField(wrappingLabelWithString: "人工台架：电机每次由模型提议、Mac 确认后执行。连接和关闭 USB 前必须关闭驱动 10V 电源。")
        bench.frame = NSRect(x: 22, y: 794, width: 774, height: 38)
        root.addSubview(bench)
        label("PWM 上限", 751)
        motorLimit.frame = NSRect(x: 106, y: 752, width: 60, height: 26)
        root.addSubview(motorLimit)
        _ = button("应用 1–90%", #selector(setMotorLimit), x: 176, y: 748, width: 130, root: root)
        let stop = button("停止电机（锁定）", #selector(stopMotor), x: 328, y: 748, width: 185, root: root)
        stop.contentTintColor = .systemRed
        let note = NSTextField(labelWithString: "单次最多 5 秒；需要独立断电开关")
        note.font = .systemFont(ofSize: 12)
        note.frame = NSRect(x: 523, y: 753, width: 270, height: 24)
        root.addSubview(note)
        connectButton = button("连接设备", #selector(connect), x: 22, y: 528, width: 112, root: root)
        _ = button("清空上下文", #selector(clear), x: 145, y: 528, width: 120, root: root)
        status.frame = NSRect(x: 280, y: 520, width: 515, height: 43)
        status.font = .systemFont(ofSize: 12)
        root.addSubview(status)
        label("输入文本", 484)
        scroll(prompt, y: 337, height: 146, root: root)
        sendButton = button("发送给 Ayni", #selector(send), x: 22, y: 294, width: 135, root: root)
        sendButton.isEnabled = false
        _ = button("取消当前任务", #selector(cancel), x: 169, y: 294, width: 135, root: root)
        _ = button("屏幕上一页", #selector(previous), x: 510, y: 294, width: 135, root: root)
        _ = button("屏幕下一页", #selector(next), x: 657, y: 294, width: 135, root: root)
        label("运行状态", 254)
        scroll(output, y: 24, height: 224, root: root)
        output.isEditable = false
        output.string = "模型通过能力调用控制 LCD、提示音和中文朗读。扬声器默认音量 80%。最终确认文字仅在此窗口显示。\n"
        window.center(); window.makeKeyAndOrderFront(nil)
        NSApplication.shared.activate(ignoringOtherApps: true)
    }
    func scroll(_ text: NSTextView, y: CGFloat, height: CGFloat, root: NSView) {
        let scroll = NSScrollView(frame: NSRect(x: 22, y: y, width: 774, height: height))
        scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder
        text.font = .systemFont(ofSize: 15)
        text.isRichText = false; text.isAutomaticQuoteSubstitutionEnabled = false
        text.textContainerInset = NSSize(width: 8, height: 8)
        scroll.documentView = text
        text.isVerticallyResizable = true; text.isHorizontallyResizable = false
        text.autoresizingMask = [.width]
        text.frame = scroll.contentView.bounds
        text.textContainer?.widthTracksTextView = true
        root.addSubview(scroll)
    }
    func button(_ title: String, _ action: Selector, x: CGFloat, y: CGFloat, width: CGFloat, root: NSView) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        b.bezelStyle = .rounded; b.frame = NSRect(x: x, y: y, width: width, height: 32)
        root.addSubview(b); return b
    }
    func command(_ value: [String: Any]) {
        guard let handle = input, let data = try? JSONSerialization.data(withJSONObject: value) else { return }
        do { try handle.write(contentsOf: data + Data([10])) }
        catch { status.stringValue = "本地桥接进程已关闭" }
    }
    @objc func connect() {
        let alert = NSAlert()
        alert.messageText = "连接前关闭电机驱动电源"
        alert.informativeText = "打开 USB 串口可能复位 ESP32。请确认独立 10V 驱动电源已关闭、两轮已静止；USB 保持连接。"
        alert.addButton(withTitle: "取消")
        alert.addButton(withTitle: "驱动已断电，连接")
        guard alert.runModal() == .alertSecondButtonReturn else { return }
        guard let limit = Int(motorLimit.stringValue), (1...90).contains(limit) else {
            status.stringValue = "PWM 上限必须为 1..90 的整数"; return
        }
        if process != nil {
            command(["action": "connect", "port": serial.stringValue, "url": url.stringValue,
                "model": model.stringValue, "key": key.stringValue, "driver_power_off": true, "motor_max_percent": limit])
            key.stringValue = ""
            connectButton.isEnabled = false
            serial.isEditable = false; url.isEditable = false; model.isEditable = false; key.isEditable = false
            return
        }
        let executable = URL(fileURLWithPath: CommandLine.arguments[0]).standardizedFileURL
        let root = executable.deletingLastPathComponent().deletingLastPathComponent()
        let p = Process()
        p.executableURL = URL(fileURLWithPath: ProcessInfo.processInfo.environment["AYNI_CHAT_PYTHON"] ?? "/usr/bin/python3")
        p.arguments = [root.appendingPathComponent("desktop/bridge.py").path, "--renderer", executable.path]
        if ProcessInfo.processInfo.environment["AYNI_CHAT_OFFLINE"] == "1" { p.arguments?.append("--offline") }
        let stdin = Pipe(), stdout = Pipe(), stderr = Pipe()
        p.standardInput = stdin; p.standardOutput = stdout; p.standardError = stderr
        stdout.fileHandleForReading.readabilityHandler = { [weak self] handle in
            let data = handle.availableData
            DispatchQueue.main.async { self?.consume(data) }
        }
        stderr.fileHandleForReading.readabilityHandler = { handle in _ = handle.availableData }
        do {
            try p.run(); process = p; input = stdin.fileHandleForWriting
            command(["action": "connect", "port": serial.stringValue, "url": url.stringValue,
                "model": model.stringValue, "key": key.stringValue, "driver_power_off": true, "motor_max_percent": limit])
            key.stringValue = ""
            connectButton.isEnabled = false
            serial.isEditable = false; url.isEditable = false; model.isEditable = false; key.isEditable = false
        } catch { status.stringValue = "无法启动 USB 桥接进程" }
    }
    func consume(_ data: Data) {
        received.append(data)
        while let end = received.firstIndex(of: 10) {
            let line = received[..<end]; received.removeSubrange(...end)
            guard let object = try? JSONSerialization.jsonObject(with: line) as? [String: Any],
                  let type = object["type"] as? String else { continue }
            let value = object["value"]
            if type == "motor_review", let review = value as? [String: Any] { motorReview(review, observe: false) }
            if type == "motor_observe", let review = value as? [String: Any] { motorReview(review, observe: true) }
            if type == "motor_dismiss" { dismissMotor() }
            if type == "closed", value as? Bool == true {
                allowTerminate = true; NSApplication.shared.terminate(nil)
            }
            if type == "motor_state", let state = value as? [String: Any] {
                let active = state["motion_enabled"] as? Bool ?? false
                let limit = state["max_percent"] as? Int ?? 0
                output.string += "[电机] PWM 上限 \(limit)%，\(active ? "人工逐次批准" : "未使能或已锁定")；电源状态未测量。\n"
            }
            if type == "ready", let ready = value as? Bool { sendButton.isEnabled = ready; connected = ready }
            if type == "busy", let busy = value as? Bool { sendButton.isEnabled = connected && !busy }
            if type == "display", let display = value as? [String: Any] {
                page = (display["page"] as? Int ?? 1) - 1; pageCount = display["count"] as? Int ?? 0
                status.stringValue = "LCD 完成回执：第 \(page+1)/\(pageCount) 页"
            }
            if let message = value as? String {
                let prefix = type == "screen_text" ? "屏幕能力正文" : type == "answer" ? "模型最终确认" : type
                output.string += "[\(prefix)] \(message)\n"
                output.scrollToEndOfDocument(nil)
                if type == "status" || type == "error" { status.stringValue = message }
                if type == "error" && !connected {
                    connectButton.isEnabled = true
                    serial.isEditable = true; url.isEditable = true; model.isEditable = true; key.isEditable = true
                }
            }
        }
    }
    @objc func send() { command(["action": "send", "text": prompt.string]) }
    @objc func clear() { command(["action": "clear"]) }
    @objc func cancel() { command(["action": "cancel"]) }
    @objc func previous() { command(["action": "page", "index": max(0, page-1)]) }
    @objc func next() { command(["action": "page", "index": min(max(0,pageCount-1),page+1)]) }
    @objc func setMotorLimit() {
        guard let limit = Int(motorLimit.stringValue), (1...90).contains(limit) else {
            status.stringValue = "PWM 上限必须为 1..90 的整数"; return
        }
        command(["action": "motor_settings", "limit": limit])
    }
    @objc func stopMotor() { command(["action": "motor_stop"]); dismissMotor() }
    func requestClose() {
        guard !closeAlert && !closing else { return }
        if process == nil { allowTerminate = true; NSApplication.shared.terminate(nil); return }
        stopMotor()
        closeAlert = true
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "关闭前切断驱动 10V 电源"
        alert.informativeText = "关闭串口可能复位 ESP32。先关闭独立驱动电源并确认两轮静止；确认后再关闭会话。取消则保持 USB 会话和停止锁定。"
        alert.addButton(withTitle: "保持连接")
        alert.addButton(withTitle: "已断电且静止，关闭")
        alert.beginSheetModal(for: window) { [weak self] response in
            guard let self = self else { return }
            self.closeAlert = false
            if response == .alertSecondButtonReturn {
                self.closing = true
                self.command(["action": "exit", "driver_power_off": true])
            }
        }
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool { requestClose(); return false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if allowTerminate { return .terminateNow }
        requestClose(); return .terminateCancel
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }
}

let application = NSApplication.shared
let delegate = App()
application.setActivationPolicy(.regular)
application.delegate = delegate
application.run()
