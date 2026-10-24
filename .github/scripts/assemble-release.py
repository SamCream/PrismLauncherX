#!/usr/bin/env python3
"""Collect exactly one tested package per platform and create release checksums."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil
import tarfile
import zipfile


def assemble(artifacts: Path, source: Path, output: Path, version: str):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", version):
        raise ValueError("Unsafe release version")
    if output.exists() and any(output.iterdir()):
        raise ValueError("Output directory must be empty")
    output.mkdir(parents=True, exist_ok=True)
    files = []

    def one(pattern):
        matches = list(artifacts.glob(pattern))
        if len(matches) != 1:
            raise ValueError(f"Expected exactly one {pattern}, found {len(matches)}")
        return matches[0]

    def copy(pattern, name):
        src = one(pattern)
        if not src.is_file() or src.stat().st_size == 0:
            raise ValueError(f"Missing or empty package: {src}")
        dest = output / name
        shutil.copyfile(src, dest)
        files.append(dest)

    for arch, platform in [("x86_64", "Linux"), ("aarch64", "Linux-aarch64")]:
        copy(f"PrismLauncher-{platform}-Qt6-Portable-*-Release/PrismLauncher-portable.tar.gz",
             f"PrismLauncher-{platform}-Qt6-Portable-{version}.tar.gz")
        for suffix in ["AppImage", "AppImage.zsync"]:
            copy(f"PrismLauncher-Linux-*-Release-{arch}.{suffix}/PrismLauncher-Linux-*{arch}.{suffix}",
                 f"PrismLauncher-Linux-{arch}.{suffix}")
    for suffix in ["zip", "dmg"]:
        directory_suffix = ".dmg" if suffix == "dmg" else ""
        copy(f"PrismLauncher-macOS-*-Release{directory_suffix}/PrismLauncher.{suffix}",
             f"PrismLauncher-macOS-{version}.{suffix}")
    for platform in ["Windows-MinGW-w64", "Windows-MSVC", "Windows-MSVC-arm64"]:
        copy(f"PrismLauncher-{platform}-Setup-*-Release/PrismLauncher-Setup.exe",
             f"PrismLauncher-{platform}-Setup-{version}.exe")
        for kind in ["", "-Portable"]:
            # Avoid treating Portable/Setup/arm64 artifact directories as the x64 package.
            pattern = re.compile(rf"^PrismLauncher-{re.escape(platform + kind)}-[0-9a-f]+-Release$")
            directories = [p for p in artifacts.iterdir() if p.is_dir() and pattern.fullmatch(p.name)]
            if len(directories) != 1:
                raise ValueError(f"Expected one Windows package for {platform}{kind}")
            directory = directories[0]
            contents = sorted(p for p in directory.rglob("*") if p.is_file())
            if not any(p.suffix.lower() == ".exe" for p in contents):
                raise ValueError(f"No executable in {directory}")
            dest = output / f"PrismLauncher-{platform}{kind}-{version}.zip"
            with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as archive:
                for path in contents:
                    archive.write(path, path.relative_to(directory))
            files.append(dest)
    archive_path = output / f"PrismLauncher-{version}.tar.gz"
    with tarfile.open(archive_path, "w:gz") as archive:
        def exclude_git(member):
            return None if ".git" in Path(member.name).parts else member
        archive.add(source, arcname=f"PrismLauncher-{version}", filter=exclude_git)
    files.append(archive_path)
    if len(files) != 18:
        raise ValueError(f"Expected 18 release assets, got {len(files)}")
    with (output / "SHA256SUMS.txt").open("w", encoding="utf-8") as manifest:
        for path in sorted(files):
            manifest.write(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n")
    return files


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    assemble(args.artifacts, args.source, args.output, args.version)
