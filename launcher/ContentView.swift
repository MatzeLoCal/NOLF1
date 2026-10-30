// ContentView.swift -- the launcher's one window.

import SwiftUI
import AppKit

struct ContentView: View {
    @AppStorage("gamePath") private var gamePath = ""
    @AppStorage("width") private var width = 1024
    @AppStorage("height") private var height = 768
    @AppStorage("software") private var software = false
    @AppStorage("mode") private var modeRaw = LaunchOptions.Mode.play.rawValue
    @AppStorage("mission") private var mission = 1
    @AppStorage("level") private var level = "M01S01"
    @AppStorage("trace") private var trace = false
    @AppStorage("bubbles") private var bubbles = true
    @AppStorage("bounce") private var bounce = true

    @StateObject private var runner = GameRunner()
    @State private var install: GameInstall?
    @State private var showAdvanced: Bool

    init(showAdvanced: Bool = false) { _showAdvanced = State(initialValue: showAdvanced) }
    @State private var confirmReset = false
    @State private var message: String?

    private static let sizes: [(Int, Int)] = [(1024, 768), (1280, 720), (1440, 900),
                                              (1600, 900), (1920, 1080), (2560, 1440)]

    private var mode: LaunchOptions.Mode { LaunchOptions.Mode(rawValue: modeRaw) ?? .play }

    var body: some View {
        VStack(alignment: .leading, spacing: 18) {
            header
            gameFolder
            Divider()
            playRow
            displayRow
            // Full width, flush left: a DisclosureGroup centres content narrower
            // than itself, so short captions pulled the whole block to the middle.
            DisclosureGroup("Advanced", isExpanded: $showAdvanced) {
                advanced.padding(.top, 8).frame(maxWidth: .infinity, alignment: .leading)
            }
            Spacer(minLength: 0)
            footer
        }
        .padding(24)
        .frame(width: 768)
        .frame(minHeight: 460)
        .background(Image("LauncherBackground2")
            .resizable()
            .scaledToFill())
        .onAppear(perform: rescan)
        .onChange(of: gamePath) { _, _ in rescan() }
        .confirmationDialog("Start with a fresh profile?", isPresented: $confirmReset) {
            Button("Move saves aside") { resetProfile() }
        } message: {
            Text("All saved games and mission progress are moved to \"Profile Backups\" in Application Support. Nothing is deleted.")
        }

    }

    // MARK: - parts

