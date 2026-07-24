#!/usr/bin/env python3
"""initialize_files.py — fetch the download-vendored third-party MP3 libraries.

The engine builds native MP3 support from two upstream libraries that have **no
official Git repository** (upstream is SVN + release tarballs): libmpg123
(decode) and LAME/libmp3lame (encode). Rather than commit their multi-megabyte
source into this repo, we download the pinned **official** release tarballs on
demand and extract only the subset the build needs into ``third_party/``.

Run this once after cloning, and again whenever a pin below changes:

    python3 initialize_files.py            # fetch what is missing
    python3 initialize_files.py --force    # re-fetch even if already present
    python3 initialize_files.py --clean    # remove the downloaded trees
    python3 initialize_files.py --check    # verify presence, fetch nothing

Pure standard library — cross-platform (Windows / macOS / Linux), no pip deps.
Importable too: ``from initialize_files import ensure_all; ensure_all()``.

What stays committed (small, essential) and is NOT downloaded:
  * the build-generated ``config.h`` in ``third_party/{mpg123,lame}-config/``
  * ``cmake/ae_mpg123.cmake`` / ``cmake/ae_lame.cmake``
  * the engine glue in ``backends/mp3/``

Licensing: only the *libraries* are fetched (libmpg123 is LGPL-2.1; libmp3lame
is LGPL) — never the GPL ``mpg123`` / ``lame`` command-line tools. The upstream
license files (COPYING/LICENSE) are extracted alongside the source.

To bump a version: change ``url``/``version``/``sha256`` below, then regenerate
the matching ``config.h`` (cross-``./configure`` for aarch64-linux-android — see
``cmake/ae_mpg123.cmake`` / ``ae_lame.cmake`` for the exact options) and, if the
upstream source list changed, update those cmake helpers.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import sys
import tarfile
import urllib.request

REPO_ROOT = os.path.dirname(os.path.abspath(__file__))

# --- pins --------------------------------------------------------------------
# Each component: the official tarball, its sha256, and the subset to keep.
# ``keep`` entries ending in "/" are directory prefixes; others are exact files.
# Paths are relative to the tarball's single top-level directory (stripped).
COMPONENTS = [
    {
        "name": "mpg123",
        "version": "1.32.10",
        "role": "MP3 decode (libmpg123, LGPL-2.1)",
        "url": "https://www.mpg123.de/download/mpg123-1.32.10.tar.bz2",
        "sha256": "87b2c17fe0c979d3ef38eeceff6362b35b28ac8589fbf1854b5be75c9ab6557c",
        "dest": os.path.join("third_party", "mpg123"),
        "keep": [
            "src/libmpg123/", "src/compat/", "src/common/", "src/include/",
            "src/version.h", "COPYING", "AUTHORS",
        ],
        "sentinel": "src/libmpg123/libmpg123.c",
    },
    {
        "name": "lame",
        "version": "3.100",
        "role": "MP3 encode (libmp3lame, LGPL)",
        "url": "https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz",
        "sha256": "ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e",
        "dest": os.path.join("third_party", "lame"),
        "keep": [
            "libmp3lame/", "mpglib/", "include/",
            "COPYING", "LICENSE", "README", "AUTHORS",
        ],
        "sentinel": "libmp3lame/lame.c",
    },
]


def _log(msg: str) -> None:
    print(msg, flush=True)


def is_present(comp: dict) -> bool:
    return os.path.isfile(os.path.join(REPO_ROOT, comp["dest"], comp["sentinel"]))


def _download(url: str) -> bytes:
    _log(f"    downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "audio_engine-initialize/1.0"})
    with urllib.request.urlopen(req, timeout=120) as resp:  # nosec - pinned URL, sha256-verified
        return resp.read()


def _verify(data: bytes, expected_sha256: str) -> None:
    got = hashlib.sha256(data).hexdigest()
    if got != expected_sha256:
        raise SystemExit(
            f"    sha256 MISMATCH\n      expected {expected_sha256}\n      got      {got}\n"
            "    Refusing to extract. The download may be corrupt or the pin is wrong."
        )
    _log(f"    sha256 OK ({got[:16]}…)")


def _wanted(rel: str, keep: list[str]) -> bool:
    for k in keep:
        if k.endswith("/"):
            if rel.startswith(k):
                return True
        elif rel == k:
            return True
    return False


def _safe_join(root: str, rel: str) -> str:
    # Prevent path traversal from a malicious member name.
    dest = os.path.realpath(os.path.join(root, rel))
    root_real = os.path.realpath(root)
    if dest != root_real and not dest.startswith(root_real + os.sep):
        raise SystemExit(f"    unsafe path in archive: {rel!r}")
    return dest


def _extract(data: bytes, comp: dict) -> int:
    dest_root = os.path.join(REPO_ROOT, comp["dest"])
    count = 0
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as tar:
        for member in tar:
            if not (member.isfile() or member.isdir()):
                continue  # skip symlinks / devices / hardlinks
            # Strip the single top-level directory (e.g. "mpg123-1.32.10/").
            parts = member.name.split("/", 1)
            if len(parts) < 2 or not parts[1]:
                continue
            rel = parts[1]
            if not _wanted(rel, comp["keep"]):
                continue
            out_path = _safe_join(dest_root, rel)
            if member.isdir():
                os.makedirs(out_path, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            src = tar.extractfile(member)
            if src is None:
                continue
            with open(out_path, "wb") as f:
                f.write(src.read())
            count += 1
    return count


def fetch(comp: dict, force: bool = False) -> None:
    name = comp["name"]
    if is_present(comp) and not force:
        _log(f"[{name}] {comp['version']} already present — skipping ({comp['role']})")
        return
    _log(f"[{name}] fetching {comp['version']} — {comp['role']}")
    data = _download(comp["url"])
    _verify(data, comp["sha256"])
    n = _extract(data, comp)
    if not is_present(comp):
        raise SystemExit(f"    extraction produced no sources for {name} (sentinel missing)")
    _log(f"    extracted {n} files into {comp['dest']}/")


def clean(comp: dict) -> None:
    import shutil
    dest = os.path.join(REPO_ROOT, comp["dest"])
    if os.path.isdir(dest):
        shutil.rmtree(dest)
        _log(f"[{comp['name']}] removed {comp['dest']}/")
    else:
        _log(f"[{comp['name']}] nothing to remove")


def ensure_all(force: bool = False) -> None:
    """Fetch every component if missing (or all, when ``force``)."""
    for comp in COMPONENTS:
        fetch(comp, force=force)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Fetch download-vendored MP3 libraries (mpg123, LAME).")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--force", action="store_true", help="re-fetch even if already present")
    g.add_argument("--clean", action="store_true", help="remove the downloaded source trees")
    g.add_argument("--check", action="store_true", help="report presence only; fetch nothing")
    args = ap.parse_args(argv)

    if args.clean:
        for comp in COMPONENTS:
            clean(comp)
        return 0

    if args.check:
        missing = [c["name"] for c in COMPONENTS if not is_present(c)]
        for comp in COMPONENTS:
            state = "present" if is_present(comp) else "MISSING"
            _log(f"[{comp['name']}] {comp['version']}: {state}")
        if missing:
            _log("Run `python3 initialize_files.py` to fetch: " + ", ".join(missing))
            return 1
        return 0

    ensure_all(force=args.force)
    _log("Done. Third-party MP3 sources are ready.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
