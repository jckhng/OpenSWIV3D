# OpenSWIV3D

OpenSWIV3D is an unofficial native reimplementation of the DOS version of
SWIV 3D. Version 0.1.0-alpha.5 is a Windows x64 testing build. It loads the
game files from your own original installation.

The prepared public alpha includes source and Windows x64 archives. See
[CHANGELOG.md](CHANGELOG.md) for the release files, validation and open checks.

## Play on Windows

1. Extract the entire ZIP to a writable folder.
2. Locate your original game directory containing `GLOBAL.BD`, `SOUNDS.BD`,
   `SWIV.CFG`, the `.SLV` level files, and the `ANIMS` directory.
3. Drag that directory onto `Play.cmd`, or run the command below in PowerShell
   from the extracted folder.

```powershell
.\Play.cmd 'D:\Games\SWIV'
```

Pass your original CD image's CUE file as a second argument to enable CD music.
Keep the referenced BIN file beside the CUE.

```powershell
.\Play.cmd 'D:\Games\SWIV' 'D:\Games\SWIV-CD\swiv3d.cue'
```

Create a pilot, choose Keyboard and use the bindings shown in Setup.
The default keyboard uses the arrow keys to fly and turn, comma and period to
strafe, and Space to fire the primary weapon. Pause opens the pause panel and
a fresh key resumes. Escape opens the mission-abort confirmation with No
selected. Use the window close button or Exit to save. The launcher stores
pilots in `%LOCALAPPDATA%\OpenSWIV3D\SWIV.CFG`.
The player reads the original files in place.
Continue reloads the saved mission checkpoint from its beginning. A pilot who
has never cleared the first mission receives the start option on relaunch.

## Test later missions

At New Player, enter `SWIV!AndLetDie`, press Enter, then enter an ordinary
pilot name. Choose Test Levels from the main menu. F12 opens the original
three-page equipment menu during test play. Test Levels resets inventory for
each mission and follows the original test-mode profile rules.

Hold keypad minus or plus during play to resize the viewport.

## Alpha limitations

- A normal-inventory clear and carry into the next mission, and an
  uninterrupted run through all 17 missions, are unverified. Controlled
  boss, vehicle and successor tests cover selected later-game paths.
- Exact mixed audio, display and interrupt timing are still under
  investigation. XInput has injected-input coverage. Physical controllers
  need playtesting.
- The interactive host is Windows x64. Linux supports the portable core and
  headless tests.
- Further original shortcut, menu and presentation branches need coverage.
  Queued menu confirmation has the original four-draw guard. Editor input
  and cancellation follow their owner-specific rules.

For a bug report, include the version, Windows version, original data edition,
mission, craft, controller type, and steps to reproduce. Keep original files,
personal profiles and captures private.

[Build from source](BUILDING.md) Â· [Distribution notices](NOTICE.md)

## License

Project-authored code and documentation use MIT. libsmacker retains LGPL 2.1
or later. Original game resources and trademarks retain their owners' rights.
See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).

Contributors can find source ownership, testing and release preparation in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).
