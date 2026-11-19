# thprac-linux

[thprac](https://github.com/touhouworldcup/thprac) is an unofficial fork with practice support for Touhou Koumakyou: New Classic (th06nc / NC), built to run on Linux through Wine/Proton.

The deliverable is a single Windows executable, `thprac-linux.exe`, which embeds a 32-bit launcher, a 64-bit NC module, and a Unix helper.

## About the upstream "Linux support"

The upstream [thprac](https://github.com/touhouworldcup/thprac) README states in its Compatibility section that it is "compatible with Wine and the Steam Deck." That only means the **Windows binary happens to run under Wine** — it is not native Linux support. In practice:

- Building is **Windows-only** (`msbuild thprac.sln`, Visual Studio, `cl.exe`); there is no way to build it on Linux.
- The README itself says the "place the exe next to the game" method **will not work with Steam games**.
- There is no way to attach to a game in a different Proton container or Wine prefix (it assumes a single wineserver).
- There is no mechanism to invoke Unix-side processing, so cross-container attachment is impossible by design.

This fork fills that gap:

- Cross-compiles on Linux with MinGW-w64 (`make`, with FreeType fetched automatically on the first build).
- Statically links the MinGW runtime DLLs away and packages everything into a single distro-friendly exe.
- Uses a static ELF helper (`thprac-xattach`) for all Unix-side work; the Windows side never runs Unix commands.
- Uses `--alongside-steam` + nsenter to attach to games in **other Proton containers**.
- Resolves the real executable from Steam shortcuts (`shortcuts.vdf`).
- Verifies downloads with the SHA-256 digest published on Releases for automatic updates.

In short: upstream claims Linux support but effectively only runs under Wine; this fork covers what is missing.

## Features

- Built-in NC (th06nc.exe) practice module (`thprac_th06nc.dll` + `thprac_bridge64.exe`)
- Cross-prefix attach for games in other Proton/Wine containers and prefixes
  - Unix-side work is handled by the static ELF helper (`thprac-xattach`)
  - The helper re-executes on the host via `--alongside-steam`, then enters the target container with nsenter / `steam-runtime-launch-client`
- Resident watchers to hook games as soon as they start
- Automatic updates from GitHub Releases with SHA-256 verification

## Building

Requirements:

- GNU make, cmake, python3
- `i686-w64-mingw32-g++`, `x86_64-w64-mingw32-g++`, `windres`
- Host `g++` (for the native ELF helper)
- `wine` (to generate the locale definitions)

```sh
make          # build everything (default target)
make nc       # rebuild only the NC module (dll + bridge)
make clean    # remove build/
```

On the first build, 32-bit and 64-bit FreeType are downloaded and built statically.

Outputs:

- `build/linux-x64/dist/thprac-linux.exe` — the launcher with all payloads embedded
- `build/linux-x86/payload/thprac-xattach` — the embedded static ELF helper

CI builds the same exe in `.github/workflows/nc.yml` and uploads it as the `thprac-linux` artifact.

## Layout

| File | Role |
| --- | --- |
| `thprac/src/thprac/thprac_th06nc.cpp` | NC practice module; the bridge is in the same file (`THPRAC_TH06NC_BRIDGE`) |
| `thprac/src/thprac/thprac_th06nc.h` | Shared NC definitions |
| `thprac/src/thprac/thprac_th06nc_map.h` | Generated stage/scene data |
| `thprac/src/thprac/thprac_wine_attach.cpp` | Windows side of cross-prefix attach |
| `tools/thprac_xattach.cpp` | Native Unix ELF helper (scan / apply / cmd / update) |
| `thprac/src/thprac/thprac_native_bundle.h` | Resource extraction and helper launch |
| `tools/make_payload_rc.py` | Generates payload resource IDs and the bundle ID |

The `FILES` order must stay in sync across `tools/make_payload_rc.py`, `tools/package_nc_single.py`, and `thprac_native_bundle.h`.

## Usage

Launch `thprac-linux.exe` with Wine or as a Proton launcher. Pick a game from the list and start it, or use the "Apply thprac to running game" button in the Tools tab.

When applying to a game in another Proton container, the helper re-executes on the host via `--alongside-steam` and enters the target container to attach. This may take a few seconds.

The version is defined by `THPRAC_FORK_VERSION` and `THPRAC_FORK_VERSION_NUMBERS` in `thprac/src/thprac/thprac_fork.h`.

## License and credits

- thprac itself: Copyright (c) 2022 Ack and thprac contributors, [MIT License](LICENCE)
- Third-party components: see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt). FreeType is used under the FTL (This software is based in part on the work of the FreeType Team).

Touhou Project and "Touhou Koumakyou: New Classic" belong to Shanghai Alice Generator / Shanghai Alice Reprise and their respective rights holders. This tool is an unofficial fan-made practice utility and is not endorsed by the game developers/publishers, the thprac maintainers, or TWC.
