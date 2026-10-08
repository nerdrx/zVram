#!/usr/bin/env python3
"""Build a relocatable, versioned userspace tarball for NX Hub."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def package(build_dir, output, version):
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", version):
        raise ValueError("version must be a semantic version without the v prefix")
    build_dir, output = Path(build_dir).resolve(), Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    codecs = json.loads((build_dir / "zvram-codecs.json").read_text())
    backend = ["libzvram_layer.so", "VK_LAYER_NX_zvram.json", "zvram-codecs.json"]
    if codecs.get("bp16_gpu"):
        backend += ["bp16.spv", "bp16-encode-analyze.spv", "bp16-encode-pack.spv"]
    if codecs.get("gdeflate_gpu"):
        backend.append("gdeflate-wave32.spv")
    for name in backend:
        if not (build_dir / name).is_file():
            raise ValueError("Missing runtime artifact: " + name)
    asset = output / f"zvram-{version}-linux-x86_64.tar.gz"
    with tempfile.TemporaryDirectory(prefix="zvram-package-") as temp:
        stage = Path(temp)
        app = stage / "usr/share/zvram" / version
        app.mkdir(parents=True)
        for name in ("zvram", "zvram_manager.py", "zvram_control.py", "zvram_model.py", "zvram_ui.py", "LICENSE", "README.md"):
            shutil.copyfile(ROOT / name, app / name)
        (app / "zvram").chmod(0o755)
        (app / "VERSION").write_text(version + "\n")
        shutil.copytree(ROOT / "docs", app / "docs")
        shutil.copytree(ROOT / "assets", app / "assets")
        (app / "build").mkdir()
        for name in backend:
            shutil.copyfile(build_dir / name, app / "build" / name)
        manifest_path = app / "build/VK_LAYER_NX_zvram.json"
        manifest = json.loads(manifest_path.read_text())
        manifest["layer"]["library_path"] = "./libzvram_layer.so"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
        binary = stage / "usr/bin/zvram"
        binary.parent.mkdir(parents=True)
        binary.symlink_to(f"../share/zvram/{version}/zvram")
        desktop = stage / "usr/share/applications/zvram-manager.desktop"
        desktop.parent.mkdir(parents=True)
        desktop.write_text("[Desktop Entry]\nType=Application\nName=zVram Manager\n"
                           "Comment=Manage zVram launches and GPU memory\nExec=zvram gui\n"
                           "Icon=zvram\nTerminal=false\nStartupNotify=true\nCategories=System;Monitor;Utility;\n")
        icon = stage / "usr/share/icons/hicolor/scalable/apps/zvram.svg"
        icon.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "assets/icon.svg", icon)
        with tarfile.open(asset, "w:gz", compresslevel=6) as archive:
            archive.add(stage / "usr", arcname="usr")
    shutil.copyfile(ROOT / "nx-app.json", output / "nx-app.json")
    digest = hashlib.sha256(asset.read_bytes()).hexdigest()
    sidecar = asset.with_name(asset.name + ".sha256")
    sidecar.write_text(f"{digest}  {asset.name}\n")
    names = [asset.name, sidecar.name, "nx-app.json"]
    (output / "SHA256SUMS").write_text("".join(
        hashlib.sha256((output / name).read_bytes()).hexdigest() + "  " + name + "\n" for name in names))
    return asset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    try:
        print(package(args.build_dir, args.output, args.version))
    except (OSError, ValueError) as exc:
        parser.exit(1, str(exc) + "\n")


if __name__ == "__main__":
    main()
