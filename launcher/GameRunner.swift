// GameRunner.swift -- starts the game the way build/play.sh does, as its own
// process, and watches it.
//
// The game is the engine's command-line host (`nolf1`) with its server dylib
// beside it, both copied into this app's Contents/Helpers by the Xcode build.
// Everything play.sh exported is set here; nothing else is needed.
//
//   * game data      NOLF_GAME_DIR (GameInstall)
//   * a window       LT_WINDOW=1 -- also what makes the game keep its
//                    settings in ~/Library/Application Support/NOLF1
//   * renderer       LT_METAL=1, or LT_SOFTWARE=1
//   * size           LT_SCREEN=WxH
//   * wall clock     LT_REALTIME=1 (without it the game runs in slow motion)
//   * saves          the working directory: Application Support/NOLF1/Profile
//                    (the game names its files "Save\Summary.sav" etc.)
//   * output         ~/Library/Logs/NOLF1/game-<date>.log, every run

import Foundation
import AppKit

enum Paths {
    static var support: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("NOLF1", isDirectory: true)
    }
    static var profile: URL { support.appendingPathComponent("Profile", isDirectory: true) }
    static var backups: URL { support.appendingPathComponent("Profile Backups", isDirectory: true) }
    static var startupList: URL { support.appendingPathComponent("StartupList", isDirectory: true) }
    static var logs: URL {
        FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/NOLF1", isDirectory: true)
    }
    /// The game, inside this app. Overridable for development runs.
    static var gameExecutable: URL {
        if let p = ProcessInfo.processInfo.environment["NOLF1_GAME_BINARY"] { return URL(fileURLWithPath: p) }
        return Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/nolf1")
    }
}

struct LaunchOptions {
    enum Mode: String, CaseIterable, Identifiable {
        case play, mission, viewer
        var id: String { rawValue }
    }
    var mode: Mode = .play
    var width = 1024
    var height = 768
    var software = false
    var mission = 1                 // .mission: unlock up to this one
    var level = "M01S01"            // .viewer
    var trace = false
    var bubbles = true              // placed bubble emitters at their authored rate (E-009)
    var bounce = true               // falling particles bounce off floors (E-010)

    /// The game's environment. `headless` is for the self-test: no window,
    /// a fixed number of frames, a level loaded straight away.
    func environment(gameDirs: [URL], headless: Bool = false, updates: Int = 0) -> [String: String] {
        var env = ProcessInfo.processInfo.environment.filter { !$0.key.hasPrefix("LT_") && !$0.key.hasPrefix("NOLF_") }
        env["NOLF_GAME_DIR"] = gameDirs.map { $0.path }.joined(separator: ":")
        if headless {
            env["LT_UPDATES"] = String(updates)
            env["LT_STUB_QUIET"] = "1"
            env["LT_SCREEN"] = "640x480"
        } else {
            env["LT_WINDOW"] = "1"
            env["LT_REALTIME"] = "1"
            env["LT_SCREEN"] = "\(width)x\(height)"
            if software { env["LT_SOFTWARE"] = "1" } else { env["LT_METAL"] = "1" }
        }
        switch mode {
        case .play, .mission:
            break                   // the game's own menus
        case .viewer:
            env["LT_NOSERVER"] = "1"
            env["LT_WORLD"] = "WORLDS\\\(level.uppercased()).DAT"
        }
        // Off = the original count: floor(rate x frame time), which is no
        // bubbles at all at 60 fps (HANDOVER 5dj, DEVIATIONS E-009).
        if !bubbles { env["LT_NOPSRATE"] = "1" }
        // Off = the original: sparks and blood fall through the floor (5dk, E-010).
        if !bounce { env["LT_NOFLOORBOUNCE"] = "1" }
        if trace {
            for k in ["LT_SHOTTRACE", "LT_DAMAGETRACE", "LT_LIFETRACE", "LT_CAMTRACE", "LT_ACTTRACE"] { env[k] = "1" }
            env["LT_TRIGTRACE"] = "2"
        }
        return env
    }
}

