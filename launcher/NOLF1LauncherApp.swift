// NOLF1LauncherApp.swift -- the entry point. With `--selftest <game folder>`
// it runs the self-test (SelfTest.swift) on the command line and exits instead
// of opening a window.

import SwiftUI
import AppKit

/// Cmd-Q while the game runs in this process: the game is asked to end the
/// way its close button ends it (settings written, audio stopped, torn down),
/// and the app quits when it has -- never with the game's audio still running.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard InProcessGame.running else { return .terminateNow }
        InProcessGame.quitWhenDone = true
        NOLF1_RequestQuit()
        return .terminateCancel
    }
}

@main
struct NOLF1LauncherApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    init() {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--selftest") {
            let folder = i + 1 < args.count ? args[i + 1] : ""
            exit(SelfTest.run(folder: folder))
        }
        // `--snapshot <out.png> [game folder] [advanced]`: the window rendered
        // into a picture, for checking the layout without opening it. The
        // folder is a REGISTERED default, so nothing is saved.
        if let i = args.firstIndex(of: "--snapshot"), i + 1 < args.count {
            if i + 2 < args.count { UserDefaults.standard.register(defaults: ["gamePath": args[i + 2]]) }
            let advanced = args.contains("advanced")
            exit(MainActor.assumeIsolated { Self.snapshot(to: args[i + 1], advanced: advanced) })
        }
    }

    @MainActor
    static func snapshot(to path: String, advanced: Bool) -> Int32 {
        let view = ContentView(showAdvanced: advanced).frame(height: advanced ? 720 : 520)
        let r = ImageRenderer(content: view)
        r.scale = 2
        guard let img = r.nsImage, let tiff = img.tiffRepresentation,
              let rep = NSBitmapImageRep(data: tiff),
              let png = rep.representation(using: .png, properties: [:]) else { return 1 }
        return (try? png.write(to: URL(fileURLWithPath: path))) != nil ? 0 : 1
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowResizability(.contentSize)
        .commands { CommandGroup(replacing: .newItem) { } }
    }
}
