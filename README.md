# reGRANDIV-v2 - GTA IV on the Web

**Grand Theft Auto IV (Xbox 360) running in a web browser**: the real game, recompiled to WebAssembly, drawn with WebGPU, played from your own copy of the disc.

Forked from here. Go support him. https://github.com/itzzace/gta4-web

![GTA IV's title over Liberty City, running in the browser](web/images/intro-title.jpg)

# Link

Discord: https://discord.gg/hrbcPjjZfj

Telegram: https://t.me/reGRANDTA/

> ### Built on Liberty Recompiled, LibertyRecompAndroid and the PS5 port
> This is the work of **[OZORDI](https://github.com/OZORDI)** ([Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp), the native recompilation of GTA IV), **[vaduur](https://github.com/vaduur)** ([LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid)) and **[elripalda](https://github.com/elripalda)** ([the PS5 port](https://github.com/elripalda/gta4-ps5) and its hub), with **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** underneath. The web port by **[itzzace](https://github.com/itzzace)** takes all of that into the browser.
>
> Go star the original projects and support them.

## Play it
You need your **own** GTA IV for the **USA Xbox 360**. No game files come with this.

1. **Install** [Python 3](https://www.python.org/downloads/) (on Windows, tick *Add python.exe to PATH*) and **Chrome** or **Edge**.
2. **Download** `gta4-web-<version>.zip` from [Releases](../../releases) and unzip it.
3. **Put your game files** in its `game-files` folder:
   | File | Size |
   |---|---|
   | The USA Xbox 360 disc as an `.iso`, for example `Grand Theft Auto IV (USA) (En,Fr,De,Es,It).iso` | 7,835,492,352 bytes |
   | Title Update 8: `TU_1A581VI_000000K000000.0000000000205` | 3,715,072 bytes |
4. **Double-click** `Play GTA IV` (`Play GTA IV.command` on a Mac: right-click it and pick *Open* the first time; `Play GTA IV.bat` on Windows). The game opens in your browser at http://localhost:8765/.

The first start installs the game into the browser, which takes a few minutes and about 8 GB of space. After that it starts in seconds. Keep the small window that opens running while you play, and close it to stop.

**Controls:** WASD and the mouse, Enter and Esc in menus, or plug in a controller. **F8** shows the FPS counter, and the buttons in the top right switch to fullscreen.

**Saves** live in your browser. Back them up from the hub's *Saves* page (*Download Saves*), and bring 360 saves over with *Import Xbox 360 Saves*.

### If it doesn't start
- **Use Chrome or Edge** on a computer with about 8 GB of free memory. Safari, Firefox and iPhone / iPad browsers can't run it yet. While the game's window is open, http://localhost:8765/check.html shows what your browser is missing.
- **"No disc image found"** means the `.iso` isn't in `game-files`. Only the **USA** Xbox 360 disc works, not PC, PS3 or other editions.
- **Stuck installing?** Keep the tab in front: browsers slow down background tabs.
- **Play from another device on your Wi-Fi** (experimental, the server on a Mac for now): start it with `GTA4_WEB_LAN=1 python3 play.py` and open the setup address it prints on the other device, which walks you through trusting the certificate once. Phones and tablets mostly can't run the game yet: check them with `check.html` first.

## What's in it
- GTA IV running in Chrome, Edge or another Chromium browser, from WebAssembly (64-bit memory, threads) and WebGPU
- The game's own Xbox 360 shaders converted to WGSL, with the 360's display gamma, so it looks like the console: checked frame by frame against a recording of the Xbox 360
- Unzip, drop in your disc image, double-click: it installs into the browser's storage once, then starts from there
- The PS5 port's hub in the browser: Home, Display, Game, Episodes, Saves, Achievements and About
- Keyboard and mouse, or a controller, with button prompts for PlayStation, Xbox 360, Xbox One / Series and Switch pads, and readable keyboard keycaps
- The pause menu's Controls page drawn for each controller, and for the keyboard on foot and in a vehicle, put into your own copy of the game's menu textures when it installs
- Saves kept in your browser, with Download Saves, Load Saves From a File and Xbox 360 save import (each player picks their own files)
- A loading screen with install progress, a Fullscreen button and an FPS counter (F8)
- Play from a phone or tablet on your Wi-Fi, served from your own computer (experimental), with a device check page

![Niko and Roman in the opening cutscene](web/images/intro-niko-roman.jpg)

## Screenshots
| | |
|---|---|
| ![The hub's Home page](web/images/hub.jpg) | ![The hub's Display settings](web/images/display.jpg) |
| ![The opening, below deck](web/images/intro-hallway.jpg) | ![The rooftop in the opening](web/images/intro-rooftop.jpg) |

The pause menu's Controls page, redrawn for each controller and the keyboard:

![PlayStation, Xbox, Switch and keyboard controls pictures](web/images/controllers.jpg)

## Building
To run it from a build instead of a release: `python3 web/scripts/play.py "<disc image>" "<title update>"` from the repository's folder, once `web/build/link` is built.

The web build lives in [`web/`](web):
- [`web/scripts`](web/scripts): the page (`index.html`, `scene3d.js` for the 3D renderer, `webui.js` for the hub's overlays, `gameinput.js`), the server and the build scripts. Copy them into `web/build` and run them from there.
- [`web/shaders`](web/shaders): the tools that turn the game's shaders into WGSL.
- [`web/thirdparty`](web/thirdparty): the third-party libraries' Emscripten build (`web/build-tp`).
- The game-side changes are in `glue/rexglue-sdk-main` (search for `[web port]` and `__EMSCRIPTEN__`), most of them in `gta4-recomp/src/web/` and `src/graphics/web_frame_capture.cpp`.

You need [Emscripten](https://emscripten.org) (`EMSDK_DIR`, by default next to this repository), then `web/scripts/rebuild.sh` compiles the game code, the runtime and the glue and links `web/build/link/gta4.wasm`. The Controls pictures are drawn by `tools/xtd_tools/build_controller_diagrams.py` and stored as the pictures alone (`LibertyRecompLib/private/button_prompts/*/controller.bin`): the game puts them into the player's own `frontend_360.xtd` (`rpf_button_prompts.cpp`). `web/scripts/make-release.sh <version>` makes the release zip. The build is a work in progress and its steps are still rough.

The PS5 build, its hub and its packaging are as the PS5 port has them: see [README-PS5.md](README-PS5.md) and [ps5/BUILD.md](ps5/BUILD.md).

## Credits
- **[itzzace](https://github.com/itzzace)**: the web port: the WebAssembly and WebGPU build, the renderer, display gamma, controls, keyboard prompts, controller pictures, saves, loading screen and Wi-Fi play
- **[OZORDI](https://github.com/OZORDI)**: [Liberty Recompiled](https://github.com/OZORDI/LibertyRecomp), the GTA IV recompilation this is built on, with its contributors
- **[vaduur](https://github.com/vaduur)**: [LibertyRecompAndroid](https://github.com/vaduur/LibertyRecompAndroid), the fork the PS5 port starts from
- **[elripalda](https://github.com/elripalda)**: [the PS5 port](https://github.com/elripalda/gta4-ps5), the hub, the PlayStation buttons and the art
- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)**: the static recompiler and runtime, which builds on [Xenia](https://github.com/xenia-project/xenia) and [XenonRecomp](https://github.com/hedge-dev/XenonRecomp)
- **[ThirteenAG](https://github.com/ThirteenAG)**: [FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix), whose shaders and camera options the hub uses
- [Emscripten](https://emscripten.org), [naga](https://github.com/gfx-rs/wgpu/tree/trunk/naga), [SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), [FFmpeg](https://ffmpeg.org), [stb](https://github.com/nothings/stb), [dr_libs](https://github.com/mackron/dr_libs), and the TeX Gyre Heros, Oswald and Inter fonts
- Everyone credited in the [PS5 port's README](README-PS5.md#credits)

## License
Liberty Recompiled, LibertyRecompAndroid, the PS5 port and this web port are under the [GPL-3.0](LICENSE). ReXGlue keeps its own BSD-style license ([glue/rexglue-sdk-main/LICENSE](glue/rexglue-sdk-main/LICENSE)), and the third-party libraries keep theirs. None of these cover the game, its artwork or its music.

*An unofficial fan project, not affiliated with or endorsed by Rockstar Games, Take-Two Interactive, Microsoft or Sony. Grand Theft Auto and all related names belong to their owners. No game files are included: you need your own copy of GTA IV for Xbox 360. The screenshots are of the game running in this port.*
