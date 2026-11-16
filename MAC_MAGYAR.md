# NOLF1 Macen: magyar fordítás + javítások (`mac-magyar` ág)

*2026-11-16. Erre az ágra került a magyar felirat, a Mac-es hibajavítások és a képjavítás.
Alapja: `linux-port` @ 00a55ff. A Linux-port ugyanebből a forrásból fordul, és ki van próbálva.*

## Mi van benne

| Mi | Fájlok |
|---|---|
| **Magyar szöveg és felirat.** 5450 sor van lefordítva, a nyelvi fájl `id TAB szöveg` formájú UTF-8. A motor a `NOLF_LANG_FILE` változóból tölti be. | `launcher/hu.txt`, `engine/kernel/res/ltstringmgr.*`, `ltperes.h` |
| **Ékezetes betűk** a bitmap fontokban (á é í ó ö ő ú ü ű és nagybetűik). | `engine/render/ltfont.*` |
| **Szebb, nagyobb feliratbetű** (csak Macen). A rendszerfontból élsimítva készül, a mérete a `NOLF_SUBSCALE` változóval állítható: alapból 1.5, 0 = az eredeti font. | `engine/platform/ltsysfont.mm`, `engine/libs/ltguimgr/ltgui_base.cpp` |
| A feliratok magyar nyelvnél alapból be vannak kapcsolva. | `NOLF/ClientShellDLL/Subtitle.cpp` |
| **Javítva:** régi mentés betöltésekor összeomlás (SIGBUS a `GetObjectColor`-ban). A mentett objektumhivatkozások most index szerint töltődnek vissza. | `engine/shared/ltmessage.*` |
| **Javítva:** a menüben megállt az egér. A pointert csak a játékban fogja meg, a menüben nem (`CursorCenter`). | `engine/render/ltnullcursor.*`, `engine/kernel/console/ltconsole.cpp` |
| **Javítva:** a leltárban nem lehetett a „Vissza” nyílra kattintani. | `NOLF/ClientShellDLL/BaseFolder.cpp` |
| **Javítva:** az egér-érzékenység csúszka nem csinált semmit. Az 5-ös állás a régi sebesség, a 0 körülbelül 0,2×, a 10 körülbelül 1,8×. | `engine/platform/ltinput.cpp` |
| **Képjavítás** (csak Macen, Metal): élesítés, színkorrekció és finom bloom, csak a 3D-s képen. A `NOLF_SHARPEN`, `NOLF_GRADE` és `NOLF_BLOOM` változók állítják (0..1), ha nincsenek megadva, ki van kapcsolva. | `engine/render/ltmetal.mm` |
| **Indító.** Két új kapcsoló: „Magyar szöveg és felirat” és „Képjavítás”. Mindkettő alapból be van kapcsolva. | `launcher/ContentView.swift`, `launcher/GameRunner.swift` |
| Teszthorog: `LT_TEST_MOUSE="frame:x:y,..."` kattint a megadott pontra. | `engine/platform/ltinput.cpp` |
| Segédprogram a CRES string-tábla kiírásához. | `harness/dumpstrings.cpp` |

**Gombok:** gyorsmentés **F6**, gyorsbetöltés **F9**. Mac-billentyűzeten **fn+F6** és **fn+F9**, ha a
Rendszerbeállításokban nincs bekapcsolva, hogy az F-gombok „normál funkcióbillentyűként” működjenek.

## Fordítás a munkahelyi Macen

Kell hozzá macOS 15 vagy újabb, Xcode 16 vagy újabb (itthon Xcode 27.2-vel fordult), git, hozzáférés a git tárolóhoz
és a saját NOLF GOTY játékmappa.

```sh
mkdir -p ~/Developer && cd ~/Developer
git clone -b mac-magyar <repo-URL> nolf1
cd nolf1
git log --oneline -1            # a legfelső commit: "Mac: magyar fordítás ..."

# Fordítás parancssorból (ad hoc aláírás, Apple ID nem kell):
xcodebuild -project NOLF1.xcodeproj -scheme "NOLF1 Launcher" -configuration Release \
  -derivedDataPath build-mac CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= \
  build 2>&1 | grep -E "error:|BUILD"
open build-mac/Build/Products/Release/NOLF1.app
```

Indítás után: **Choose…** → válaszd ki a játékmappát → kapcsolók → **Play**.

- Ha az `xcodebuild` azt írja, hogy *"tool 'xcodebuild' requires Xcode"*, akkor az Xcode nincs kiválasztva. Ez megoldja: `sudo xcode-select -s /Applications/Xcode.app`.
- Ha az Xcode-ot frissen telepítették vagy frissítették, előtte futtasd egyszer: `sudo xcodebuild -runFirstLaunch`.
- Grafikus felületről is fordíthatsz: `open NOLF1.xcodeproj`, aztán a „NOLF1 Launcher” séma, My Mac, majd ⌘B.
  Az aláírásnál a saját Team-edet vagy a „Sign to Run Locally” beállítást válaszd. Ezt a `project.pbxproj`-ban ne commitold.

### DMG készítése (a barátodnak)

```sh
mkdir -p /tmp/dmg && rm -rf /tmp/dmg/* && ditto build-mac/Build/Products/Release/NOLF1.app /tmp/dmg/NOLF1.app
ln -s /Applications /tmp/dmg/Applications
hdiutil create -volname "NOLF1 magyar" -srcfolder /tmp/dmg -format UDZO -ov NOLF1-magyar.dmg
```

A kész DMG a felhasználói leírással (`OLVASS EL.txt`) együtt fent van a szerveren:
(a saját szerveren).

## Linux (ellenőrizve, Debian trixie, clang 19, SDL3)

```sh
python3 engine/linux_case_links.py
NOLF_OBJ=build-linux/obj NOLF_BIN=build-linux/nolf1 ./engine/build_linux.sh
NOLF_LANG_FILE=$PWD/launcher/hu.txt NOLF_GAME_DIR=~/workspace/"No One Lives Forever" build-linux/nolf1   # magyarul
```

A Mac-only részek (`.mm`, rendszerfont, Metal szűrők) Linuxon nem fordulnak be. Ha a `NOLF_LANG_FILE` nincs
megadva, a játék angol, pontosan úgy, mint eddig.

## Hasznos

- Napló: `~/Library/Logs/NOLF1`. Beállítások és mentések: `~/Library/Application Support/NOLF1`.
- **Nyitott hiba:** a kártyás ajtók hangja néha később szólal meg. Még nincs meg, melyik pályán.
