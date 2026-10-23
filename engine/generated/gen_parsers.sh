#!/bin/bash
# gen_parsers.sh
#
# NOLF 1's ObjectDLL project lists three .cpp files that DO NOT EXIST on disk,
# because the original build generated them:
#
#   AnimationLex.cpp    <- flex,  from NOLF/ObjectDLL/AnimationLex.l
#   AnimationParse.cpp  <- bison, from NOLF/ObjectDLL/AnimationParse.y
#   ltquatbase.cpp      <- not generated; it lives in LT2/sdk/inc/
#
# The SDK even ships flex.exe and bison.exe next to the game code for this.
# The exact command lines below are taken from the custom build steps in
# Object.dsp, so this reproduces the original build rather than approximating it.
#
# Output goes here (a build artifact directory), NOT into the game source tree.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../NOLF/ObjectDLL"

command -v flex  >/dev/null || { echo "flex not found";  exit 1; }
command -v bison >/dev/null || { echo "bison not found"; exit 1; }

cd "$SRC"

# --- flex: -P renames the yy* symbols to yyanimation* ---
flex -Pyyanimation -o "$HERE/AnimationLex.cpp" AnimationLex.l

# --- bison: same prefix; --defines emits the token/symbol header ---
bison --defines="$HERE/AnimationParse.h" -v -pyyanimation \
      -o "$HERE/AnimationParse.cpp" AnimationParse.y

# Object.dsp renames bison's defines output to AnimationLexSymbols.h, which is
# the name AnimationLex.l includes.
cp "$HERE/AnimationParse.h" "$HERE/AnimationLexSymbols.h"

# The bison Monolith used declared the parse entry point in its defines header;
# bison 2.3 (what macOS ships) emits only the token enum and yyanimationlval.
# The lexer calls yyanimationparse(), so the prototype has to be supplied.
#
# ⚠️ C++ LINKAGE, NOT extern "C". An earlier revision of this script wrapped the
# prototype in extern "C". That is wrong in both directions and fails only at
# link time, in a link nothing performed yet:
#   * AnimationParse.cpp is listed in Object.dsp and so is compiled as C++ --
#     it DEFINES the mangled symbol;
#   * NOLF/ObjectDLL/AnimationParse.h, original game source, declares
#     `int yyanimationparse();` with C++ linkage.
# So extern "C" made AnimationLex.o alone ask for an unmangled symbol nothing
# defines. It went unnoticed because the skeleton host links the CLIENT side
# only, and both parser objects are ObjectDLL's.
if ! grep -q 'yyanimationparse' "$HERE/AnimationLexSymbols.h"; then
cat >> "$HERE/AnimationLexSymbols.h" <<'EOF'

/* PORT: bison 2.3 does not put the parse entry point in its --defines header,
   but AnimationLex.l calls it. Older bison (used by the original build) did.
   C++ linkage deliberately -- that is how AnimationParse.cpp defines it and
   how NOLF/ObjectDLL/AnimationParse.h declares it. */
int yyanimationparse(void);
EOF
fi

rm -f "$HERE/AnimationParse.output"

echo "generated:"
for f in AnimationLex.cpp AnimationParse.cpp AnimationParse.h AnimationLexSymbols.h; do
    printf "  %-26s %s bytes\n" "$f" "$(wc -c < "$HERE/$f" | tr -d ' ')"
done
