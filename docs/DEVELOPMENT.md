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

`third_party/libsmacker` retains its upstream identity, license and dated
local change notes. The player links it as a replaceable shared library.
Preserve those notices and ship matching decoder source with Windows builds.

Preserve recovered integer widths, update/render ordering, shared storage,
clock domains and resource decoding. Inspect existing callers and tests
before changing behavior. Run focused regressions and the complete synthetic
CTest suite. Original-resource tests require a developer-supplied installation.
Report controlled fixtures and live play separately when describing coverage.

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

For this alpha, prepare tag `v0.1.0-alpha.5` and a GitHub prerelease titled
`OpenSWIV3D 0.1.0-alpha.5`, using CHANGELOG.md as the release description.
Attach the two ZIPs, SHA256SUMS.txt and release-index.json from the reviewed commit. Check their
hashes against the prepared release before publication. GitHub-generated
source archives come from the committed tree.

Public testing should retain the open checks listed in CHANGELOG.md.
