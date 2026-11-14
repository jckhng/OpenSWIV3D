# Build OpenSWIV3D

The source builds independently of the DOS RE Harness. Tests registered with
CTest use synthetic fixtures and need no game files.

## Windows x64

Use CMake 3.21 or later with Visual Studio 2022 C++ Build Tools and the Windows
SDK. Python 3.9 or later is required for the ZIP packager. The alpha uses the
statically linked Microsoft C/C++ runtime.

```powershell
cmake -S reimplementation -B build -G 'Visual Studio 17 2022' -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python tools/package_alpha.py windows --build-dir build/Release --out-dir dist
```

Keep `swiv3d_smacker.dll` beside `swiv3d_player.exe`. The packaged
`Play.cmd` starts the interactive player and selects a separate profile.

## Headless replay

The player accepts a frontend script, held-input movie and fresh output directory.
Use `--campaign` for retained menus, combat and mission transitions. Supply
`--profile` with a separate writable CFG path for save/reload checks. An existing
profile selects its first pilot and starts or continues that pilot's checkpoint.

```powershell
.\build\Release\swiv3d_player.exe 'D:\Games\SWIV' startup.input.txt flight.input.txt replay-output --campaign --final-only --profile qa\SWIV.CFG
```

`--final-only` retains initial and final captures while simulating and rendering
every update. The default retains a capture after every input segment. Headless
replay skips movies and uses scripted key edges. Desktop input, window output
and physical audio-device scheduling require interactive checks.

## Linux core and tests

```sh
cmake -S reimplementation -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```
