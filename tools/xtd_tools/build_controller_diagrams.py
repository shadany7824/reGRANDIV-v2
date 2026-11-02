#!/usr/bin/env python3
"""Builds the pause menu Controls picture for other controllers.

frontend_360.xtd's controller.dds is the Xbox 360 pad with leader lines out
to the labels the game draws; controller_draw.js draws the PlayStation, Switch
Pro or newer Xbox pad with its lines from the same label rows to that pad's
controls, and the result goes back into a copy of the dictionary, one per
button-prompt set
(rpf_button_prompts.cpp mounts it beside that set's buttons_360.xtd).

Usage: build_controller_diagrams.py <xtd_store binary> <frontend_360.xtd> <out root> [work dir]
writes <out root>/<variant>/frontend_360.xtd for each variant below (ONLY=a,b:
just those), and the
pictures alone, to ship without the game's textures: <variant>/controller.bin
(the tiled texture) and controller-place.txt (where it goes in the unpacked
dictionary, and the stock picture's SHA-256), which rpf_button_prompts.cpp
BuildFrontend writes into the player's own frontend_360.xtd.
"""

import hashlib
import importlib.util
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

# Prompt set -> controller_draw.js style.
VARIANTS = {"ps3": "ps", "ps4": "ps", "ps5": "ps", "switch": "switch", "xbox_one": "xbox", "xbox_series_x": "xbox",
            "keyboard": "keys_foot", "touch": "touch"}
# (the keyboard's two pictures, on foot and in a vehicle, also as the tiled
# texture bytes the web build copies over the loaded one when the page
# changes between them: web_frame_capture.cpp SetControlsPicture)
SWAPPED = {"keyboard": {"controls_foot.bin": "keys_foot", "controls_vehicle.bin": "keys_vehicle"}}


def load(name, file):
    spec = importlib.util.spec_from_file_location(name, HERE / file)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # (dataclasses look themselves up there)
    spec.loader.exec_module(module)
    return module


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    store, template, out_root = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    work = Path(sys.argv[4]) if len(sys.argv) > 4 else out_root / "controller_work"
    work.mkdir(parents=True, exist_ok=True)
    roundtrip = load("roundtrip", "button_prompt_roundtrip.py")
    keyboard = load("build_keyboard_prompts", "build_keyboard_prompts.py")

    unpacked = work / "frontend.bin"
    subprocess.run([str(store), "unpack", str(template), str(unpacked)], check=True)
    base = bytes(unpacked.read_bytes())
    roundtrip.TEXTURE_NAMES = [part.split(b"\0")[0].decode("ascii").removesuffix(".dds")
                               for part in base.split(b"pack:/")[1:]]
    record = roundtrip.parse_texture_records(base)["controller"]
    if record.texture_type != 20:
        raise SystemExit(f"controller: texture type {record.texture_type}, not DXT5")
    cpu_size, _ = roundtrip.rsc_segment_sizes(template.read_bytes()[:16])
    at = cpu_size + record.gpu_offset

    original = work / "controller.dds"
    subprocess.run([sys.executable, str(HERE / "xtd_extract_dxt5.py"), str(template), str(unpacked), "controller",
                    str(original)], check=True)
    original_png = work / "controller.png"
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", str(original), str(original_png)], check=True)

    def tiled_picture(style):
        png = work / f"controller_{style}.png"
        subprocess.run(["osascript", "-l", "JavaScript", str(HERE / "controller_draw.js"), str(png), style],
                       check=True, capture_output=True)
        rgba = subprocess.run(["ffmpeg", "-v", "error", "-i", str(png), "-f", "rawvideo", "-pix_fmt", "rgba", "-"],
                              check=True, capture_output=True).stdout
        if len(rgba) != record.width * record.height * 4:
            raise SystemExit(f"{style}: {len(rgba)} bytes for {record.width}x{record.height}")
        return roundtrip.swizzle_dxt5_payload(keyboard.bc3(rgba, record.width, record.height),
                                              record.width, record.height)

    size = record.width * record.height  # (DXT5: a byte a texel)
    place = out_root / "controller-place.txt"
    place.parent.mkdir(parents=True, exist_ok=True)
    place.write_text(f"{at} {hashlib.sha256(base[at:at + size]).hexdigest()}\n")
    print("wrote", place)
    only = set(filter(None, os.environ.get("ONLY", "").split(",")))
    for variant, style in VARIANTS.items():
        if only and variant not in only:
            continue
        tiled = tiled_picture(style)
        (out_root / variant).mkdir(parents=True, exist_ok=True)
        (out_root / variant / "controller.bin").write_bytes(tiled)
        for name, other in SWAPPED.get(variant, {}).items():
            path = out_root / variant / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(tiled if other == style else tiled_picture(other))
            print("wrote", path)
        data = bytearray(base)
        data[at:at + len(tiled)] = tiled
        patched = work / f"frontend_{variant}.bin"
        patched.write_bytes(data)
        output = out_root / variant / "frontend_360.xtd"
        output.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([str(store), "pack", str(template), str(patched), str(output)], check=True)
        print("wrote", output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