    private var header: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("No One Lives Forever").font(.largeTitle.bold())
            Text("macOS port — launcher").foregroundStyle(.secondary)
        }
    }

    private var gameFolder: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("Game data").font(.headline)
            HStack {
                Text(gamePath.isEmpty ? "No folder chosen" : gamePath)
                    .lineLimit(1).truncationMode(.middle)
                    .foregroundStyle(gamePath.isEmpty ? .secondary : .primary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                Button("Choose…", action: chooseFolder)
            }
            if gamePath.isEmpty {
                Text("Point to the folder of your No One Lives Forever installation, or to a folder holding the copied game discs. The game data is not part of this app.")
                    .font(.callout).foregroundStyle(.secondary)
            } else if let gi = install {
                if gi.usable {
                    Label("Game of the Year data found.", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    ForEach(gi.notes, id: \.self) { Label($0, systemImage: "info.circle").foregroundStyle(.secondary) }
                } else {
                    ForEach(gi.problems, id: \.self) { Label($0, systemImage: "xmark.octagon.fill").foregroundStyle(.red) }
                }
            }
        }
    }

    private var playRow: some View {
        HStack(spacing: 12) {
            Button(action: play) {
                Text(runner.isRunning ? "Running…" : playTitle)
                    .font(.title2.bold()).frame(minWidth: 180)
            }
            .controlSize(.large)
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
            .disabled(runner.isRunning || !(install?.usable ?? false))
            if runner.isRunning {
                Button("Quit game", role: .destructive) { runner.stop() }
            }
        }
    }

    private var playTitle: String {
        switch mode {
        case .play:    return "Play"
        case .mission: return "Play (missions unlocked)"
        case .viewer:  return "Open level viewer"
        }
    }

    // The fixed sizes plus the stored one when it is not among them: a size
    // chosen in the game's Options > Display is written back here (HANDOVER
    // 5dw), and a Picker shows nothing for a selection it has no row for.
    private var sizeChoices: [String] {
        var all = Self.sizes
        if !all.contains(where: { $0.0 == width && $0.1 == height }) { all.append((width, height)) }
        all.sort { ($0.0 * $0.1, $0.0) < ($1.0 * $1.1, $1.0) }
        return all.map { "\($0.0)x\($0.1)" }
    }

    private var displayRow: some View {
        Picker("Window size", selection: Binding(
            get: { "\(width)x\(height)" },
            set: { v in let p = v.split(separator: "x"); width = Int(p[0]) ?? 1024; height = Int(p[1]) ?? 768 })) {
            ForEach(sizeChoices, id: \.self) { t in
                let p = t.split(separator: "x")
                Text(verbatim: "\(p[0]) × \(p[1])").tag(t)
            }
        }
        .frame(maxWidth: 300)
    }

    private var advanced: some View {
        VStack(alignment: .leading, spacing: 12) {
            Picker("Renderer", selection: $software) {
                Text("Metal (fast)").tag(false)
                Text("Software").tag(true)
            }
            .pickerStyle(.segmented).frame(maxWidth: 320)

            Picker("Start", selection: $modeRaw) {
                Text("The game's menus").tag(LaunchOptions.Mode.play.rawValue)
                Text("Unlock missions up to…").tag(LaunchOptions.Mode.mission.rawValue)
                Text("Level viewer (walk around, no game)").tag(LaunchOptions.Mode.viewer.rawValue)
            }
            .frame(maxWidth: 420)

            if mode == .mission {
                Picker("Mission", selection: $mission) {
                    ForEach(Missions.all) { m in Text("\(m.index). \(m.title)").tag(m.index) }
                }
                .frame(maxWidth: 420)
                Text("Raises the game's own progress so Single Player › Select mission offers it.")
                    .font(.callout).foregroundStyle(.secondary)
            }
            if mode == .viewer {
                Picker("Level", selection: $level) {
                    ForEach(Missions.all) { m in
                        Section(m.title) { ForEach(m.levels, id: \.self) { Text($0).tag($0) } }
                    }
                }
                .frame(maxWidth: 420)
            }

            VStack(alignment: .leading, spacing: 4) {
                Toggle("Bubbles in aquariums", isOn: $bubbles)
                Text("Off: as in the original at today's frame rates — no bubbles at all.")
                    .font(.callout).foregroundStyle(.secondary)
            }

            VStack(alignment: .leading, spacing: 4) {
                Toggle("Sparks bounce off floors", isOn: $bounce)
                Text("Sparks and blood drops. Off: as in the original — they fall through.")
                    .font(.callout).foregroundStyle(.secondary)
            }

            Toggle("Write a detailed log (for bug reports)", isOn: $trace)

            HStack {
                Button("Fresh profile…") { confirmReset = true }.disabled(runner.isRunning)
                Button("Show saves") { reveal(Paths.profile) }
                Button("Show logs") { reveal(Paths.logs) }
            }
        }
    }

    private var footer: some View {
        Group {
            switch runner.state {
            case .idle:
                Text(message ?? " ")
            case .running:
                Text("The game is running.")
            case .ended(let code):
                Text(code == 0 ? "The game ended normally." : "The game ended with status \(code).")
            case .crashed(let sig):
                HStack {
                    Text("The game crashed (signal \(sig)).").foregroundStyle(.red)
                    if let l = runner.log { Button("Show log") { NSWorkspace.shared.activateFileViewerSelecting([l]) } }
                }
            case .failed(let why):
                Text(why).foregroundStyle(.red)
            }
        }
        .font(.callout)
        .foregroundStyle(.secondary)
    }

    // MARK: - actions

    private func rescan() {
        install = gamePath.isEmpty ? nil : GameInstall.scan(URL(fileURLWithPath: gamePath))
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Use this folder"
        panel.message = "Choose your No One Lives Forever folder"
        if panel.runModal() == .OK, let url = panel.url { gamePath = url.path }
    }

    private func play() {
        guard let gi = install, gi.usable else { return }
        var o = LaunchOptions()
        o.mode = mode; o.width = width; o.height = height; o.software = software
        o.mission = mission; o.level = level; o.trace = trace; o.bubbles = bubbles; o.bounce = bounce
        message = nil
        runner.start(gi, o)
    }

    private func resetProfile() {
        do {
            if let dest = try Profile.reset() { message = "Saves moved to \(dest.lastPathComponent) in Profile Backups." }
            else { message = "The profile was already fresh." }
        } catch { message = "Could not reset the profile: \(error.localizedDescription)" }
    }

    private func reveal(_ url: URL) {
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
        NSWorkspace.shared.open(url)
    }
}
