@echo off
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if not "%~3"=="" goto usage
if not defined LOCALAPPDATA (
    echo LOCALAPPDATA is unavailable. 1>&2
    exit /b 2
)
set "game=%~f1"
if not exist "%game%\GLOBAL.BD" goto data
if not exist "%game%\SOUNDS.BD" goto data
if not exist "%game%\SWIV.CFG" goto data
if not exist "%game%\DEMO.SLV" goto data
if not exist "%game%\TERRAN00.SLV" goto data
if not exist "%game%\ANIMS\INTRO1.SMK" goto data
if not exist "%game%\ANIMS\INTRO2.SMK" goto data
if not exist "%~dp0swiv3d_player.exe" goto package
if not exist "%~dp0swiv3d_smacker.dll" goto package
set "profile=%LOCALAPPDATA%\OpenSWIV3D\SWIV.CFG"
if "%~2"=="" goto play
if not exist "%~f2" (
    echo Cannot find the supplied CD cue file. 1>&2
    exit /b 2
)
"%~dp0swiv3d_player.exe" "%game%\." --frontend --profile "%profile%" --cd-cue "%~f2"
exit /b %errorlevel%

:play
"%~dp0swiv3d_player.exe" "%game%\." --frontend --profile "%profile%"
exit /b %errorlevel%

:data
echo The game folder must contain the original archives, SWIV.CFG, levels, and ANIMS. 1>&2
exit /b 2

:package
echo Extract the whole Windows ZIP so the player and decoder DLL are beside Play.cmd. 1>&2
exit /b 2

:usage
echo Usage: Play.cmd "original game directory" ["optional CD cue file"] 1>&2
exit /b 2
