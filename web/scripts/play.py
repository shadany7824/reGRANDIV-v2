#!/usr/bin/env python3
"""Starts GTA IV on the Web from a release folder: finds your disc image and
Title Update 8 in game-files/ (or takes their paths), checks them, starts the
local server (serve.py) and opens the game in your browser.

Usage: play.py [disc image] [title update]      (GTA4_WEB_LAN=1: also on your Wi-Fi)
"""

import glob
import os
import socket
import subprocess
import sys
import threading
import time
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
# (the release folder: play.py sits at its top, serve.py in web/scripts)
ROOT = HERE if os.path.isdir(os.path.join(HERE, "web")) else os.path.abspath(os.path.join(HERE, "..", ".."))
FILES = os.path.join(ROOT, "game-files")
PORT = 8765  # (the browser keeps the install per address: keep it the same)
DISC_SIZE = 7835492352
UPDATE_NAME, UPDATE_SIZE = "TU_1A581VI_000000K000000.0000000000205", 3715072


def stop(message):
    # (the Windows launcher waits for a key after an error; Terminal stays open)
    print("\n" + message + "\n")
    sys.exit(1)


def find(pattern):
    found = sorted(glob.glob(os.path.join(FILES, pattern)))
    return found[0] if found else None


def main():
    sys.stdout.reconfigure(line_buffering=True)
    lan = os.environ.get("GTA4_WEB_LAN") == "1"
    if sys.version_info < (3, 8):
        stop("Python 3.8 or newer is needed (python.org).")
    disc = sys.argv[1] if len(sys.argv) > 1 else find("*.iso")
    update = sys.argv[2] if len(sys.argv) > 2 else (find(UPDATE_NAME) or find("TU_*"))
    if not disc or not os.path.isfile(disc):
        stop(f"No disc image found. Put your GTA IV (USA, Xbox 360) .iso in:\n  {FILES}")
    if not update or not os.path.isfile(update):
        stop(f"No Title Update 8 found. Put {UPDATE_NAME} in:\n  {FILES}")
    if os.path.getsize(disc) != DISC_SIZE:
        print(f"Warning: {os.path.basename(disc)} is {os.path.getsize(disc):,} bytes; the USA disc is "
              f"{DISC_SIZE:,}. Another edition or a bad rip may not work.")
    if os.path.getsize(update) != UPDATE_SIZE:
        print(f"Warning: {os.path.basename(update)} is {os.path.getsize(update):,} bytes; Title Update 8 is "
              f"{UPDATE_SIZE:,}.")
    with socket.socket() as s:
        if s.connect_ex(("127.0.0.1", PORT)) == 0:
            print(f"Already running: opening http://localhost:{PORT}/")
            webbrowser.open(f"http://localhost:{PORT}/")
            return
    serve = os.path.join(ROOT, "web", "scripts", "serve.py")
    site = os.path.join(ROOT, "web", "build", "link")
    if not os.path.isfile(os.path.join(site, "gta4.wasm")):
        # (GitHub's "Source code" download has no build in it)
        stop("The game isn't built in this folder. Download gta4-web-<version>.zip from the Releases page "
             "(not \"Source code\"), unzip it and run Play GTA IV from there.")
    key = os.path.join(ROOT, "LibertyRecompLib", "aes_key.bin")
    print(f"Disc:          {disc}\nTitle Update:  {update}\n")
    if lan:
        print("Wi-Fi mode: the server prints the addresses to open below.")
    else:
        print(f"GTA IV is at http://localhost:{PORT}/ - open it in Chrome or Edge.")
    print("The first start installs the game into the browser (a few minutes). Keep this window open while you "
          "play; close it to stop.\n")
    server = subprocess.Popen([sys.executable, "-I", serve, site, disc, update, key, str(PORT)])

    def open_page():
        for _ in range(50):
            with socket.socket() as s:
                if s.connect_ex(("127.0.0.1", PORT)) == 0:
                    webbrowser.open(f"http://localhost:{PORT}/")
                    return
            time.sleep(0.2)

    if not lan:
        threading.Thread(target=open_page, daemon=True).start()
    try:
        server.wait()
    except KeyboardInterrupt:
        server.terminate()


if __name__ == "__main__":
    main()
