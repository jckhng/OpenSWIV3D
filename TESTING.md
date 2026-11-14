# Windows alpha smoke test

On a separate Windows x64 system, extract the ZIP to a path with spaces and
leave the original game directory in a separate location. Use a fresh Windows
user profile for the test.

1. Run `Play.cmd` with the game directory. Confirm the introduction plays
   and pilot selection opens.
2. Create a keyboard pilot, start a new game and let the first mission draw.
   Confirm that the image stays visible, the helicopter responds, and firing
   produces a visible shot and audible effect.
3. Close the window. Confirm that `%LOCALAPPDATA%\OpenSWIV3D\SWIV.CFG`
   was created. Relaunch, reselect the same pilot and start the mission again.
   A pilot who has never cleared the first mission receives the start option.
4. With a pilot that has a later-mission checkpoint, close and relaunch, then
   use Continue. Confirm that it loads that saved mission from its beginning.
   Record whether the checkpoint came from play or a supplied compatible CFG.
5. Die during a mission, acknowledge the Game Over panel, then start or Continue
   with the same pilot. Confirm that mission initialization and controls work.
6. Confirm that the original `SWIV.CFG` is unchanged. Try the optional
   CUE path and record whether CD music plays if the image is available.

Record the package SHA-256, Windows version, data edition and result for each
step. Include controller and audio hardware if you used them.
