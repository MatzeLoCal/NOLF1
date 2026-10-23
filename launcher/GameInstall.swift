// GameInstall.swift -- finds a No One Lives Forever install in the folder the
// player chose, and turns it into what the engine needs: NOLF_GAME_DIR, a
// colon-separated list of directories holding startup.txt and the .rez
// archives (engine: skeleton_host.cpp, CLTFileMgr::InitFromGameDirs).
//
// Two layouts are known:
//   * an INSTALLED game (Windows install folder copied over): everything in
//     one directory;
//   * the GOTY DISCS copied over: startup.txt and autoexec.cfg in
//     disc2/Game, the archives spread over disc1/Data and disc2/Data.
// The Retail discs carry no startup.txt at all (the Windows installer wrote
// it), so when none is found the standard list is written into Application
// Support and handed to the engine first.

import Foundation

struct GameInstall {
    struct Tree: Identifiable {
        let name: String
        let found: URL?
        var id: String { name }
    }

    let root: URL
    var dirs: [URL] = []            // handed to the engine, in this order
    var startupFile: URL?           // nil -> the standard list is used
    var trees: [Tree] = []
    var problems: [String] = []     // any of these blocks Play
    var notes: [String] = []        // worth knowing, not blocking

    var usable: Bool { problems.isEmpty && !dirs.isEmpty }
    var foundCount: Int { trees.filter { $0.found != nil }.count }

    /// The archive list of NOLF Game of the Year (its own startup.txt).
    static let standardTrees = ["nolf", "nolf.rez", "nolf2.rez", "nolfdll.rez", "nolfl.rez",
                                "nolfu003.rez", "nolfc001.rez", "nolfc002.rez", "nolfgoty.rez"]

    /// The directories the engine is given, as NOLF_GAME_DIR.
    var gameDirValue: String { dirs.map { $0.path }.joined(separator: ":") }

    static func scan(_ root: URL, maxDepth: Int = 4) -> GameInstall {
        var gi = GameInstall(root: root)
        let fm = FileManager.default

        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: root.path, isDirectory: &isDir), isDir.boolValue else {
            gi.problems.append("The folder does not exist any more.")
            return gi
        }

