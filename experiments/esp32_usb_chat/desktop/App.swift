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
    func applicationDidFinishLaunching(_ notification: Notification) {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 820, height: 730),
            styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "Ayni · ESP32 USB 屏幕与扬声器实验"
        window.delegate = self
        window.minSize = NSSize(width: 820, height: 730)
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
        if process != nil {
            command(["action": "connect", "port": serial.stringValue, "url": url.stringValue,
                "model": model.stringValue, "key": key.stringValue])
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
                "model": model.stringValue, "key": key.stringValue])
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
    func windowWillClose(_ notification: Notification) {
        command(["action": "exit"])
        NSApplication.shared.terminate(nil)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

let application = NSApplication.shared
let delegate = App()
application.setActivationPolicy(.regular)
application.delegate = delegate
application.run()
