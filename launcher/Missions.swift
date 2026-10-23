// Missions.swift -- NOLF's missions and their levels, in the game's own order
// (Attributes\Missions.txt; titles from its CRES.DLL string table, ids
// 2500..2524 and 30000). Used by the advanced options only: the game's own
// menus are how a mission is normally started.

struct Mission: Identifiable {
    let index: Int
    let title: String
    let levels: [String]
    var id: Int { index }
}

enum Missions {
    static let all: [Mission] = [
        Mission(index: 0,  title: "The Assignment",                       levels: ["T01S01", "T01S02", "T01S03"]),
        Mission(index: 1,  title: "Misfortune in Morocco",                levels: ["M01S01", "M01S02", "M01S03", "M01S04"]),
        Mission(index: 2,  title: "Requiem for a Spy",                    levels: ["T02S01"]),
        Mission(index: 3,  title: "Berlin by Night",                      levels: ["M02S01", "M02S02", "M02S03"]),
        Mission(index: 4,  title: "Unexpected Turbulence",                levels: ["M03S01", "M03S02"]),
        Mission(index: 5,  title: "Care to Explain?",                     levels: ["T03S01"]),
        Mission(index: 6,  title: "Rendezvous in Hamburg",                levels: ["M04S01", "M04S02"]),
        Mission(index: 7,  title: "Visit to Santa's Workshop",            levels: ["T04S01"]),
        Mission(index: 8,  title: "A Tenuous Lead",                       levels: ["M05S01", "M05S02", "M05S03", "M05S04", "M05S05"]),
        Mission(index: 9,  title: "H.A.R.M.'s Promise",                   levels: ["T05S01"]),
        Mission(index: 10, title: "The Dive",                             levels: ["M06S01", "M06S02", "M06S03"]),
        Mission(index: 11, title: "If Our Demands Are Not Met",           levels: ["T06S01"]),
        Mission(index: 12, title: "A Man of Influence",                   levels: ["M07S01", "M07S02", "M07S03"]),
        Mission(index: 13, title: "Further Investigation Required",       levels: ["T07S01"]),
        Mission(index: 14, title: "Safecracker",                          levels: ["M08S01", "M08S02", "M08S03", "M08S04", "M08S05", "M08S06"]),
        Mission(index: 15, title: "An Ounce of Hope, A Pound of Despair", levels: ["T08S01"]),
        Mission(index: 16, title: "Rescue Attempt",                       levels: ["M09S01", "M09S02", "M09S03"]),
        Mission(index: 17, title: "A Stern Warning",                      levels: ["T09S01"]),
        Mission(index: 18, title: "Trouble in the Tropics",               levels: ["M10S01", "M10S02", "M10S03", "M10S04"]),
        Mission(index: 19, title: "Low Earth Orbit",                      levels: ["M11S01", "M11S02"]),
        Mission(index: 20, title: "Good Luck and God Speed",              levels: ["T10S01"]),
        Mission(index: 21, title: "Alpine Intrigue",                      levels: ["M12S01", "M12S02", "M12S03", "M12S04"]),
        Mission(index: 22, title: "The Indomitable Cate Archer",          levels: ["M13S01", "M13S02", "M13S03", "M13S04"]),
        Mission(index: 23, title: "A Very Large Explosion",               levels: ["M14S01", "M14S02"]),
        Mission(index: 24, title: "Such is the Nature of Revenge",        levels: ["M15S01", "M15S02"]),
        Mission(index: 25, title: "Rest and Relaxation",                  levels: ["M16S01", "M16S02", "M16S03", "M16S04"]),
    ]

    static var allLevels: [String] { all.flatMap { $0.levels } }
}
