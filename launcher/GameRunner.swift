// GameRunner.swift -- starts the game the way build/play.sh does, INSIDE THIS
// APP'S OWN PROCESS (2026-11-21), and comes back to the launcher afterwards.
//
// ⚠️ ONE PROGRAM, ONE DOCK ICON. The engine and the game's client code are
// linked into this app (the static library `nolf1core`, -force_load; the
// bridging header declares NOLF1_Main). The game used to be a second
// executable, Contents/Helpers/nolf1, with its own Dock icon. Only the game
// MODULE is still a file of its own -- libnolfobject.dylib, in
// Contents/Frameworks, which the engine dlopens (NOLF_SERVER says where).
//
// ⚠️ ONCE PER PROCESS. The engine's globals and the game module are not reset
// when NOLF1_Main returns, so after a game the app RESTARTS ITSELF (a tiny
// shell waits for this process to end, then `open`s the app with
// --after-game) and shows the result and the log as before. Cmd-Q while
// playing ends the game cleanly first and then quits (AppDelegate).
//
// Everything play.sh exported is set here, in this process's environment,
// before NOLF1_Main runs; nothing else is needed.
//
//   * game data      NOLF_GAME_DIR (GameInstall)
//   * game module    NOLF_SERVER = Contents/Frameworks/libnolfobject.dylib
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
    /// The game's module (its server side), inside this app. The engine itself
    /// is linked into the app.
    static var gameModule: URL {
        Bundle.main.bundleURL.appendingPathComponent("Contents/Frameworks/libnolfobject.dylib")
    }
}

/// The game, run in this process (NOLF1_Main, the bridging header).
enum InProcessGame {
    /// NOLF1_Main has been entered: it may not be entered again.
    private(set) static var used = false
    /// The game is running right now (Cmd-Q asks it to end first).
    private(set) static var running = false
    /// Cmd-Q came while it ran: quit when it is done, do not come back.
    static var quitWhenDone = false

    /// This process's LT_* / NOLF_* variables become exactly the game's.
    private static func applyEnvironment(_ env: [String: String]) {
        let game: (String) -> Bool = { $0.hasPrefix("LT_") || $0.hasPrefix("NOLF_") }
        for k in ProcessInfo.processInfo.environment.keys where game(k) { unsetenv(k) }
        for (k, v) in env where game(k) { setenv(k, v, 1) }
    }

    /// Runs the whole game, as `nolf1` would with this environment, in this
    /// directory, its output appended to `output`. `restoreOutput` puts this
    /// process's stdout/stderr back afterwards (the self-test prints after it).
    static func run(env: [String: String], in dir: URL, output: URL, restoreOutput: Bool) -> Int32 {
        precondition(!used, "the game runs once per process")
        used = true
        applyEnvironment(env)
        FileManager.default.changeCurrentDirectoryPath(dir.path)

        fflush(stdout); fflush(stderr)
        let savedOut = restoreOutput ? dup(STDOUT_FILENO) : -1
        let savedErr = restoreOutput ? dup(STDERR_FILENO) : -1
        let fd = Darwin.open(output.path, O_WRONLY | O_APPEND | O_CREAT, 0o644)
        if fd >= 0 { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); close(fd) }

        var argv: [UnsafeMutablePointer<CChar>?] = [strdup("NOLF1"), nil]
        running = true
        let rc = NOLF1_Main(1, &argv)
        running = false
        free(argv[0])

        fflush(stdout); fflush(stderr)
        if restoreOutput {
            if savedOut >= 0 { dup2(savedOut, STDOUT_FILENO); close(savedOut) }
            if savedErr >= 0 { dup2(savedErr, STDERR_FILENO); close(savedErr) }
        }
        return rc
    }

    /// After a game: a shell waits until this process has ended, then opens the
    /// app again with the result -- so the next game starts in a fresh process.
    static func restartLauncher(status: Int32, log: URL) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c",
                       "while /bin/kill -0 \"$1\" 2>/dev/null; do /bin/sleep 0.05; done; "
                       + "exec /usr/bin/open -n \"$2\" --args --after-game \"$3\" \"$4\"",
                       "sh", String(getpid()), Bundle.main.bundleURL.path, String(status), log.path]
        try? p.run()
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
        // The engine looks beside its executable -- this app's Contents/MacOS --
        // so it is told where the module really is.
        env["NOLF_SERVER"] = Paths.gameModule.path
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

    var isRunning: Bool { state == .running }

    init() {
        // Back from a game (InProcessGame.restartLauncher): its result and log.
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--after-game"), i + 2 < args.count {
            state = .ended(Int32(args[i + 1]) ?? 0)
            log = URL(fileURLWithPath: args[i + 2])
        }
    }

    func start(_ install: GameInstall, _ options: LaunchOptions) {
        guard !InProcessGame.used else { return }
        do {
            let fm = FileManager.default
            guard fm.fileExists(atPath: Paths.gameModule.path) else {
                state = .failed("The game module is missing from the app (\(Paths.gameModule.path)).")
                return
            }
            try fm.createDirectory(at: Paths.profile, withIntermediateDirectories: true)
            if options.mode == .mission { try Profile.unlock(upTo: options.mission, in: Paths.profile) }
            let dirs = try install.directoriesForEngine()
            let env = options.environment(gameDirs: dirs)
            let logURL = try Self.newLog()

            log = logURL
            state = .running
            // The launcher steps aside; the game opens its own window in this app.
            for w in NSApp.windows where w.isVisible { w.orderOut(nil) }

            // ⚠️ NOT INSIDE THE BUTTON'S ACTION: SwiftUI finishes this update
            // first, then the game's frame loop runs on the main thread -- it
            // pumps the app's events itself until its window closes.
            //
            // ⚠️ FROM A RUN-LOOP TIMER, NEVER FROM DispatchQueue.main.async
            // (user's bug 2026-11-21: the green button hung the game a while,
            // then no key reached it). GCD runs no other main-queue block while
            // one is running, so a game loop started INSIDE one starved the main
            // queue for the whole session, its own event pumping included --
            // and AppKit's fullscreen transition finishes on the main queue: it
            // timed out and left the window without the keyboard. Measured
            // (scratch mq.m): main-queue work queued during such a loop ran
            // NO from a main-queue block, YES from a timer. `nolf1` alone runs
            // the loop straight from main(), which is why it never had this.
            Timer.scheduledTimer(withTimeInterval: 0, repeats: false) { _ in
                let rc = InProcessGame.run(env: env, in: Paths.profile, output: logURL, restoreOutput: false)
                if !InProcessGame.quitWhenDone { InProcessGame.restartLauncher(status: rc, log: logURL) }
                exit(rc)
            }
        } catch {
            state = .failed(error.localizedDescription)
        }
    }

    /// Ends a running game as its close button would.
    func stop() { NOLF1_RequestQuit() }

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
