# Development

Run build and packaging commands from the repository root. BUILDING.md
contains Windows and Linux commands. TESTING.md records the interactive
smoke procedure, and CHANGELOG.md describes the alpha's validation scope.

## Source ownership

`reimplementation/` owns the portable C++17 implementation and native hosts.
`src/input_script.cpp` and `src/campaign.cpp` coordinate retained frontend,
mission, movie and profile transitions. `src/mission.cpp`, combat/vehicle
modules and rendering modules own the simulation and presentation.
`src/profile_file.cpp` handles filesystem profile I/O. Windows window, input
and audio adapters are in the corresponding `*_win32.cpp` files.

`Frontend::Impl::press` dispatches raw input to editors and mission shortcuts.
Its private menu handler normalizes aliases before cancellation or confirmation.
`Mission::Impl::load_level` and `setup_player` own level loading and initial
player construction within the retained mission state.

`third_party/libsmacker` retains its upstream identity, license and dated
local change notes. The player links it as a replaceable shared library.
Preserve those notices and ship matching decoder source with Windows builds.

Preserve recovered integer widths, update/render ordering, shared storage,
clock domains and resource decoding. Inspect existing callers and tests
before changing behavior. Run focused regressions and the complete synthetic
CTest suite. Original-resource tests require a developer-supplied installation.
Report controlled fixtures and live play separately when describing coverage.

## Regression checks

Run the synthetic CTest baseline using the commands in BUILDING.md. With an
original installation available, also run the retained controller/session and
movie-resource checks from the repository root.

```powershell
$game = Read-Host 'Original game directory'
.\build\Release\swiv3d_controller_session.exe $game
.\build\Release\swiv3d_cutscene_tests.exe --resources $game
```

The corresponding Linux commands use the build directory from BUILDING.md.

```sh
./build/swiv3d_controller_session "$game"
./build/swiv3d_cutscene_tests --resources "$game"
```

Set `game` to the original installation directory before running the Linux
commands. Private reference comparisons use fixed original captures and the
same declared input conditions for each revision. Record the compared state,
pixels, palette or PCM and retain a negative control with each comparison.

## Private reference work

Original-reference experiments use a separate private DOS RE Harness workspace.
Local data paths, original resources, profiles, captures, emulator state and
machine-specific configuration belong under ignored `.work/` directories.
The runtime and ordinary source build use this standalone repository.

## Release preparation

```powershell
python tools/package_alpha.py source --out-dir dist
python tools/package_alpha.py windows --build-dir build/Release --out-dir dist
```

Packaging uses an explicit source/runtime allowlist and fresh output paths.
The archives include license notices and corresponding decoder source.
Review staged files and Git history for imported resources, personal paths
and local configuration before pushing the repository.

For this alpha, prepare tag `v0.1.0-alpha.6` and a GitHub prerelease titled
`OpenSWIV3D 0.1.0-alpha.6`, using CHANGELOG.md as the release description.
Attach the two ZIPs, SHA256SUMS.txt and release-index.json from the reviewed commit. Check their
hashes against the prepared release before publication. GitHub-generated
source archives come from the committed tree.

Public testing should retain the open checks listed in CHANGELOG.md.
