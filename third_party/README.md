# third_party

Vendored third-party libraries. How each one is obtained differs by what
upstream offers:

| Dir | Library | License | How it gets here |
|-----|---------|---------|------------------|
| `libusb/` | libusb | LGPL-2.1 | git submodule (`git submodule update --init`) |
| `flac/` | libFLAC | BSD-3-Clause | git submodule (`git submodule update --init`) |
| `mpg123/` | libmpg123 (MP3 decode) | LGPL-2.1 | **downloaded** by `initialize_files.py` — *not committed* |
| `lame/` | LAME / libmp3lame (MP3 encode) | LGPL | **downloaded** by `initialize_files.py` — *not committed* |
| `mpg123-config/`, `lame-config/` | — | — | committed: the build-generated `config.h` for the above |

## MP3 libraries are fetched, not committed

mpg123 and LAME have **no official Git repository** (upstream is SVN + release
tarballs), so a submodule from the real source is impossible and committing a
third-party mirror would defeat "latest from *its* source". Instead their source
is downloaded on demand:

```sh
python3 initialize_files.py          # fetch mpg123 + LAME into third_party/
python3 initialize_files.py --check  # report presence
python3 initialize_files.py --clean  # remove the downloaded trees
```

Run it **once after cloning** (and after a version pin changes). The tarballs are
sha256-verified before extraction, and only the library subset is kept — never
the GPL `mpg123`/`lame` command-line tools. The Android CMake build hard-errors
with this instruction if the source is missing. Version pins, URLs, checksums,
and update steps all live in `initialize_files.py`.

## Submodules

`libusb` and `flac` are git submodules — after cloning:

```sh
git submodule update --init --recursive
```
