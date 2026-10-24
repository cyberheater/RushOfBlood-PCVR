# How this play folder was first built

This repository is [RandomName729/RushofPCVR](https://github.com/RandomName729/RushofPCVR) at
0.4.1 (`b7dcd7c`), plus the commits on top of it:

1. **Add Quest controllers as PlayStation Move wands**: `patches/0001-*.patch`
2. **Add a per-frame performance trace** (`perf_trace=1`, `tools/perf-report.py`): `patches/0002-*.patch`
3. **Let the Move aim angle go below zero** (-55 / 100% / 100% recommended): `patches/0003-*.patch`
4. The report's corrected verdicts, a black point and gamma for the headset picture
   (`black=`, `gamma=`), and the settings tested on a Quest 3 as the defaults. These are
   commits only: there are no patch files for them.

`Build RushVR.bat` and `build-rushvr.ps1` are the scripts the first build was made with. They
clone the upstream repository and apply patch 0001 only. To build this repository as it is,
build `shadps4-arm64-main` the way `build-rushvr.ps1` does (from "Building the emulator" on),
in a clone of this repository.

`pc-vr/shadps4.exe` is the emulator built from this source (Windows x64, clang-cl Release).
Put the game's dump (CUSA02350, your own copy) in a `games` folder next to `pc-vr`, and start
it with `Play Rush of Blood.bat`.

Known issue: eye width 2880 (High) crashes after a while in this build, so 2304 (Medium) is
the default. The pipeline cache (`pipeline_cache_enabled` in `pc-vr/user/config.json`) has to
stay off. It saves pointers that are stale in the next run, and the emulator then crashes at
start.
