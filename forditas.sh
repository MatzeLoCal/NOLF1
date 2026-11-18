#!/bin/bash
# ----------------------------------------------------------------------- //
# NOLF1 Linux -- fordítás egy paranccsal (magyar felirattal).
#
#   ./forditas.sh            fordítás (csak a változott fájlok)
#   ./forditas.sh tiszta     teljes újrafordítás (a régi objektumok törlése)
#   ./forditas.sh ellenoriz  csak a függőségek ellenőrzése
#   ./forditas.sh ikon       menü- és asztali ikon létrehozása/frissítése
#
# Internet NEM kell hozzá, ha a csomagok (clang, libsdl3-dev, pkg-config,
# python3) már fent vannak. Részletek: OLVASS_EL_FORDITAS.md
# ----------------------------------------------------------------------- //
set -u
R="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$R"

zold()  { printf '\033[32m%s\033[0m\n' "$*"; }
piros() { printf '\033[31m%s\033[0m\n' "$*"; }

ellenoriz() {
    local hiba=0
    for p in clang clang++ pkg-config python3; do
        if command -v "$p" >/dev/null 2>&1; then zold "  OK   $p"; else piros "  HIÁNYZIK  $p"; hiba=1; fi
    done
    if pkg-config --exists sdl3 2>/dev/null; then
        zold "  OK   SDL3 $(pkg-config --modversion sdl3)"
    else
        piros "  HIÁNYZIK  SDL3 fejlesztői csomag (libsdl3-dev)"; hiba=1
    fi
    if [ -f "$R/launcher/hu.txt" ]; then zold "  OK   magyar nyelvi fájl (launcher/hu.txt)"
    else piros "  HIÁNYZIK  launcher/hu.txt -- rossz ágon vagy? (git checkout linux-port)"; hiba=1; fi
    if [ $hiba -ne 0 ]; then
        echo
        piros "Hiányzó csomagok. Telepítés (ehhez internet kell):"
        echo  "  sudo apt install clang pkg-config python3 libsdl3-dev"
        return 1
    fi
}

ikon() {
    local app="$HOME/.local/share/applications/nolf1.desktop"
    local ico="$HOME/.local/share/NOLF1/nolf1.png"
    mkdir -p "$(dirname "$app")"
    cat > "$app" <<EOF
[Desktop Entry]
Type=Application
Name=No One Lives Forever
Comment=NOLF1 Linux port (magyar felirattal)
Exec=$R/play_linux.sh
Path=$R
Icon=$ico
Terminal=false
Categories=Game;ActionGame;
StartupWMClass=nolf1
EOF
    chmod +x "$app"
    local desk; desk="$(xdg-user-dir DESKTOP 2>/dev/null)"
    if [ -n "$desk" ] && [ -d "$desk" ]; then cp "$app" "$desk/" && chmod +x "$desk/nolf1.desktop"; fi
    zold "Ikon kész: $app"
    [ -f "$ico" ] || echo "(Az ikonkép nincs meg: $ico -- az ikon kép nélkül jelenik meg.)"
}

case "${1:-}" in
    ellenoriz) ellenoriz; exit $? ;;
    ikon)      ikon; exit 0 ;;
    ""|tiszta) ;;
    *) echo "Ismeretlen kapcsoló: $1   (használat: ./forditas.sh [tiszta|ellenoriz|ikon])"; exit 2 ;;
esac

echo "== 1/3 Függőségek ellenőrzése"
ellenoriz || exit 1

echo "== 2/3 Kis-/nagybetűs include linkek (egyszer kell, de ártani nem árt)"
python3 engine/linux_case_links.py || { piros "A linkek létrehozása nem sikerült."; exit 1; }

echo "== 3/3 Fordítás (pár perc, a gép összes magját használja)"
mkdir -p build-linux
ARG=""; [ "${1:-}" = "tiszta" ] && ARG="clean"
LOG="$R/build-linux/forditas.log"
if NOLF_OBJ="$R/build-linux/obj" NOLF_BIN="$R/build-linux/nolf1" ./engine/build_linux.sh $ARG >"$LOG" 2>&1 \
   && [ -x build-linux/nolf1 ] && [ -f build-linux/libnolfobject.so ]; then
    zold "KÉSZ: build-linux/nolf1 + build-linux/libnolfobject.so"
    echo "Indítás: ./play_linux.sh        (angolul: NOLF_LANG=en ./play_linux.sh)"
else
    piros "HIBA a fordításban. Az első hibák:"
    grep -m 15 -E "error:|Error|undefined reference" "$LOG"
    echo "Teljes napló: $LOG"
    exit 1
fi
