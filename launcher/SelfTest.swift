// SelfTest.swift -- `NOLF1.app/Contents/MacOS/NOLF1 --selftest <game folder>`
//
// Checks the launcher's whole path without a window: scans the folder the way
// the Choose button does, prints what was found, then runs the game that is
// linked into THIS app, in this process, headless -- a level loaded straight
// away, a few hundred frames, no window -- and reports whether the engine mounted the data and
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

        // The engine is linked into this app; only the game module is a file.
        let module = Paths.gameModule
        print("game: in this app's process; module \(module.path)")
        guard FileManager.default.fileExists(atPath: module.path) else { print("PROBLEM: the game module is missing"); return 1 }

        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("nolf1-selftest-\(getpid())", isDirectory: true)
        try? FileManager.default.createDirectory(at: tmp, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: tmp) }

        let dirs: [URL]
        do { dirs = try gi.directoriesForEngine() } catch { print("PROBLEM: \(error)"); return 1 }
        var env = LaunchOptions().environment(gameDirs: dirs, headless: true, updates: 400)
        // A level loaded at once, through the console -- play.sh's `game` mode.
        env["LT_SERVER"] = "1"
        env["LT_CONSOLE"] = "Record Worlds\\M01S01 demo"

        // ⚠️ IN THIS PROCESS, as Play runs it (GameRunner): the engine linked
        // into the app, NOLF1_Main called directly, its output to run.log. A
        // crash ends the self-test too (no "PASSED" line, a non-zero exit).
        let out = tmp.appendingPathComponent("run.log")
        FileManager.default.createFile(atPath: out.path, contents: nil)
        let cwd = FileManager.default.currentDirectoryPath
        let status = InProcessGame.run(env: env, in: tmp, output: out, restoreOutput: true)
        FileManager.default.changeCurrentDirectoryPath(cwd)

        let log = (try? String(contentsOf: out, encoding: .utf8)) ?? (try? String(contentsOf: out, encoding: .isoLatin1)) ?? ""
        func line(_ s: String) -> String? { log.components(separatedBy: "\n").first { $0.contains(s) } }
        let mounted = line("mounted ")
        let server = line("[SERVER] loaded")
        let world = line("LoadWorld(")
        print("exit: status \(status)")
        print("engine: \(mounted ?? "(no 'mounted' line)")")
        print("server: \(server ?? "(not loaded)")")
        print("world:  \(world ?? "(no world loaded)")")
        let ok = status == 0
            && (mounted.map { !$0.contains("mounted 0 ") } ?? false) && server != nil && world != nil
        // A failure keeps the game's log: the temporary profile is deleted.
        if !ok {
            let f = DateFormatter(); f.dateFormat = "yyyyMMdd-HHmmss"
            let dest = Paths.logs.appendingPathComponent("selftest-failed-\(f.string(from: Date())).log")
            try? FileManager.default.createDirectory(at: Paths.logs, withIntermediateDirectories: true)
            if (try? FileManager.default.copyItem(at: out, to: dest)) != nil { print("log:    \(dest.path)") }
        }
        print(ok ? "SELFTEST PASSED" : "SELFTEST FAILED")
        return ok ? 0 : 1
    }
}
