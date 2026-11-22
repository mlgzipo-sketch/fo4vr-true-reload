# True Reload for Fallout 4 VR

Reload your guns with your own hands, in the spirit of Into the Radius. The reload button only drops the magazine; everything else (taking a fresh magazine from your belt, guiding it into the well, racking the slide, opening a revolver or a break-action shotgun and loading it round by round) is done by hand.

Nexus page: https://www.nexusmods.com/fallout4/mods/111481

The plugin is still an **early alpha**. Bug reports with the log file (`Documents\My Games\Fallout4VR\F4SE\ManualReloadF.log`) are very welcome. In game, a click on the stick of the free hand puts a `MARK` into the log at that moment.

## Requirements

- F4SE VR
- Address Library for F4SE VR
- FRIK
- [ROCK](https://github.com/brunocatani/ROCK) 1.0 or newer (hands, fingers and physical objects)

## What is in this repository

| Path | What it is |
| --- | --- |
| `src/` | The F4SE plugin `ManualReloadF.dll` (C++23, CommonLibF4VR) |
| `src/ROCK/` | ROCK public API headers |
| `src/api/` | FRIK API and OpenVR headers |
| `Scripts/Source/User/` | Papyrus scripts that spawn the real magazines, rounds and casings |
| `tools/build_esp.py` | Writes `ManualReloadF.esp` (fixed FormIDs) |
| `zones/` | Per-gun zone files (magazine well, slide, hinge, ...), one per weapon FormID |
| `Meshes/`, `Textures/` | Models used by the plugin |
| `ManualReloadF.ini` | Settings, with comments |
| `tests/` | Unit tests of the pure geometry, run by CI on every build |
| `stubs/` | Minimal Papyrus stubs so scripts compile without the game's sources |

### Source layout

`ManualReload.cpp` still holds the main per-frame logic and is being split into modules step by step, with no change in behaviour:

- `Log`, `Settings`, `SafeMemory`: logging, the ini, guarded memory reads
- `NiMath`, `Angles`, `Paths`: vector/transform math, Papyrus angle conventions, arcs and paths
- `Gun`, `Nodes`: the equipped gun, its magazine kind, node helpers
- `State`: the reload state, real magazines lying around
- `Hands`: controllers, FRIK hands, HUD messages
- `Rail`, `Palm`, `Zones`: the magazine's way into the well, the palm on a part, the zone files
- `Objects`: real magazines, rounds and fired cases
- `ScriptBridge`: the command queue to the Papyrus script
- `RockLink`, `GripHook`: the link to ROCK and the grip input hook

## Building

Windows, Visual Studio 2022, CMake 4.2+, vcpkg:

```
git clone --recursive https://github.com/mlgzipo-sketch/fo4vr-true-reload
set VCPKG_ROOT=C:\path\to\vcpkg
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The GitHub Actions workflow (`.github/workflows/build.yml`) builds the DLL, runs the unit tests (`tests/`, the geometry of transforms and of the ways drawn for each gun), compiles the scripts with [Caprica](https://github.com/Orvid/Caprica), writes the ESP and packs a ready-to-install zip. Each build of a branch is published to the `dist/<branch>` branch.

## License

GPL-3.0, see `LICENSE`. Third-party code keeps its own license: CommonLibF4VR (MIT, `extern/CommonLibF4VR`), OpenVR headers (BSD-3-Clause, Valve), FRIK API header, ROCK API headers.

## Credits

- devartificial (Bruno Catani) for ROCK
- The FRIK team for the VR body
- ArthurHub and contributors for CommonLibF4VR
- The F4SE VR team and the Address Library authors
- Into the Radius for the inspiration

This mod was made with the help of AI (Claude by Anthropic); design, testing and tuning in VR by the author.
