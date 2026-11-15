#!/usr/bin/env python3
"""Assemble the Retro web installer into build/installer/ after `idf.py build`.

    python tools/build_installer.py

Copies installer/ (the page) and the four files `idf.py flash` writes, at the
offsets in build/flasher_args.json, and writes firmware/firmware.json for the
page. The result is a static site (GitHub Pages serves it).
"""
import datetime, json, pathlib, shutil, subprocess

root = pathlib.Path(__file__).resolve().parent.parent
build = root / "build"
out = build / "installer"
args = json.loads((build / "flasher_args.json").read_text())

if out.exists():
    shutil.rmtree(out)
shutil.copytree(root / "installer", out, ignore=shutil.ignore_patterns("test.mjs"))
(out / "firmware").mkdir()

parts = []
for offset, rel in sorted(args["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    src = build / rel
    shutil.copyfile(src, out / "firmware" / src.name)
    parts.append({"path": src.name, "offset": int(offset, 16), "size": src.stat().st_size})

git = lambda *a: subprocess.run(["git", *a], cwd=root, capture_output=True, text=True, check=True).stdout.strip()
version = git("rev-parse", "--short", "HEAD") + ("-dirty" if git("status", "--porcelain", "--untracked-files=no") else "")
(out / "firmware" / "firmware.json").write_text(json.dumps({
    "name": "Retro for Solana OS",
    "version": version,
    "built": datetime.date.today().isoformat(),
    "flash": args["flash_settings"],
    "parts": parts,
}, indent=2))
(out / ".nojekyll").write_text("")
for p in parts:
    print(f"0x{p['offset']:06x}  {p['size']:>8}  {p['path']}")
print(f"version {version} -> {out}")