        // Every directory (to maxDepth) holding a .rez archive, startup.txt or
        // autoexec.cfg -- names compared without case: the discs say NOLF.REZ,
        // startup.txt says nolf.rez.
        var dataDirs: [URL] = []
        var startup: URL?
        var autoexec: URL?
        func visit(_ dir: URL, _ depth: Int) {
            guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.isDirectoryKey],
                                                          options: [.skipsHiddenFiles, .skipsPackageDescendants]) else { return }
            var isData = false
            for item in items {
                let name = item.lastPathComponent.lowercased()
                let sub = (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
                if sub {
                    if depth < maxDepth { visit(item, depth + 1) }
                    continue
                }
                if name.hasSuffix(".rez") { isData = true }
                if name == "startup.txt" { isData = true; if startup == nil { startup = item } }
                if name == "autoexec.cfg" { isData = true; if autoexec == nil { autoexec = item } }
            }
            if isData { dataDirs.append(dir) }
        }
        visit(root, 0)

        guard !dataDirs.isEmpty else {
            gi.problems.append("No game data in this folder: no .rez archives were found. Choose the folder your No One Lives Forever installation (or the copied game discs) lives in.")
            return gi
        }

        // Two installations under one folder would be mixed into one game.
        for key in ["nolf.rez", "nolf2.rez", "startup.txt", "autoexec.cfg"] {
            let holders = dataDirs.filter { d in
                ((try? fm.contentsOfDirectory(atPath: d.path)) ?? []).contains { $0.lowercased() == key }
            }
            if holders.count > 1 {
                gi.problems.append("This folder holds more than one copy of the game (\(key.uppercased()) in \(holders.map { Self.relative($0, to: root) }.joined(separator: " and "))). Choose the folder of ONE installation.")
                return gi
            }
        }

        // The archive list: the install's own startup.txt, else the standard one.
        var names = standardTrees
        if let s = startup, let list = Self.readStartup(s), !list.isEmpty {
            names = list
            gi.startupFile = s
        } else {
            gi.notes.append("No startup.txt was found (the Retail discs have none), so the standard Game of the Year archive list is used.")
        }

        // Order: the directory with startup.txt first, then the rest by path --
        // the engine's own default for the GOTY discs comes out the same.
        let first = startup?.deletingLastPathComponent().standardizedFileURL
        dataDirs.sort { a, b in
            let fa = a.standardizedFileURL == first, fb = b.standardizedFileURL == first
            if fa != fb { return fa }
            return a.path < b.path
        }
        gi.dirs = dataDirs

        // Where each archive is (a file, or a loose directory such as "nolf").
        for name in names {
            var hit: URL?
            for d in dataDirs {
                if let items = try? fm.contentsOfDirectory(atPath: d.path),
                   let m = items.first(where: { $0.lowercased() == name.lowercased() }) {
                    hit = d.appendingPathComponent(m); break
                }
            }
            gi.trees.append(Tree(name: name, found: hit))
        }

        func has(_ n: String) -> Bool { gi.trees.contains { $0.name.lowercased() == n && $0.found != nil } }
        if !has("nolf.rez") {
            gi.problems.append("NOLF.REZ, the main game archive, was not found.")
        }
        if !has("nolf2.rez") {
            gi.problems.append("NOLF2.REZ was not found. If you copied the game discs, copy BOTH discs into this folder.")
        }
        if autoexec == nil {
            gi.problems.append("autoexec.cfg was not found -- it holds the game's default controls (it is in the Game folder of the installation or disc).")
        }
        // ⚠️ Measured 2026-10-18: the Retail discs (no patch, no GOTY archive)
        // mount and then crash before the first level -- this port's game code
        // is the Game of the Year release, and it needs that data.
        if !has("nolfgoty.rez") {
            gi.problems.append("This is not the Game of the Year edition (NOLFGOTY.REZ was not found). The macOS port needs the Game of the Year data; the original release's data makes the game crash.")
        }
        // The engine's limits: ':' separates directories, 16 of them, 2047 characters.
        if gi.dirs.contains(where: { $0.path.contains(":") }) {
            gi.problems.append("A folder name contains a colon (\"/\" in Finder). Rename it, please -- the game cannot read such a path.")
        }
        if gi.dirs.count > 15 || gi.gameDirValue.count > 1900 {
            gi.problems.append("The game data is spread over too many folders. Choose the installation folder itself rather than a folder above it.")
        }
        return gi
    }

    /// `dir` as a path below `root`, for messages.
    static func relative(_ dir: URL, to root: URL) -> String {
        let r = root.standardizedFileURL.path, d = dir.standardizedFileURL.path
        return d.hasPrefix(r + "/") ? String(d.dropFirst(r.count + 1)) : dir.lastPathComponent
    }

    /// The [Rez] section of a startup.txt: RezN = "name", in N order.
    static func readStartup(_ url: URL) -> [String]? {
        guard let data = try? Data(contentsOf: url),
              let text = String(data: data, encoding: .isoLatin1) else { return nil }
        var inRez = false
        var entries: [(Int, String)] = []
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("[") { inRez = line.lowercased() == "[rez]"; continue }
            guard inRez, line.lowercased().hasPrefix("rez"),
                  let eq = line.firstIndex(of: "=") else { continue }
            let key = line[line.index(line.startIndex, offsetBy: 3)..<eq].trimmingCharacters(in: .whitespaces)
            let value = line[line.index(after: eq)...].trimmingCharacters(in: .whitespaces)
                .trimmingCharacters(in: CharacterSet(charactersIn: "\""))
            if let n = Int(key), !value.isEmpty { entries.append((n, value)) }
        }
        return entries.sorted { $0.0 < $1.0 }.map { $0.1 }
    }

    /// When the install has no startup.txt: the standard list, written where
    /// the engine will find it (handed to it as the first directory).
    func directoriesForEngine() throws -> [URL] {
        guard startupFile == nil else { return dirs }
        let dir = Paths.startupList
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var text = "[Rez]\r\n"
        for (i, n) in Self.standardTrees.enumerated() { text += "Rez\(i) = \"\(n)\"\r\n" }
        try text.write(to: dir.appendingPathComponent("startup.txt"), atomically: true, encoding: .isoLatin1)
        return [dir] + dirs
    }
}
