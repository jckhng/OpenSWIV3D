# libsmacker

Decoder sources from JonnyH/libsmacker revision
`ae8d4c9ec07b24d43ccff184d6e512bae793dfd1`.

Upstream source is <https://github.com/JonnyH/libsmacker>.
Copyright Greg Kennedy. Licensed under LGPL 2.1 or later, reproduced in COPYING.
The CMake build links this decoder as a separate shared library.

Local changes on 10 November 2026 allow overlapping palette-copy indices
because the source is a separate previous-palette buffer. They also propagate
palette decode failures and validate the palette record length. A synthetic
overlap regression covers the case reached by END_LUN and INTRO2.

On 13 November 2026, a logical source filename was added for diagnostics.
Allocation and read errors report `smacker.c` and its original line number.
Build-machine paths are excluded from the decoder DLL.

On 14 November 2026, dated local change notices were added to `smacker.c`.
The logical filename and diagnostic line numbering remain unchanged.
