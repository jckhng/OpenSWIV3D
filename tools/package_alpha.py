"""Prepare asset-free standalone source and Windows alpha archives."""

import argparse
import hashlib
from pathlib import Path
import shutil
import zipfile


PROJECT = Path(__file__).resolve().parents[1]
TITLE = "OpenSWIV3D"
VERSION = "0.1.0-alpha.6"
DOCS = ("README.md", "BUILDING.md", "NOTICE.md", "TESTING.md", "LICENSE", "Play.cmd", "CHANGELOG.md")
PLAYER_FILES = ("swiv3d_player.exe", "swiv3d_smacker.dll")
REIMPLEMENTATION = Path("reimplementation")
DECODER = REIMPLEMENTATION / "third_party/libsmacker"
DECODER_FILES = ("README.md", "COPYING", "smacker.c", "smacker.h", "smk_malloc.h")
DECODER_CMAKE = """cmake_minimum_required(VERSION 3.16)
project(swiv3d_smacker LANGUAGES C)
add_library(swiv3d_smacker SHARED smacker.c)
set_target_properties(swiv3d_smacker PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
"""


def copy_files(source, target, files):
    for relative in files:
        item = source / relative
        if not item.is_file() or not item.resolve().is_relative_to(source.resolve()):
            raise ValueError(f"Missing or external release input: {item}")
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(item, destination)


def archive(folder, path):
    files = sorted(item for item in folder.rglob("*") if item.is_file())
    with zipfile.ZipFile(path, "x") as output:
        for item in files:
            member = zipfile.ZipInfo(item.relative_to(folder.parent).as_posix())
            output.writestr(member, item.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)
    with zipfile.ZipFile(path) as output:
        if output.testzip() is not None or len(output.namelist()) != len(files):
            raise ValueError(f"Archive verification failed: {path}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    path.with_suffix(".zip.sha256").write_text(f"{digest}  {path.name}\n", encoding="ascii")
    print(path.resolve())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kind", choices=("source", "windows"))
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, help="Directory containing the Release player and DLL")
    args = parser.parse_args()
    if (args.kind == "windows") != bool(args.build_dir):
        parser.error("--build-dir is required for Windows packaging only")

    name = f"{TITLE}-{VERSION}-{args.kind}"
    if args.kind == "windows":
        name += "-x64"
    output = args.out_dir / name
    bundle = args.out_dir / (name + ".zip")
    if output.exists() or bundle.exists() or bundle.with_suffix(".zip.sha256").exists():
        parser.error("Release output must be fresh")
    docs = PROJECT / "release" if (PROJECT / "release").is_dir() else PROJECT
    output.mkdir(parents=True)
    copy_files(docs, output, DOCS)
    copy_files(PROJECT, output, (Path("docs/DEVELOPMENT.md"), Path("docs/RELEASE_AUDIT.md")))
    copy_files(PROJECT, output, (DECODER / name for name in DECODER_FILES))
    if args.kind == "source":
        copy_files(docs, output, (".gitignore", ".gitattributes"))
        copy_files(PROJECT, output, (REIMPLEMENTATION / "CMakeLists.txt", Path("tools/package_alpha.py")))
        sources = (path.relative_to(PROJECT) for subtree in ("src", "include", "tests")
                   for path in (PROJECT / REIMPLEMENTATION / subtree).rglob("*")
                   if path.is_file() and path.suffix in {".c", ".cpp", ".h", ".hpp"})
        copy_files(PROJECT, output, sources)
    else:
        copy_files(args.build_dir, output, PLAYER_FILES)
        (output / DECODER / "CMakeLists.txt").write_text(DECODER_CMAKE, encoding="ascii")
    archive(output, bundle)


if __name__ == "__main__":
    main()
