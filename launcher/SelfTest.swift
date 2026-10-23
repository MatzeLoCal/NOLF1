// SelfTest.swift -- `NOLF1.app/Contents/MacOS/NOLF1 --selftest <game folder>`
//
// Checks the launcher's whole path without a window: scans the folder the way
// the Choose button does, prints what was found, then starts the game that
// is inside THIS app headless -- a level loaded straight away, a few hundred
// frames, no window -- and reports whether the engine mounted the data and
// loaded the server. A headless run reads and writes no settings, and its
// saves go to a temporary profile, so the player's are not touched.
// Exit status 0 = everything worked.

import Foundation

enum SelfTest {
    static func run(folder: String) -> Int32 {
        guard !folder.isEmpty else {
            print("usage: NOLF1 --selftest <game folder>")
            return 2
        }
        let gi = GameInstall.scan(URL(fileURLWithPath: folder))
        print("game folder: \(gi.root.path)")
        print("startup.txt: \(gi.startupFile?.path ?? "(none -- the standard list is used)")")
        for d in gi.dirs { print("  data dir: \(d.path)") }
        for t in gi.trees { print("  \(t.found == nil ? "missing" : "found  ") \(t.name)\(t.found.map { "  (\($0.path))" } ?? "")") }
        for p in gi.problems { print("PROBLEM: \(p)") }
        for n in gi.notes { print("note: \(n)") }
        guard gi.usable else { return 1 }

        let exe = Paths.gameExecutable
        print("game: \(exe.path)")
        guard FileManager.default.isExecutableFile(atPath: exe.path) else { print("PROBLEM: the game is missing"); return 1 }

        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("nolf1-selftest-\(getpid())", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tmp) }

        let dirs: [URL]
        do { dirs = try gi.directoriesForEngine() } catch { print("PROBLEM: \(error)"); return 1 }
        var env = LaunchOptions().environment(gameDirs: dirs, headless: true, updates: 400)
        // A level loaded at once, through the console -- play.sh's `game` mode.
        env["LT_SERVER"] = "1"
        env["LT_CONSOLE"] = "Record Worlds\\M01S01 demo"

        let out = tmp.appendingPathComponent("run.log")
        FileManager.default.createFile(atPath: out.path, contents: nil)
        let p = Process()
        p.executableURL = exe
        p.currentDirectoryURL = tmp
        p.environment = env
        do {
            let h = try FileHandle(forWritingTo: out)
            p.standardOutput = h; p.standardError = h
            try p.run()
            p.waitUntilExit()
            try? h.close()
        } catch { print("PROBLEM: could not start the game: \(error)"); return 1 }

        let log = (try? String(contentsOf: out, encoding: .utf8)) ?? (try? String(contentsOf: out, encoding: .isoLatin1)) ?? ""
        func line(_ s: String) -> String? { log.components(separatedBy: "\n").first { $0.contains(s) } }
        let mounted = line("mounted ")
        let server = line("[SERVER] loaded")
        let world = line("LoadWorld(")
        print("exit: \(p.terminationReason == .uncaughtSignal ? "signal" : "status") \(p.terminationStatus)")
        print("engine: \(mounted ?? "(no 'mounted' line)")")
        print("server: \(server ?? "(not loaded)")")
        print("world:  \(world ?? "(no world loaded)")")
        let ok = p.terminationReason == .exit && p.terminationStatus == 0
            && (mounted.map { !$0.contains("mounted 0 ") } ?? false) && server != nil && world != nil
        print(ok ? "SELFTEST PASSED" : "SELFTEST FAILED")
        return ok ? 0 : 1
    }
}
