#!/usr/bin/env python3
"""Bump cuimoji's version strings and promote the changelog section.

Usage:
    tools/bump_version.py 0.8 [--date YYYY-MM-DD] [--dry-run]

The release checksums are refreshed by the release workflow after publishing;
this script only touches the version strings and the changelog. It does not
commit, tag or push.
"""
import argparse
import datetime
import difflib
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VERSION_RE = re.compile(r"^\d+\.\d+(?:\.\d+)?$")
TAG_URL = "https://github.com/NoCrypt/cuimoji/releases/tag/v{}"


def fail(message):
    print(f"bump_version: {message}", file=sys.stderr)
    sys.exit(1)


def change(path, text, pattern, replacement, count=1):
    new, n = re.subn(pattern, replacement, text, count=count, flags=re.MULTILINE)
    if n == 0:
        fail(f"no match for {pattern!r} in {path}")
    if count and n != count:
        fail(f"expected {count} match(es) for {pattern!r} in {path}, found {n}")
    return new


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", help="new version, for example 0.8 or v0.8")
    ap.add_argument("--date", default=datetime.date.today().isoformat(),
                    help="release date (default: today, YYYY-MM-DD)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the changes instead of writing them")
    args = ap.parse_args()

    version = args.version.lstrip("v")
    if not VERSION_RE.match(version):
        fail(f"'{args.version}' is not a version like 0.8 or 1.2.3")
    try:
        datetime.date.fromisoformat(args.date)
    except ValueError:
        fail(f"'{args.date}' is not a date like 2026-11-13")

    try:
        old_version = re.search(
            r'^#define CUIMOJI_VERSION "([^"]+)"',
            (ROOT / "cuimoji.h").read_text(), re.MULTILINE).group(1)
    except AttributeError:
        fail("CUIMOJI_VERSION not found in cuimoji.h")
    if old_version == version:
        fail(f"cuimoji.h is already at {version}")

    tag = subprocess.run(["git", "-C", str(ROOT), "tag", "--list", f"v{version}"],
                         capture_output=True, text=True, check=False)
    if tag.returncode == 0 and tag.stdout.strip():
        fail(f"tag v{version} already exists")

    files = ["cuimoji.h", "linux/install.sh", "README.md",
             "packaging/arch/PKGBUILD",
             "packaging/flatpak/io.github.NoCrypt.cuimoji.json",
             "CHANGELOG.md"]
    texts = {path: (ROOT / path).read_text() for path in files}

    texts["cuimoji.h"] = change(
        "cuimoji.h", texts["cuimoji.h"],
        r'^(#define CUIMOJI_VERSION ")[^"]+(")$',
        rf"\g<1>{version}\g<2>")
    texts["linux/install.sh"] = change(
        "linux/install.sh", texts["linux/install.sh"],
        r'^(DEFAULT_VERSION=")[^"]+(")$',
        rf"\g<1>{version}\g<2>")

    for pattern, replacement in (
        (r"cuimoji-\d+\.\d+(?:\.\d+)?-linux-x86_64\.AppImage",
         f"cuimoji-{version}-linux-x86_64.AppImage"),
        (r"cuimoji-\d+\.\d+(?:\.\d+)?-x86_64\.flatpak",
         f"cuimoji-{version}-x86_64.flatpak"),
        (r"cuimoji-\d+\.\d+(?:\.\d+)?-1-x86_64\.pkg\.tar\.zst",
         f"cuimoji-{version}-1-x86_64.pkg.tar.zst"),
    ):
        texts["README.md"] = change("README.md", texts["README.md"], pattern, replacement)

    texts["packaging/arch/PKGBUILD"] = change(
        "packaging/arch/PKGBUILD", texts["packaging/arch/PKGBUILD"],
        r"^pkgver=.*$", f"pkgver={version}")

    texts["packaging/flatpak/io.github.NoCrypt.cuimoji.json"] = change(
        "packaging/flatpak/io.github.NoCrypt.cuimoji.json",
        texts["packaging/flatpak/io.github.NoCrypt.cuimoji.json"],
        r"releases/download/v[^/]+/cuimoji-[^/]+-linux-x86_64\.tar\.gz",
        f"releases/download/v{version}/cuimoji-{version}-linux-x86_64.tar.gz")

    changelog = texts["CHANGELOG.md"]
    if re.search(rf"^## \[{re.escape(version)}\]", changelog, re.MULTILINE):
        fail(f"CHANGELOG.md already has a [{version}] section")

    heading = re.search(r"^## \[Unreleased\][ \t]*$", changelog, re.MULTILINE)
    if heading:
        changelog = (changelog[:heading.start()] +
                     f"## [{version}] - {args.date}" +
                     changelog[heading.end():])
    else:
        first = re.search(r"^## \[", changelog, re.MULTILINE)
        if not first:
            fail("CHANGELOG.md has no release headings")
        changelog = (changelog[:first.start()] +
                     f"## [{version}] - {args.date}\n\n" +
                     changelog[first.start():])
        print(f"bump_version: no [Unreleased] section; added an empty "
              f"[{version}] section — fill in the notes before releasing.",
              file=sys.stderr)

    link = f"[{version}]: {TAG_URL.format(version)}"
    existing = re.search(rf"^\[{re.escape(version)}\]: .*$", changelog, re.MULTILINE)
    if existing:
        changelog = changelog[:existing.start()] + link + changelog[existing.end():]
    else:
        first_link = re.search(r"^\[\d", changelog, re.MULTILINE)
        if first_link:
            changelog = changelog[:first_link.start()] + link + "\n" + changelog[first_link.start():]
        else:
            changelog = changelog.rstrip("\n") + "\n\n" + link + "\n"
    texts["CHANGELOG.md"] = changelog

    changed = []
    for path in files:
        new = texts[path]
        old = (ROOT / path).read_text()
        if new == old:
            continue
        changed.append(path)
        if args.dry_run:
            sys.stdout.writelines(difflib.unified_diff(
                old.splitlines(keepends=True), new.splitlines(keepends=True),
                fromfile=f"a/{path}", tofile=f"b/{path}"))
        else:
            (ROOT / path).write_text(new)

    if not changed:
        fail("nothing changed")

    verb = "Would update" if args.dry_run else "Updated"
    print(f"\n{verb}: {', '.join(changed)}")
    if not args.dry_run:
        print(f"\nBumped {old_version} -> {version}. Next:")
        print("  cmake --build build -j\"$(nproc)\" && ./build/cuimoji --version")
        print(f"  git add -A && git commit -m \"Bump to {version}\"")
        print(f"  git tag v{version} && git push origin main && git push origin v{version}")


if __name__ == "__main__":
    main()
