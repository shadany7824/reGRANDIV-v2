#!/bin/bash
# Makes the GTA IV on the Web release: the built page (web/build/link, from
# rebuild.sh), the server, the hub's art and fonts, the button prompts and
# Controls pictures (our own; the game's menu textures are patched on the
# player's computer), the RPF key, and the launchers. No game files.
# Usage: make-release.sh <version>      writes web/dist/gta4-web-<version>.zip
set -eu
GTA4_ROOT=${GTA4_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
VERSION=${1:?usage: make-release.sh <version>}
LINK=$GTA4_ROOT/web/build/link
OUT=$GTA4_ROOT/web/dist
NAME=gta4-web-$VERSION
STAGE=$OUT/$NAME
[ -f "$LINK/gta4.wasm" ] || { echo "build first: web/scripts/rebuild.sh"; exit 1; }
if ls "$LINK"/prompts/*/frontend_360.xtd >/dev/null 2>&1; then
  echo "prompts/ has frontend_360.xtd (the game's menu textures): remove them first"; exit 1
fi
rm -rf "${STAGE:?}" "$OUT/$NAME.zip"
mkdir -p "$STAGE/web/scripts" "$STAGE/web/build/link" "$STAGE/ps5/package" "$STAGE/ps5/assets" \
         "$STAGE/LibertyRecompLib" "$STAGE/game-files"
for f in index.html check.html record.js scene3d.js webui.js gameinput.js touchinput.js demo.js gta4.js gta4.wasm \
         scene3d-pipelines.json; do
  cp "$LINK/$f" "$STAGE/web/build/link/"
done
cp -R "$LINK/wgsl" "$LINK/prompts" "$STAGE/web/build/link/"
cp "$GTA4_ROOT/web/scripts/serve.py" "$GTA4_ROOT/web/scripts/lan-cert.sh" "$STAGE/web/scripts/"
cp "$GTA4_ROOT/web/scripts/play.py" "$STAGE/"
cp -R "$GTA4_ROOT/ps5/package/hub" "$STAGE/ps5/package/"
cp -R "$GTA4_ROOT/ps5/assets/fonts" "$STAGE/ps5/assets/"
cp "$GTA4_ROOT/LibertyRecompLib/aes_key.bin" "$STAGE/LibertyRecompLib/"
cp "$GTA4_ROOT/LICENSE" "$STAGE/LICENSE.txt"
cat > "$STAGE/game-files/PUT YOUR GAME FILES HERE.txt" <<'EOF'
Put these two files in this folder:

  - GTA IV for the USA Xbox 360, as an .iso
      (for example "Grand Theft Auto IV (USA) (En,Fr,De,Es,It).iso", 7,835,492,352 bytes)
  - Its Title Update 8: TU_1A581VI_000000K000000.0000000000205 (3,715,072 bytes)

Then double-click "Play GTA IV" (Mac: Play GTA IV.command, Windows: Play GTA IV.bat).
No game files come with this release: they have to be your own.
EOF
printf '#!/bin/bash\ncd "$(dirname "$0")" && exec python3 play.py "$@"\n' > "$STAGE/Play GTA IV.command"
printf '#!/bin/bash\ncd "$(dirname "$0")" && exec python3 play.py "$@"\n' > "$STAGE/play.sh"
printf '@echo off\r\ncd /d "%%~dp0"\r\nwhere py >nul 2>nul && (py -3 play.py %%*) || (python play.py %%*)\r\nif errorlevel 1 pause\r\n' \
  > "$STAGE/Play GTA IV.bat"
chmod +x "$STAGE/Play GTA IV.command" "$STAGE/play.sh" "$STAGE/play.py"
# The builder's home folder, which compiled-in source file names carry (log
# and assert messages in gta4.wasm), out of everything published: replaced by
# a name of the same length, so no string in the files changes size. Stops
# if the account name is still anywhere after that.
python3 -I - "$STAGE" "$HOME" "$(id -un)" <<'PY'
import os, sys
stage, home, user = sys.argv[1], sys.argv[2].encode(), sys.argv[3].encode()
fill = (b"/home/player" + b"_" * len(home))[:len(home)] if len(home) >= 12 else b"/h" + b"_" * (len(home) - 2)
changed = 0
for root, _, names in os.walk(stage):
    for name in names:
        path = os.path.join(root, name)
        data = open(path, "rb").read()
        if home in data:
            open(path, "wb").write(data.replace(home, fill))
            changed += 1
left = [os.path.join(r, n) for r, _, ns in os.walk(stage) for n in ns
        if user.lower() in open(os.path.join(r, n), "rb").read().lower()]
if left:
    sys.exit("the account name is still in: " + ", ".join(left[:5]))
print(f"home folder name replaced in {changed} files")
PY
cat > "$STAGE/README.txt" <<EOF
GTA IV on the Web $VERSION - the Xbox 360 game, recompiled to run in your browser.

1. Install Python 3 (python.org; on Windows tick "Add python.exe to PATH") and Chrome or Edge.
2. Put your own GTA IV (USA, Xbox 360) .iso and Title Update 8 in game-files/.
3. Double-click "Play GTA IV" (Mac: right-click > Open the first time). The game opens at
   http://localhost:8765/ and installs itself into the browser the first time (a few minutes).
4. Keep the window that opens running while you play; close it to stop.

Keyboard: WASD to move, mouse to look, Enter/Esc in menus. A controller works too.
F8 shows the FPS counter. Saves stay in this browser: back them up from the hub's Saves page.
Not sure your computer can run it? Open http://localhost:8765/check.html while it runs.

Web port by itzzace, built on Liberty Recompiled (OZORDI), LibertyRecompAndroid (vaduur),
the PS5 port (elripalda) and ReXGlue. GPL-3.0 (LICENSE.txt). Not affiliated with Rockstar
Games or Take-Two. No game files included. Source: https://github.com/itzzace/gta4-web
EOF
(cd "$OUT" && zip -qr -X "$NAME.zip" "$NAME")
echo "wrote $OUT/$NAME.zip ($(du -h "$OUT/$NAME.zip" | cut -f1))"