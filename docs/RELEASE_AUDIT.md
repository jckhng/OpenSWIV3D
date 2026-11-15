# Source and release audit

Reviewed on 15 November 2026 for OpenSWIV3D 0.1.0-alpha.6.

## Privacy scope

The review covers the tracked source, all reachable Git commit/tag/tree/blob
objects and the prepared source/Windows ZIP contents. Checks include original
resources, captures, local configuration filenames, personal filesystem paths,
private keys, common service-token formats and authenticated URLs. Private
reference-work, build and packaging directories are ignored by Git and excluded
from the package inputs.

The initial unpublished commit and tag contained a personal email address.
The owner approved `jckhng@users.noreply.github.com`. Author, committer and tagger
metadata now use that identity, and the release index/checksum bindings were
refreshed. Private local backup/reflog storage is outside the reviewed public
Git references. The scan reports no remaining findings in its declared scope.
Pattern scans provide bounded detection and need manual review of new files.

## License inventory

| Material | Terms and review |
| --- | --- |
| Project-authored source/documentation | MIT, with OpenSWIV3D and dos-re-harness copyright notices in LICENSE |
| libsmacker | LGPL 2.1 or later, pinned upstream revision, original copyright/license headers, dated modification notice and COPYING |
| MSVC C++ standard library in Windows object code | Apache 2.0 with LLVM exception. The exception addresses embedded compiled portions |
| Microsoft compiler/runtime components | Distribution is governed by the applicable toolchain terms. The alpha uses the Release static runtime |
| Original resources and recovered behavior | Original game resources are user-supplied. NOTICE identifies recovered behavior constants and numerical tables |

The MIT text matches the standard grant and warranty terms with project
attribution. [OSI MIT text](https://opensource.org/license/mit)

The decoder's headers and COPYING match the pinned upstream revision. Its
documented local changes cover palette handling and logical diagnostic names.
The source/runtime packages include matching decoder source, change notes and
the license. The Windows package supplies a standalone decoder CMake build.
A separate DLL rebuild/replacement check passes. The application uses a runtime
DLL import, and its MIT terms permit user modification and debugging.
[libsmacker upstream](https://github.com/JonnyH/libsmacker/tree/ae8d4c9ec07b24d43ccff184d6e512bae793dfd1)

The original user-supplied credit pages display copyright notices. The audit
found that the native runtime lacked a libsmacker copyright/license reference.
The native window title now includes Greg Kennedy's copyright, LGPL version
and a pointer to NOTICE.md, which identifies the included COPYING file.
A regression captures the production window creation title through mocked
Win32 calls. LGPL 2.1 section 6 contains the runtime-notice and shared-library
conditions. [GNU LGPL 2.1](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)

The installed C++ standard-library header identifies Apache 2.0 with LLVM
exception. Microsoft's published license describes the compiled-object
exception. Compiler installation or publisher license entitlements are outside
this source-file audit. [MSVC STL license](https://github.com/microsoft/STL/blob/vs-2022-17.14/LICENSE.txt), [Microsoft redistribution terms](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)

Recovered material includes the 256-vector combat jitter sequence, weapon
patterns and numerical device/timing models. These have functional provenance
from original observation and analysis. This review establishes their presence
and declared purpose. It does not establish blanket ownership or legal clearance
for every recovered value or translated expression. Copyright treatment of
functional aspects and protected expression requires a fact-specific assessment.
[US Copyright Office Circular 61](https://www.copyright.gov/circs/circ61.pdf)

## Cleanup assessment

The implementation has roughly 23,000 C/C++ lines including tests and the
vendored decoder. Existing modules separate assets, frontend, mission/combat,
rendering, audio and native adapters. A broad architectural rewrite has little
release benefit compared with the outstanding interactive checks in CHANGELOG.

Useful subsequent work is incremental.

1. Extract phase-specific private handlers from `Frontend::Impl::press` in
   `src/frontend.cpp`. Keep menu poll, key ownership, clock and save ordering
   covered by retained regressions.
2. Decompose the approximately 290-line mission setup dispatcher in
   `src/mission.cpp` into bounded command handlers on the same owner. Preserve
   actor allocation, parent references and initialization order.
3. Consolidate arithmetic/parsing helpers only after checking each caller's
   signedness, overflow and truncation contract. Existing flight/combat helpers
   use different overflow handling.
4. Address existing compiler warnings through small verified changes. Keep the
   vendored decoder's upstream diff and attribution easy to inspect.

The pre-alpha changes are the identity/privacy correction and runtime library
notice. Broader cleanup can follow tester feedback and additional control tests.
