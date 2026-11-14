# Distribution notices

OpenSWIV3D is an unofficial reimplementation. The MIT license in
`LICENSE` covers project-authored code and documentation. The dos-re-harness
copyright notice records the source's development origin.

Original SWIV 3D executables, archives, levels, art, movies, music and
trademarks belong to their respective owners. Obtain the game separately.
The distribution excludes original executables, resource archives, levels,
artwork, movies, music, emulators and private captures. The implementation
contains recovered behavior constants and numerical tables.

## libsmacker

`swiv3d_smacker.dll` contains libsmacker by Greg Kennedy, licensed under the
GNU Lesser General Public License, version 2.1 or later. The Windows ZIP
includes the corresponding decoder source, local change notes and `COPYING`
under `reimplementation/third_party/libsmacker`. In the Windows ZIP, that
directory has a standalone CMake build for the x64 DLL. A user can replace
the DLL with an interface-compatible build of the library.