enum Profile {
    /// play.sh's `mission N`: raise [Status] NextMission in the game's own
    /// progress file so Single Player > Select mission offers it.
    static func unlock(upTo n: Int, in dir: URL) throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("Save\\Summary.sav")
        let text = (try? String(contentsOf: url, encoding: .isoLatin1)) ?? ""
        var out: [String] = []
        var inStatus = false, seen = false, done = false
        for line in text.components(separatedBy: "\n") {
            let t = line.trimmingCharacters(in: .whitespaces)
            if t.hasPrefix("[") {
                if inStatus && !done { out.append("\tNextMission = \(n)"); done = true }
                inStatus = t.lowercased().hasPrefix("[status]")
                if inStatus { seen = true }
                out.append(line); continue
            }
            if inStatus, t.lowercased().hasPrefix("nextmission"), let eq = t.firstIndex(of: "=") {
                let v = Int(t[t.index(after: eq)...].trimmingCharacters(in: .whitespaces)) ?? 0
                out.append("\tNextMission = \(max(v, n))"); done = true; continue
            }
            out.append(line)
        }
        if inStatus && !done { out.append("\tNextMission = \(n)"); done = true }
        if !seen { out.append(""); out.append("[Status]"); out.append("\tNextMission = \(n)") }
        try out.joined(separator: "\n").write(to: url, atomically: true, encoding: .isoLatin1)
    }

    /// play.sh's `--reset`: move every save aside (nothing is deleted).
    @discardableResult
    static func reset() throws -> URL? {
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(at: Paths.profile, includingPropertiesForKeys: nil),
              !items.isEmpty else { return nil }
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let dest = Paths.backups.appendingPathComponent(f.string(from: Date()), isDirectory: true)
        try fm.createDirectory(at: dest, withIntermediateDirectories: true)
        for item in items { try fm.moveItem(at: item, to: dest.appendingPathComponent(item.lastPathComponent)) }
        return dest
    }
}

@MainActor
final class GameRunner: ObservableObject {
    enum State: Equatable {
        case idle
        case running
        case ended(Int32)           // exit status
        case crashed(Int32)         // signal
        case failed(String)
    }
    @Published private(set) var state: State = .idle
    @Published private(set) var log: URL?
    private var process: Process?

    var isRunning: Bool { state == .running }

    func start(_ install: GameInstall, _ options: LaunchOptions) {
        guard process == nil else { return }
        do {
            let fm = FileManager.default
            let exe = Paths.gameExecutable
            guard fm.isExecutableFile(atPath: exe.path) else {
                state = .failed("The game itself is missing from the app (\(exe.path)).")
                return
            }
            try fm.createDirectory(at: Paths.profile, withIntermediateDirectories: true)
            if options.mode == .mission { try Profile.unlock(upTo: options.mission, in: Paths.profile) }
            let dirs = try install.directoriesForEngine()

            let logURL = try Self.newLog()
            let handle = try FileHandle(forWritingTo: logURL)

            let p = Process()
            p.executableURL = exe
            p.currentDirectoryURL = Paths.profile
            p.environment = options.environment(gameDirs: dirs)
            p.standardOutput = handle
            p.standardError = handle
            p.terminationHandler = { [weak self] proc in
                let status = proc.terminationStatus
                let reason = proc.terminationReason
                try? handle.close()
                Task { @MainActor in
                    guard let self else { return }
                    self.process = nil
                    self.state = reason == .uncaughtSignal ? .crashed(status) : .ended(status)
                }
            }
            try p.run()
            process = p
            log = logURL
            state = .running
            handOverFocus(to: p.processIdentifier)
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    func stop() { process?.terminate() }

    /// Since macOS 14 activation is cooperative: the game asks to come to the
    /// front (ltwindow.mm, `[NSApp activate]`), and that only works if the
    /// app in front -- this one, the Play button was just clicked -- yields.
    /// The game is a plain process, so wait for it to show up as one.
    private func handOverFocus(to pid: Int32) {
        Task { @MainActor in
            for _ in 0..<50 {
                if let app = NSRunningApplication(processIdentifier: pid), app.activationPolicy == .regular {
                    NSApp.yieldActivation(to: app)
                    app.activate()
                    return
                }
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
    }

    /// A fresh log file; the newest 20 are kept.
    static func newLog() throws -> URL {
        let fm = FileManager.default
        try fm.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
        let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
        let url = Paths.logs.appendingPathComponent("game-\(f.string(from: Date())).log")
        fm.createFile(atPath: url.path, contents: nil)
        if let old = try? fm.contentsOfDirectory(at: Paths.logs, includingPropertiesForKeys: nil)
            .filter({ $0.lastPathComponent.hasPrefix("game-") })
            .sorted(by: { $0.lastPathComponent > $1.lastPathComponent }), old.count > 20 {
            for u in old.dropFirst(20) { try? fm.removeItem(at: u) }
        }
        return url
    }
}
