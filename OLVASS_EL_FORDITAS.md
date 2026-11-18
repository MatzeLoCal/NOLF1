# NOLF1 Linuxon – fordítás és indítás (magyar felirattal)

Ez a leírás internet nélkül is elég a játék újrafordításához, ha a szükséges csomagok
már fent vannak a gépen. Röviden:

```sh
cd ~/workspace/nolf1
./forditas.sh          # fordítás
./play_linux.sh        # indítás, magyarul
```

---

## 1. Mi kell hozzá

| Mi | Honnan | Megjegyzés |
|---|---|---|
| **Forráskód**, `linux-port` ág | GitLab vagy a saját git szerver (lásd 6. pont) | Mindkettőn ugyanaz van |
| **clang / clang++** | `sudo apt install clang` | Ezen a gépen clang 21 van |
| **SDL3 fejlesztői csomag** | `sudo apt install libsdl3-dev` | Ebből lesz az ablak, a hang és a bevitel |
| **pkg-config, python3** | `sudo apt install pkg-config python3` | Általában már fent vannak |
| **A játék adatai** (NOLF GOTY) | `~/workspace/No One Lives Forever` | Telepített játékmappa (`NOLF.REZ` stb.) |
| **Zenei hangbank** (nem kötelező) | `~/.local/share/NOLF1/GeneralUser-GS.sf2` | Enélkül néhány hangszer hiányzik a zenéből |

**Offline használat:** az `apt install`-hoz internet kell. Ha a csomagok már egyszer
fent vannak, a fordítás és a játék is internet nélkül megy. Az ellenőrzés:

```sh
./forditas.sh ellenoriz
```

Ez csupa zöld „OK”-t ír, ha minden megvan.

Ha előre szeretnél készülni egy gép újratelepítésére, töltsd le a csomagokat egy mappába, amíg van net:

```sh
mkdir -p ~/nolf-csomagok && cd ~/nolf-csomagok
apt download clang libsdl3-dev pkg-config     # (a függőségeik nélkül!)
```

Biztosabb, ha a gépről nem törlöd a csomagokat. Az `apt autoremove` nem veszi le őket,
mert kézzel lettek telepítve.

---

## 2. Fordítás

```sh
cd ~/workspace/nolf1
./forditas.sh            # csak a változott fájlokat fordítja (gyors)
./forditas.sh tiszta     # mindent elölről fordít (pár perc)
```

A script lépései:
1. ellenőrzi a függőségeket;
2. létrehozza a kis- és nagybetűs include-linkeket (`engine/linux_case_links.py`);
3. lefordítja a játékot. Az eredmény: `build-linux/nolf1` és `build-linux/libnolfobject.so`.

A fordítási napló a `build-linux/forditas.log` fájlba kerül. Hiba esetén a script az első
hibasorokat ki is írja.

**Kézzel, a script nélkül**, ugyanez:

```sh
python3 engine/linux_case_links.py
NOLF_OBJ=$PWD/build-linux/obj NOLF_BIN=$PWD/build-linux/nolf1 ./engine/build_linux.sh
```

---

## 3. Indítás

```sh
./play_linux.sh               # magyarul, 1024x768
./play_linux.sh 1920x1080     # más felbontással
NOLF_LANG=en ./play_linux.sh  # eredeti angol szöveggel
```

- A **magyar szöveg és a felirat** alapból be van kapcsolva. A nyelvi fájl:
  `launcher/hu.txt`, sorai `azonosító TAB szöveg` formájúak, UTF-8 kódolással.
  Ha belejavítasz, újrafordítás nem kell, elég újraindítani a játékot.
- Ha a játékmappa máshol van: `NOLF_GAME_DIR=/út/a/játékhoz ./play_linux.sh`
- Teljes képernyő: **Alt+Enter**. Gyorsmentés: **F6**, gyorsbetöltés: **F9**.

**Menüikon:** a „No One Lives Forever” ikon a menüben és az Asztalon is a `play_linux.sh`-t
indítja. Ha elveszne, vagy máshová kerül a forrás:

```sh
./forditas.sh ikon
```

### Hasznos kapcsolók

| Változó | Hatás |
|---|---|
| `NOLF_LANG=en` | Angol szöveg (a magyar helyett) |
| `NOLF_SUBSCALE=0` | Eredeti, kisebb bitmapes feliratbetű (alapból 1.5× élsimított) |
| `LT_SOFTWARE=1` | Szoftveres megjelenítés OpenGL helyett (ha a videókártya-driver gondot okoz) |
| `LT_NOVSYNC=1` | Vsync ki |
| `LT_MSAA=n` | Élsimítás a 3D képen (alapból 4) |
| `LT_GMBANK=<fájl>` | Másik zenei hangbank (.sf2) |

---

## 4. Ha valami nem megy

| Jelenség | Megoldás |
|---|---|
| `HIÁNYZIK SDL3` | `sudo apt install libsdl3-dev` (ehhez net kell) |
| Nincs magyar szöveg | Jó ágon vagy? `git branch` → `linux-port` kell, és létezzen a `launcher/hu.txt`. Ha mégis angol, nézd meg, nincs-e beállítva a `NOLF_LANG=en` |
| Fordítási hiba furcsa include-dal | `./forditas.sh tiszta` (a linkeket is újracsinálja) |
| Fekete kép vagy összeomlás induláskor | Próbáld meg így: `LT_SOFTWARE=1 ./play_linux.sh`. A naplót a terminálban látod |
| Hiányzik a zene egy része | Tedd a hangbankot ide: `~/.local/share/NOLF1/GeneralUser-GS.sf2` |
| Mentések, beállítások | `~/.local/share/NOLF1/` |

**Fontos:** a `linux_case_links.py` kb. 50 `.h` symlinket hoz létre (ezek jelennek meg
`??`-ként a `git status`-ban). Ezeket **ne commitold**, mert minden gépen újra létrejönnek.

---

## 5. Mi került bele a magyarításba

Ez Linuxon és Macen ugyanaz a forrás:

- Magyar szöveg és felirat, 5450 sor (`launcher/hu.txt`). A motor a `NOLF_LANG_FILE`-ból tölti be.
- Ékezetes betűk a játék bitmap fontjaiban (á é í ó ö ő ú ü ű).
- Nagyobb, élsimított feliratbetű.
- Magyar nyelv esetén a feliratok alapból be vannak kapcsolva.
- Javítások:
  - régi mentés betöltésekor nem omlik össze;
  - játék után a menüben nem ragad be az egér, ez a Kilépés ablakot is érintette;
  - a leltárban működik a „Vissza” nyíl;
  - az egér-érzékenység csúszka működik.

A Mac-specifikus részletek (Xcode, DMG): `MAC_MAGYAR.md`.

---

## 6. Git: honnan, hová

Két saját szerver van, mindkettőn ugyanaz a `linux-port` ág:

| Név | Cím |
|---|---|
| Elsődleges | `<repo-URL>` |
| Tükör | `<repo-URL>` |

Friss letöltés:

```sh
cd ~/workspace
git clone -b linux-port <repo-URL> nolf1
cd nolf1 && ./forditas.sh
```

Módosítás után mindkét helyre:

```sh
git status -uno               # mi változott (a ?? symlinkeket nem mutatja)
git add -u                    # a már követett, módosított fájlok
git add uj_fajl.cpp           # új fájlt név szerint (a ?? .h symlinkeket ne!)
git commit -m "leírás"
git push origin linux-port               # GitLab (ha az origin a GitLab)
git push origin-git linux-port           # saját git szerver
```

Ha nincs `origin-git` remote:

```sh
git remote add origin-git <repo-URL>
```

