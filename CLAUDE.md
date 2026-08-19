# CLAUDE.md

Guidance for Claude Code sessions working in this repository. Keep this file truthful: list a command
only in the commit that makes it work, keep the "Current state" section matching the repo, and update
the Roadmap markers here whenever a milestone's state changes.

## Project

DOCXtoMD is a native Windows console application (C++, MSVC v143 / Visual Studio 2022) that converts
`.docx` (Office Open XML / WordprocessingML) files into GitHub-Flavored Markdown `.md` files, built
from scratch with no external converter tools **and — since D1/D2 — no third-party code at all**.
Two documents govern all work:

- `GDC_GCS_v1_1_4.md` — **Guild Coding Standard v1.1.4 (GCS)**. `CONTRIBUTING.MD` makes it mandatory:
  all submissions are reviewed against it. **Read it in full before writing or modifying any C++ code.**
- `docs/CONVERSION_REFERENCE.md` — the full DOCX→Markdown domain specification (OPC container,
  WordprocessingML element inventory, feature→GFM mapping table, escaping rules, edge cases ranked
  by real-world frequency, pipeline design). **Read the relevant sections before implementing any
  conversion milestone.**

Six owner-authored shared headers live in `include/` and supply the GCS substrate (aliases,
allocators, SIMD helpers, spin locks) that new code is expected to build on — see "Shared headers"
below.

## Current state (do not assume more exists)

- `DOCXtoMD.cpp` — placeholder entry point, 25 lines: the r17 prolog (v0.1.0, `ISA: Scalar`,
  `Thread-safety: Reentrant`), D4's `#ifndef __AVX2__` + `#error` guard, `#include "typedefs.h"` and
  `si32 main() { return 0; }`, with a one-line note that r11 does not reach the name — the entry
  point is spelled by the language, not chosen, so it is not an en3 deviation and needs no
  `RULE-DEV` tag. `wmain` is the same case at M2. The Visual Studio default comment and `#include <iostream>` are gone.
  **Prolog and guard are both temporary**: M2 carries them into `src/main.cpp` and
  `src/BuildGuards.h` and deletes this file.
- `DOCXtoMD.sln` — **exists** (VS 17.14, UTF-8 BOM, CRLF) and exposes **only** `Debug|x64` and
  `Release|x64`, matching the project file exactly.
- `DOCXtoMD.vcxproj` — v143, Unicode, Console, `/W3`, SDLCheck, ConformanceMode, Release
  WholeProgramOptimization. Declares **two** `ProjectConfiguration`s, `Debug|x64` and `Release|x64`
  — **D3 is executed**: every `Win32` `ProjectConfiguration`, `PropertyGroup`, `ImportGroup` and
  `ItemDefinitionGroup` is gone, and `<Keyword>Win32Proj</Keyword>` is the standard VS project
  keyword, not a platform. Both configs set `<LanguageStandard>stdcpp20</LanguageStandard>` +
  `<LanguageStandard_C>stdc17</LanguageStandard_C>` and
  `<AdditionalIncludeDirectories>$(ProjectDir)include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>`,
  so any TU writes `#include "typedefs.h"` with no path prefix. Both configs also carry
  `<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>` — **D4 is
  applied**, so both x64 configurations compile with `/arch:AVX2`; that it *builds* is unverified,
  see M1's marker. No OutDir override. Six `<ClInclude>`s, all `include\…`.
- `DOCXtoMD.vcxproj.filters` — lists `DOCXtoMD.cpp` under Source Files and all six headers under
  Header Files. Every `<ClInclude Include="…">` path matches the `.vcxproj` character-for-character;
  keep it that way, or the IDE tree stops reflecting the build.
- Shared headers in `include/` — all six listed as `<ClInclude>` in the `.vcxproj` and under Header
  Files in the `.filters`, all CRLF, all tab-free, none exceeding 150 columns:
  - `typedefs.h` v1.0.1 — r1/r2/t1/t2 aliases, the full pointer lattice, `al1`–`al64`, `$LoopMT*`,
    `defpa`/`refpa` (m1/m2). r17 prolog, but `ISA: Scalar | SSE4.2 | AVX2 | AVX512` — `AVX512` is
    not a valid r17 token (see Known gaps).
  - `memory management.h` v1.2 — the p2 allocator family: `amalloc`/`salloc`/`mdealloc`,
    `malloc1..64`, `declare1d16/32/64`, `zalloc*`, `mzero`/`mset`, `Copy*`/`Stream*`, and the
    interlocked `LockedCopy`/`LockedSwap`/`LockedMoveAndClear` (relevant to D6's shared state).
    r17 prolog; `ISA: Scalar | SSE4.2 | AVX2 | AVX-512`.
  - `common functions.h` v1.1 — constants, `Min`/`Max`, `AllTrue`/`AllFalse`,
    `RoundUp/DownToNearest4..64`, sincos, `Idle`. r17 prolog; `ISA: Scalar | SSE4.2 | AVX2`.
  - `SIMD management.h` — `namespace simd` FMA wrappers (`fmadd_ps`/`fmsub_ps`/`fnmadd_ps`, 128- and
    256-bit). **Pre-r17 boxed banner**, no `ISA:` field.
  - `vector structures.h` — `VEC*`/`SSE*`/`AVX*` unions and vector structs. **Pre-r17 boxed banner**,
    no `ISA:` field.
  - `spinlocks.h` v1.0.0 — user-space spin locks: `SpinLockMin`/`SpinLock`/`SpinLockMax` (long-wait,
    balanced and minimum-latency profiles), `SpinLockTry`, `SpinUnlock`, and the `SPIN_*` tuning
    constants. r17 prolog; `ISA: AVX2` (but see Known gaps); `Thread-safety: MT-safe`. It carries an
    `#ifndef __AVX2__` + `#error` guard of D4's shape — with a `spinlocks.h`-specific message and an
    extra `static_assert` D4 ruled out. **The sanctioned lock for the one-thread-per-file worker
    layer** (D6) — not for use inside a single document's conversion.
- Tooling and process files, all five added by M1 and all CRLF except `CHANGELOG.md`:
  - `.clang-format` — tc1's keys verbatim, plus five groups that stop the formatter breaking a
    rule stated elsewhere: `AlignConsecutiveMacros: Consecutive` (r12), `SpaceBeforeParens: Never`
    (r13), the three `AllowShort*OnASingleLine` keys that keep r3/r4 same-line statements from
    being exploded onto separate lines, `SortIncludes: Never` (the shared headers have a
    load-bearing include order — see "Shared headers"), and `ReflowComments: false` (an r17 prolog
    is regex-validated byte-for-byte and must not be rewrapped). `NamespaceIndentation: All`
    matches `SIMD management.h`. **Known limit**: clang-format has no option that keeps two
    statements on one line, so r4's three-space form — `va_list val;   va_start(val, pointer);` as
    `memory management.h` writes it — is split no matter what this file says. Format a file
    carrying that idiom only if you mean to lose it.
  - `include/.clang-format` — `DisableFormat: true` (plus `SortIncludes: Never`, because a
    directory `.clang-format` **replaces** the parent rather than merging with it). Without it the
    repository style rewrites the owner-authored headers by thousands of lines — 874 in
    `typedefs.h`, 986 in `vector structures.h` — which "Shared headers" forbids. With it,
    formatting all six is a verified no-op.
  - `.editorconfig` — tc2's four properties plus `indent_style = space` from r8, with four
    `RULE-DEV`-tagged exemptions: Markdown and `LICENSE` keep their authored line endings; `*.sln`
    keeps Visual Studio's tab indentation; and `*.sln` and `*.filters` are `charset = utf-8-bom`
    because both ship with a BOM and EditorConfig's plain `utf-8` means *no* BOM, so an honest
    `[*]` charset would strip it on the next save (`DOCXtoMD.vcxproj` has no BOM and is unaffected).
    The Markdown glob is `[*.{md,MD}]`: EditorConfig globs are **case-sensitive**, so a bare
    `[*.md]` silently misses `CONTRIBUTING.MD` and leaves that owner-managed LF file on `crlf`.
  - `.gitattributes` — see "Line endings" below.
  - `CHANGELOG.md` — c2/c3 Keep-a-Changelog, `[Unreleased]` only; nothing is released yet.
  None of the five is a `<ClCompile>`/`<ClInclude>` candidate, so the MSBuild file-list rule does
  not reach them and neither project file mentions them.
- `GDC_GCS_v1_1_4.md`, `CONTRIBUTING.MD`, `docs/CONVERSION_REFERENCE.md`, `LICENSE`
  (MIT, Copyright (c) 2026 David William Bull), this file.
- Line endings: `.gitattributes` now holds the line, so this no longer needs checking by hand.
  Source and build files (`*.c`, `*.cpp`, `*.h`, `*.hpp`, `*.inl`, `*.sln`, `*.vcxproj`, `*.filters`,
  `*.props`, and the three tooling dotfiles) are `text eol=crlf`: Git stores LF and materialises
  **CRLF** in every working tree, on Linux exactly as on Windows, so tc2 cannot drift and a
  line-ending change can never reach a diff. Everything else is `* -text` — byte-for-byte as
  committed, whatever `core.autocrlf` a contributor has set — which is what leaves the Markdown docs
  (`CLAUDE.md`, `docs/CONVERSION_REFERENCE.md`, `CONTRIBUTING.MD`) LF, `GDC_GCS_v1_1_4.md` CRLF and
  `LICENSE` LF. The M1 commit ran `git add --renormalize .` so the stored blobs agree with the new
  attributes and nothing shows as modified; **what a checkout produces is byte-identical to before**,
  including for the six owner-authored headers, whose content was not touched.
- `docs/` **exists** and holds `CONVERSION_REFERENCE.md`; `include/` **exists** and holds the six
  shared headers. Neither is planned-only any more.
- **Not yet created** (GCS obligations, see Roadmap): `src/`, `tests/`, `bench/`, CI. Do not
  reference them as if they exist.

## Build & run

MSVC only; there is no CMake. A `.sln` now exists and is x64-only, so either invocation is fine —
build from the repo root (VS Developer prompt, or run `vcvarsall.bat x64` first in plain cmd):

```bat
msbuild DOCXtoMD.sln     /m /p:Configuration=Release /p:Platform=x64   &:: canonical build
msbuild DOCXtoMD.vcxproj /m /p:Configuration=Release /p:Platform=x64
msbuild DOCXtoMD.vcxproj /m /p:Configuration=Debug   /p:Platform=x64
msbuild DOCXtoMD.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

Default output paths (no OutDir override): `x64\Release\DOCXtoMD.exe`, `x64\Debug\DOCXtoMD.exe`.
**x64 is the only supported platform** — GCS a2 declares 32-bit unsupported, and D3 is **executed
and verified on Windows**: the Win32 configurations are gone from `DOCXtoMD.vcxproj`, and
`/p:Platform=Win32` fails instead of building. A bare `msbuild DOCXtoMD.vcxproj` with no
`/p:Platform` fails the same way and for the same reason — MSBuild defaults `$(Platform)` to `Win32`
for `.vcxproj`, so it lands on the missing configuration and reports MSB8013. That is the guard
working, not a broken project file: pass `/p:Platform=x64`, or build the `.sln`, whose default
configuration is `Debug|x64`. Do not add the Win32 configurations back, and do not add a new
platform without a new numbered decision.

**Linux/remote sessions cannot run MSVC — nothing in this project can be compiled or executed there.**
What you can still verify on Linux: `.vcxproj`/`.filters`/`.sln` XML/text well-formedness and mutual
sync, GCS mechanical rules (indent, tabs, line width, prolog regexes, CRLF), and any Python
fixture/golden scripts. A `g++ -std=c++20 -fsyntax-only` smoke check is **not** available for anything
touching the shared headers — they are MSVC-specific (`__declspec(align)`, `__vectorcall`, `__int64`,
`__bfloat16`, `<windows.h>`, `_aligned_malloc`). MSVC v143 is the only supported compiler. **Never
claim the build passes when you could not run msbuild; state exactly what was and was not verified.**

### MSBuild file-list rule (silent-failure trap)

MSBuild compiles **only** files listed in the `.vcxproj` — there is no globbing. Every new `.cpp`
needs a `<ClCompile Include="..."/>` and every new `.h` a `<ClInclude Include="..."/>` in
`DOCXtoMD.vcxproj`, plus a matching entry in `DOCXtoMD.vcxproj.filters` (the `.filters` file only
affects the IDE tree, but a mismatched entry breaks project load in VS). Update both **in the same
commit** that adds the file, and keep the two `Include=` paths byte-identical. Headers live under
`include\`, which is on the compiler's include path via `<AdditionalIncludeDirectories>`, so
`Include=` attributes carry the `include\` prefix while `#include` directives in code do not.

## Coding standard (GCS v1.1.4) — the rules you will otherwise break

`GDC_GCS_v1_1_4.md` is the single source of truth; rule IDs below cite it. This cheat sheet exists
because standard C++ habits violate nearly all of these. Intentional deviations must be tagged
`// RULE-DEV:<rule-id> <why>` (en3) — never deviate silently.

| Rule | Requirement |
|---|---|
| r8 | Indent **3 spaces**. Never tabs. |
| e2/r7 | Lines ≤150 columns; hard cap 180. |
| r1 | Width/sign-encoded scalar aliases only: `ui8 ui16 ui32 ui64`, `si8 si16 si32 si64`, `fl32 fl64`. CI bans new `f32`/`f64` spellings (en2). All live in `typedefs.h`. |
| r2/t2 | const/volatile and indirection live in **typedefs, not identifiers**: `cui32` = `const ui32`, `ui32ptr` = `ui32*`, `cui32ptr` = `const ui32*`, `ui32ptrc` = `ui32* const`, `cui32ptrc` = `const ui32* const`. Leading `c` binds the pointee, trailing `c` binds the pointer, repeat per indirection. `typedefs.h` carries the full lattice including the `void*` family (`ptr`, `cptr`, `vptr`, `ptrc`, `cptrc`, `vptrc`, `ptrptr`, …). |
| t1 | Vector aliases (`ui256`, `fl32x8`, `fl64x4`, `ui512`, `fl32x16`, `fl64x8`, …) — **live, not dormant**; see the ISA baseline below. |
| t3 | Never mix alias forms with raw `const T*` style in the same TU (CI-checked, en2). |
| m1/m2 | Pointer-array macros `defpa`/`defpa2`/`defp1a1` and casts `refpa`/`refpa2` come from `typedefs.h` — do not re-roll them (`refp1a1` is commented out upstream). |
| r11 / r12 | Functions **PascalCase**; tables/macros/global constants **UPPER_SNAKE**. |
| r13 | Control structures: no space before `(`, exactly one space after each `;` — `if(x)`, `for(ui32 i = 0; i < n; ++i)`. |
| r14/r15 | `{` on the same line as the control statement / function signature (functions: **exactly one space** before `{`; never on its own line). `}` on its own line, except a function body that fits on the signature line within e2 may close there. |
| r3/r4 | Spreadsheet-style padding where it locally helps readability; same-line statements only when r3 justifies them, separated by **exactly three spaces**. |
| r5/r6/d1 | `///` with `@param`/`@return`/`@tparam`/`@note` for API docs only (public APIs require it); `//` for notes; `//==`/`//--` grouping headers. Disable >5 lines of code with `/* */`, else `//`. |
| r17 | Every source file opens with the validated prolog (template below). |
| c1/c2 | **No history in prologs** — record changes in root `CHANGELOG.md` (`[Unreleased]` + Added/Changed/Fixed/Removed/Perf per c3). |
| p1 | `inline` in headers only when profile-hot and ODR/size safe; else in `.cpp`. |
| p2 | Explicit alignment-aware allocators with matching frees — **the family already exists** in `memory management.h` (`amalloc`/`salloc`/`mdealloc`, `malloc16/32/64`, `mzero`/`mset`). Use it; do not write a new allocator and do not call bare `new`/`malloc`. |
| p3/a2/a3/a8 | AVX2 is the ISA floor; threading is one thread per input file and nothing finer — see the next subsection (D4/D5/D6). |
| p4/bd1/bd2 | Performance-over-idiom, but every performance **claim** needs a benchmark diff in `bench/`; acceptance = ≥3% win or parity with meaningful simplification. |

**GCS sections that do NOT apply here:** g1–g10 (GPU/shader — this tool has no GPU code; the GPU gates
named in en1/en2 are no-ops). The graphics halves of a2/a6.

### ISA and threading baseline (D4 + D5 + D6) — the AVX2 floor, one thread per file

Three rulings govern this: **AVX2 is the ISA floor** (D4, adopting a3), **no sub-baseline fallback**
(D5, whose threading half D6 narrowed), and **threading exists only at the file level — one thread
per input file** (D6). Together they settle into five operative rules:

1. **AVX2 is the floor, unconditionally.** x64 builds compile with `/arch:AVX2`
   (`<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>`), which
   on MSVC also licenses FMA3 and BMI/BMI2 codegen — a2's `AVX2+FMA3+BMI2` baseline. There is no
   scalar-only build, no SSE-only build, and no `#ifdef` ISA fork (a11). A compile-time
   `#ifndef __AVX2__` + `#error` guard fails the build if the flag is ever lost (that is an *error*
   guard, not a behavior fork).
2. **Runtime dispatch may only go up, never down** (a8): an AVX-512 microkernel above the baseline is
   allowed with a CPUID check; a scalar/SSE fallback *below* it is not. Nothing needs a CPUID check to
   use AVX2.
3. **Intrinsics and the t1 vector aliases are in scope from the first line of code**, not gated behind
   a benchmark. What still needs a `bench/` diff (p4/bd1/bd2) is any *claim* that one implementation
   beats another — replacing a straightforward loop with a hand-written kernel, or adding an AVX-512
   path above the baseline.
4. **Scalar code stays legal and expected wherever SIMD is not faster** — p3 says "prefer SIMD
   wherever faster", not "vectorize everything". Most of a DOCX→MD converter is branchy
   pointer-chasing; the genuinely data-parallel candidates are the byte-scanning hot spots: UTF-8
   validation, XML token scanning, escape-class scanning, LZ77 match copies, CRC-32 folding. Scalar
   reference implementations of those kernels double as the p3/a1 oracle for testing them.
5. **A file is the unit of work, and nothing finer** (D6 + D7a). One worker converts one whole
   document at a time; workers come from a **bounded pool** sized by `--threads`, defaulting to the
   system's virtual core count, so with more inputs than workers a thread converts several files in
   sequence. Converting a single document stays strictly sequential — that is the owner's ruling,
   not a measurement, so do not argue it
   either way without a `bench/` diff (bd1/bd2). Concurrency lives one level up, in the driver that
   walks the input list and hands each file to a worker. What follows from that:
   - **each worker owns its whole pipeline**: its own `ZipReader`, `XmlPull`, `StyleModel`,
     `DocWalker`, `MdEmitter`, buffers and output file. No object is ever touched by two workers, so
     these modules take **no locks at all** — they are only ever safe *because* they are unshared,
     which is a weaker promise than thread-safety and must not be mistaken for one. A buffer must
     never travel between workers.
   - **lock only what is genuinely compound.** The work-list cursor and the exit-code accumulator
     are single scalars: an `_Interlocked*` increment or CAS covers them with no lock at all, and
     `memory management.h` already ships `LockedCopy`/`LockedSwap`/`LockedMoveAndClear` for small
     interlocked moves. That leaves the diagnostics/console sink as the one thing needing
     `include/spinlocks.h`, the primitive D6 put in scope. Match the profile to the hold time:
     `SpinLock` is pause + backoff with **no yield path**, so it suits only provably short sections;
     use `SpinLockMin`, which escalates to `Sleep(0)`/`Sleep(1)`, for anything that performs I/O —
     console and stderr writes are syscalls that can block for milliseconds on a redirected pipe,
     and spinning through that burns a core.
   - **allocation is safe from any worker.** The `memory management.h` family declares no global
     state and bottoms out in `_aligned_malloc`, which the MSVC CRT serialises internally, so
     `amalloc`/`mdealloc` need no lock of their own. That holds **only because `DATA_TRACKING` is
     never defined** — its hooks call unsynchronised `MemTrack`/`MemUntrack` on every alloc and
     free, so defining it would introduce exactly the shared mutable state this bullet denies.
   - **`$LoopMT*` and `/Qpar` stay banned.** `$LoopMT` is `__pragma(loop(hint_parallel(0)))`, a
     *hint* to MSVC's auto-parallelizer that does nothing at all without `/Qpar` and, with it,
     hands the compiler discretion over whether and how a loop is split. D6's threading is a
     deliberate, per-file worker the code owns and can join, size and account for — not a compiler
     hint. (The pragma applies to whichever loop follows it, inner or outer, so this is a control
     argument, not a granularity one.)
   - **the single-file case is still effectively single-threaded**, which is the common invocation;
     do not let the worker layer complicate it.
   - **start workers with `_beginthreadex`, not `CreateThread`.** Every worker calls CRT code —
     `_aligned_malloc` through `amalloc`, and file I/O — and a raw `CreateThread` leaks the
     per-thread CRT block. `std::thread` is CRT-correct and is not third-party (D1/D2 bar vendored
     libraries, not the standard library), so it is also fine; pick one at M13 and say which.
   - **read the default worker count with `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)`.** D7's
     default is the system's virtual core count, and the obvious ways to ask — `GetSystemInfo`'s
     `dwNumberOfProcessors` and `std::thread::hardware_concurrency()` — report only the calling
     thread's processor group, capping at 64 on large machines. Clamp the result to at least 1, and
     treat `--threads 0` or a value above the core count as a usage error rather than silently
     coercing it.
   D5's a2 exception is **still live**, not spent: threading only arrives at M13, so every binary
   from M2 through M12 is strictly single-threaded, and even after M13 a single document's
   conversion is deliberately sequential — which is exactly the a2 deviation D5 was granted for.
   Keep writing `// RULE-DEV:a2 single-threaded by owner ruling (D5)` where a reader would expect
   threading inside the per-file pipeline; what D6 removed is the reason to write it on the
   file-list loop, which is now threaded on purpose. p3's "expose thread status via atomics;
   document memory order" is only partly discharged here — see Known gaps.

MSVC macro trap: MSVC defines `__AVX__`/`__AVX2__`/`__AVX512*__` but **never** `__FMA__` or
`__BMI2__`. Guard on `__AVX2__` alone — that is why the `defined(__FMA__) || defined(__AVX2__)` tests
in `SIMD management.h` resolve through their `__AVX2__` arm. PCLMULQDQ (the fast CRC-32 route) is
*not* in a2's named baseline even though every AVX2-class CPU carries it: use it behind an a8 CPUID
check, or raise a decision to widen the baseline — do not just assume it.

### Shared headers — how to use them

They live in `include/` and are owner-authored library files shared with other projects, not
repo-local code. **Do not reformat, refactor, or re-version them**; if one needs a change, raise it
as a numbered decision (D8+) the way D1–D7 were raised. `include/.clang-format` enforces that
mechanically — `DisableFormat: true`, so a stray "Format Document" in the IDE is a no-op there. What
sessions need to know:

- `include\` is on the compiler's include path (`<AdditionalIncludeDirectories>` in every config), so
  write `#include "typedefs.h"`, never `#include "include/typedefs.h"` or a `..\` path. The headers
  include each other by bare name and sit in one directory, so they resolve either way.
- Include `typedefs.h` first; everything else depends on it.
- `common functions.h` calls `Sleep()` in `Idle()` but does **not** include `<windows.h>` — include
  `memory management.h` (which pulls `<windows.h>` before `typedefs.h`) or `<windows.h>` yourself
  first, or that TU will not compile. When a project TU first pulls `<windows.h>` in, consider
  defining `WIN32_LEAN_AND_MEAN` and `NOMINMAX` project-wide.
- **Never define `DATA_TRACKING`.** `memory management.h` has ten `#ifdef DATA_TRACKING` hooks that
  call `MemTrack`/`MemUntrack` from `data tracking.h`, and that header is **not in this repo** — the
  build breaks the moment the macro is defined.
- Allocation goes through them (p2): `amalloc(bytes, alignment)` / `salloc(...)` /
  `mdealloc(ptr)` / `malloc16|32|64(bytes)` / `declare1d16|32|64(...)`, with `mzero`/`mset` for fills
  and `Copy*`/`Stream*` for bulk moves (`Stream*` is non-temporal — bd1/bd2 before claiming it wins).
- `spinlocks.h` guards what the file-level workers share (D6) — diagnostics, console, work-list
  cursor — and nothing inside a single document's pipeline. It pulls `<windows.h>` and `<intrin.h>`
  itself, before `typedefs.h`. `SpinLockMin` when a wait may be long or the section does I/O,
  `SpinLock` for provably short sections, `SpinLockMax` for very short hot ones, `SpinLockTry` to
  avoid blocking. Every entry point takes `vui32ptrc` (`volatile ui32* const`), so **declare the
  flag `volatile ui32`**, initialised to 0 and naturally aligned. Two of the header's own `To Do`
  items bear on us: the `SPIN_*` thresholds are untuned, and there is no cache-line-padded lock type
  yet — so keep separate lock flags in separate cache lines by hand, or they will false-share.

### Do NOT (anti-habit list)

- No tabs; no 2- or 4-space indent (r8).
- No `uint32_t`, `int32_t`, `unsigned`, `float`, `double` in new code — use `ui32`/`si32`/`fl32`/`fl64` (r1).
- No `{` on its own line after a function signature (r15); no missing space before it.
- No `const T*` written at use sites — use the alias forms (r2), and never mix styles in a TU (t3).
- No `History:` field or changelog notes in file prologs (r17/c1).
- No `snake_case` or `camelCase` function names (r11).
- No GPU/Vulkan machinery, no archiving/oracle scaffolding (a1/a5/a7) until there is a second
  implementation of an algorithm to archive.
- No new behavior `#ifdef`s — no `#ifdef _DEBUG` code paths, feature forks, or compile-time
  implementation selection; choose behavior via traits/strategy or separate TUs at link/dispatch time
  (a11, a8). Compile-time *error* guards (D4's `#ifndef __AVX2__` + `#error`) are fine — they fail
  the build, they do not fork it.
- **No third-party code, period** (D1 + D2): no vendored libraries, no `third_party/` directory, no
  package manager. ZIP, inflate and XML are all first-party. Adding a dependency needs a new decision.
- No scalar/SSE fallback paths and no CPUID dispatch below the AVX2 baseline (D4, a8).
- No `$LoopMT*` and no `/Qpar` (**D6** — auto-parallelizer hints hand the compiler control over
  threading that D6 gives to an explicit per-file worker).
- No threading *inside* one document's conversion (D6). Concurrency belongs to the file-list driver
  and nowhere else, bounded by the `--threads` pool (D7a) — never a thread spawned per file with no
  ceiling, and never a second pool layered under the first.
- No performance *claim* without a `bench/` diff (bd1/bd2) — using intrinsics needs no permission,
  asserting they are faster does.
- No hand-rolled allocators or bare `new`/`malloc` — `memory management.h` owns that (p2).
- No edits to `GDC_GCS_v1_1_4.md`, `CONTRIBUTING.MD`, or the six shared headers without owner sign-off.

### r17 file prolog — copy this template

```cpp
/*
 * File: <ExactFilename.ext>
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: <YYYY-MM-DD, date the file is created>
 * Last Modified: <YYYY-MM-DD, update on every edit — the only field tools may auto-update>
 * Description: <one concise line only; details go in docs/>
 * To Do: 1) <highest-impact task>
 *        2) <next task>
 * Dependencies: None
 * ISA: Scalar
 * Thread-safety: <N/A | Reentrant | MT-safe>
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
```

Prolog validation traps (regex-checked per r17): content lines start with exactly `" * "`; a blank
separator line is exactly `" *"`; ASCII only; wrap at 150 (`" // WIDTH-EXEMPT"` allows ≤180 for
unbreakable tokens); `License:` has **exactly two spaces** between the SPDX id and `Copyright:`;
`To Do:` items are numbered `1)`, `2)`, … with continuations aligned under the value; ISA tokens come
from {Scalar, SSE4.2, AVX2, **AVX-512**} separated by `" | "` (use the validation form, e.g.
`ISA: Scalar | AVX2`, not the template's bracket form, and not `AVX512` — `typedefs.h` gets that
token wrong, do not copy it); Thread-safety ∈ {N/A, Reentrant, MT-safe} — the template shows all
three because after D6 it is a real choice, but a real file names exactly one (see the D6 mapping
below).

`ISA:` describes the code in the file, not the compiler flag: a file whose only vectorization comes
from `/arch:AVX2` auto-vectorizing scalar loops stays `ISA: Scalar`; write `ISA: Scalar | AVX2` once
the file actually carries intrinsics or vector aliases. `Thread-safety:` follows D6's boundary, but
r17 only offers three tokens, so map onto them the way the owner's own headers do: anything the
workers share — diagnostics sink, console writer, work-list cursor — is `MT-safe` (and must say in
the prolog what its locking contract is); everything else in the pipeline is `Reentrant`, which is
how `common functions.h` and `memory management.h` already tag stateless-or-per-instance code;
`N/A` is reserved for files with no executable code at all, as `typedefs.h` uses it. Strictly, a
per-worker `ZipReader` or `MdEmitter` is *thread-compatible* rather than reentrant — r17 has no
token for that, so `Reentrant` is the nearest legal one (see Known gaps). `MT-safe` is no longer
forbidden; before D6 it was.

### Known gaps in the GCS you must not paper over

- a3's bare `static_assert(__AVX2__)` needs C++17's single-argument form **and** `/arch:AVX2` (else
  the macro is undefined and the assert reads as `static_assert(0)` — or fails to compile); the
  two-argument form needs only C++11. C++20 is already set on the x64 configs, so only the flag is
  missing; D4 settles the form as `#ifndef __AVX2__` + `#error` for a readable message. **M1 has
  applied both** — the flag on both x64 configs, the guard in `DOCXtoMD.cpp` — though no build has
  yet confirmed it. `include/spinlocks.h` carries a guard of that shape — copy the **structure, not
  the text**: its `#error` message names `spinlocks.h`, and the `static_assert(__AVX2__, …)`
  underneath it is exactly the construct D4 ruled out.
- p3 literally says "keep scalar baseline; … run-time CPUID dispatch", which reads as a scalar
  fallback path; a2/a8 plus D5 override that for this project — scalar survives as an *oracle* and as
  the right choice where SIMD is not faster, never as a shipped fallback build.
- tc2 mandates **CRLF source files**, and until M1 nothing enforced it. `.gitattributes` now does
  (`text eol=crlf` on every source and build pattern), so a Linux session cannot drift a source file
  to LF: whatever it writes, the checkout is CRLF. What is still unenforced is tc2's *other* half —
  no tool checks `indent_size = 3` or `max_line_length = 180`; `.editorconfig` only asks editors
  nicely, and there is no CI (M12) or pre-commit hook to fail a violation.
- Two shared headers (`SIMD management.h`, `vector structures.h`) still carry the pre-r17 boxed
  banner, `typedefs.h` writes the nonconforming ISA token `AVX512` and un-numbered `To Do:` items,
  `spinlocks.h` declares `ISA: AVX2` although it carries no AVX2 code (its only intrinsics are
  `_mm_pause`, `__rdtsc` and the `_Interlocked*` family — by the rule above that reads as
  `ISA: Scalar`; the token appears to describe its `/arch:AVX2` build guard instead), and
  the allocator family is lowercase (`amalloc`, `salloc`, `mzero`) against r11's PascalCase. These are
  owner-authored files: **report them, do not fix them here.**
- `memory management.h` documents a dependency on `data tracking.h`, which is absent from this repo
  (see "Shared headers").
- r17's `Thread-safety` vocabulary has no token for **thread-compatible** ("safe as long as two
  threads do not share the instance"), which after D6 is the accurate description of every per-worker
  module. `Reentrant` is used for it because it is the nearest legal token and matches the owner's
  headers — not because these types are reentrant in the strict sense. Do not read `Reentrant` in a
  project prolog as a promise about recursion or signal handlers.
- p3's [MUST] to "expose thread status via atomics; document memory order" is only half discharged
  by D6. `spinlocks.h` is the sanctioned primitive and it uses `_Interlocked*` intrinsics over
  `volatile ui32` with unconditional full barriers — there is no `std::atomic` and no memory-order
  argument to document. Record the locking contract in the prolog instead; adopting `std::atomic`
  anywhere would need a new decision.

## Conversion engine — non-negotiable correctness rules

Full detail with rationale lives in `docs/CONVERSION_REFERENCE.md`; these are the invariants every
implementation session must respect:

1. **Resolve, never hardcode**: the main document part comes from `_rels/.rels` (cross-checked via
   `[Content_Types].xml`); hyperlink/image/footnote targets resolve through relationship IDs, which
   are **scoped per part** (`rId3` in document.xml ≠ `rId3` in footnotes.xml).
2. **Match XML by namespace URI + local name** (accept both Transitional and Strict URI families),
   never by the literal `w:` prefix. Handle `mc:AlternateContent` (take the first understood
   `mc:Choice`, else `mc:Fallback`) or images will double- or zero-emit.
3. **Effective formatting = layered resolution**: docDefaults → paragraph-style `basedOn` chain →
   character style → direct `rPr`, with **XOR toggle semantics** for `w:b`/`w:i`/`w:strike`/`w:caps`/
   `w:smallCaps`/`w:vanish` (direct formatting wins outright). Cycle-guard `basedOn` chains.
4. **Coalesce adjacent runs with identical effective formatting before emitting any delimiter** —
   Word fragments runs mid-word (rsid/spellcheck); naive per-run emission produces `**Hel****lo**`.
5. **Hoist leading/trailing whitespace (including U+00A0) out of emphasis spans** — `**bold **text`
   does not parse. Never emit delimiters around empty or whitespace-only spans.
6. **All output text flows through one context-aware escaping writer** (contexts: inline, lineStart,
   tableCell, linkText, linkDest, altText, codeSpan, codeBlock, html). Walker code never concatenates
   raw strings into output. Nothing is backslash-escaped inside code spans/fences — handle backtick
   collisions by lengthening the delimiter run.
7. **Fields run through a begin/separate/end state machine with a nesting stack** (fields nest and
   span paragraphs): `HYPERLINK` → link, `TOC` → skip entire field, others → cached result text.
8. **Tracked changes: accept-all** — keep `w:ins`, drop `w:del` (+ `w:delText`); recurse transparently
   into `w:sdt`/`w:sdtContent`, `w:smartTag`, `w:customXml`.
9. **Lists**: counter state per `(abstractNumId, ilvl)`; `startOverride` restarts; `numId` 0 = no
   numbering; numbering can come from the style chain; dangling `numId` refs degrade gracefully.
10. **Hostile input is expected** (M11 hardens, but design for it from M3): cap total/per-entry
    decompressed bytes, compression ratio, and entry count; **never reuse archive entry names as disk
    paths** (generate `image1.png`… yourself); reject `<!DOCTYPE` (XXE); detect OLE magic
    `D0 CF 11 E0` and report "encrypted or legacy .doc" instead of crashing.
11. **Verify what you inflate** (new with D1's first-party decoder): enforce the byte and ratio caps
    *while* inflating, never from the central directory's declared sizes, and check each entry's
    CRC-32 against its header. **ZIP's CRC-32 is IEEE 802.3, reflected polynomial `0xEDB88320`** — the
    SSE4.2 `_mm_crc32_u*` intrinsics implement **CRC-32C (Castagnoli)**, a different polynomial, and
    silently validate nothing. Use a table-driven scalar CRC, or PCLMULQDQ folding under the a8 caveat
    above once a `bench/` diff justifies it.

### Default mapping policies (decided — do not re-litigate per session)

| DOCX | Markdown output |
|---|---|
| Heading styles / `outlineLvl` 0–8 | `#`–`######` (clamp 7–9 to `######`); heading text never additionally bolded |
| Bold / italic / strike | `**` / `*` (never `_`) / `~~` |
| Superscript / subscript | `<sup>` / `<sub>` |
| Underline, highlight, color, size | **Dropped** (no Markdown equivalent; hyperlink styling suppressed) |
| Inline code | `` ` `` — via code-named character styles or monospace `rFonts` |
| Code block | Fenced ``` — consecutive all-monospace paragraphs merge into one fence |
| Quote styles | `> ` blockquote |
| Bullet / numbered lists | `-` / real computed numbers (`3.` honors start); nested by `ilvl` |
| Tables | GFM pipe tables; header = first row (or `tblHeader`); cell breaks → `<br>`; merged → padded GFM cells (gridSpan: content in first cell + empty pads; vMerge continue: empty cell; HTML `<table>` under `--tables=html-on-merge`); nested → HTML `<table>` fallback |
| Hyperlinks | `[text](url)` external, `[text](#anchor)` internal (GFM heading slugs) |
| Images | `![alt](media dir/imageN.ext)` — extracted, extension from content type, alt from `docPr/@descr` |
| Footnotes/endnotes | `[^n]` refs + definitions at end, renumbered 1..n |
| Horizontal rule (`pBdr` bottom on empty ¶) | `---` with blank lines around |
| `w:br` (textWrapping) / page break | Backslash hard break (`<br>` in cells) / nothing |
| TOC (field or SDT), headers/footers, comments | Skipped |
| Soft hyphens | Removed; NBSP and smart punctuation kept verbatim |
| Output encoding | UTF-8, no BOM, LF line endings (tc2's CRLF governs source files, not program output) |

## Planned architecture (only `docs/` and `include/` exist so far — build the rest via the Roadmap)

```
src/
   main.cpp              wmain + SetConsoleOutputCP(CP_UTF8) + wiring only; wide APIs for all paths
   Batch.h/.cpp          input list → bounded worker pool, one file per worker at a time (D6/D7a);
                         interlocked work cursor and exit-code fold; failed-input list for the
                         end-of-run report; the only module in the tree that starts a thread
   BuildGuards.h         #ifndef __AVX2__ + #error (D4); included first by every project TU
   CliOptions.h/.cpp     argv → options struct; usage/version text
   Utf.h/.cpp            UTF-8 validate/transcode (UTF-16 only at the Win32 boundary)
   Inflate.h/.cpp        first-party RFC 1951 DEFLATE (D1): stored/fixed/dynamic Huffman, 32 KiB window
   Crc32.h/.cpp          ZIP CRC-32 (poly 0xEDB88320 — NOT SSE4.2 CRC-32C); entry verification
   ZipReader.h/.cpp      EOCD/central directory/local headers, methods 0+8, ZIP64; bomb+traversal caps
   XmlPull.h/.cpp        streaming pull tokenizer over the inflated buffer; zero-allocation, string_view tokens
   OpcPackage.h/.cpp     [Content_Types].xml + rels graphs; part lookup; r:id resolution
   StyleModel.h/.cpp     styles.xml → resolved-props cache (basedOn chains, toggle XOR, name normalization)
   NumberingModel.h/.cpp numbering.xml → per-numId levels with overrides; runtime counters
   Ir.h                  intermediate representation (blocks/spans) — the walker never emits Markdown
   DocWalker.h/.cpp      document/footnote walk → IR (fields, tracked changes, sdt, AlternateContent)
   RunCoalescer.h/.cpp   effective-format resolution + adjacent-run merging + whitespace hoisting
   MdEscape.h/.cpp       the context-aware escaping writer (pure, unit-testable)
   MdEmitter.h/.cpp      IR → Markdown text; blank-line discipline; delimiter sizing
   MediaExtractor.h/.cpp referenced media parts → disk; content-type extensions; dedup; safe names
   Diag.h/.cpp           error codes/messages → stderr; exit-code mapping. MT-safe: every worker
                         reports through this one sink, so it locks (D6)
tests/                   fixtures/<case>/src/ (unzipped part trees) + expected.md; make_fixtures.py;
                         run_golden.py; unit tests as a second console .vcxproj with a tiny CHECK header
bench/                   GCS p4 microbenches (create with the first performance claim)
docs/                    CONVERSION_REFERENCE.md (already here); module guides (d2/d3) still to come
include/                 the six owner-authored shared headers (already here); on the include path
                         via $(ProjectDir)include, so TUs include them by bare name
```

There is **no `third_party/`** and there will not be one (D1/D2): the shipped binary is first-party
code plus the CRT/Win32 and the six shared headers in `include/`, which every module may include.

Allocation-conscious modules (GCS p2 hot set): `Inflate`, `ZipReader`, `XmlPull` (zero-allocation
steady state), `DocWalker`, `RunCoalescer`, `MdEmitter` (single growable buffer), `Utf` — all
allocating through `memory management.h`. The parsed-once models (`StyleModel`, `NumberingModel`,
`OpcPackage`, `CliOptions`) use the same allocators but are not hot.

Under D6, **`Batch` and `Diag` are the only `MT-safe` modules**. Everything that converts a document
— `Utf`, `Inflate`, `Crc32`, `ZipReader`, `XmlPull`, `OpcPackage`, `StyleModel`, `NumberingModel`,
`Ir`, `DocWalker`, `RunCoalescer`, `MdEscape`, `MdEmitter`, `MediaExtractor` — is instantiated once
per worker, holds no cross-file state and is never shared, so it needs no lock. `CliOptions` holds
the input **list** (D7b) plus the worker count, is parsed once before any worker starts and is then
read-only, so workers may share it by const reference. Design each module that way from its first commit: retrofitting a shared cache into a
per-worker pipeline later is exactly the rework D6 exists to avoid.

One hazard this does **not** cover: `MediaExtractor` and the output writer share the *filesystem*,
not memory. D7b derives output names from input stems, so `a\report.docx` and `b\report.docx` in one
run both target `report.md` and `report_media\`, and an explicit `--media-dir` shared by several
inputs collides the same way. No amount of per-worker isolation fixes that. Recommended (derived,
not ruled): `Batch` detects duplicate output targets up front, before any worker starts, and fails
those inputs into D7c's failure list rather than letting two workers race — a pre-flight check is
cheap and the alternative is silent data loss.

### Target CLI (implemented incrementally from M2)

```
Usage: DOCXtoMD [options] <input.docx> [input2.docx [input3.docx [...]]]
  -o, --output <path>    output path: the .md filename for a single input,
                         the destination directory when several are given
  -j, --threads <n>      worker threads (default: system virtual core count)
  --media-dir <dir>      image dir (default <stem>_media\)   --no-images   alt text only
  --hard-break=<backslash|spaces>  (default backslash)      -q, --quiet   errors only
  --stdout               markdown to stdout — single input only
  --version              print version, exit 0              -h, --help    usage, exit 0
```

Note what D7 removed: there is **no positional output operand** any more. `<input.docx> [output.md]`
could not coexist with repeated inputs — a second path would be ambiguous — so every operand is an
input and output goes through `-o`. Output filenames are otherwise derived from each input stem.

Exit codes (stable API): 0 all inputs converted · 1 usage error · 2 input not found/readable ·
3 not a valid DOCX · 4 output write failure · 5 internal error · **6 partial success** (at least one
input converted and at least one failed). Per D7c, the failures are listed on the console before the
process exits, so code 6 is a summary and never the only diagnosis. Codes 2–5 stay per-file
verdicts: with a single input they are the exit code directly, and when **every** input fails they
are what the process returns (the highest code among them if they differ) — that last rule is
derived, not ruled, since D7c only names the partial case.

`-j`/`--threads` is the spelling this file assumes for D7a's user-specified thread count; the owner
ruled the behaviour, not the flag name.

D7 settles this surface, so it is no longer provisional. Two consequences reach back into M2, where
`CliOptions` is first written: **hold the inputs as a list from the start** — retrofitting one later
means re-cutting argument parsing, `-o` semantics and the output-naming rule together — and **give
`-o` its two meanings from the start** (a filename when exactly one input is given, a directory
otherwise), because that branch is the whole reason the positional output operand had to go. M2 may
still accept only one input; what it must not do is assume there will only ever be one.

## Testing & definition of done

- Fixtures are **unzipped part trees** under `tests/fixtures/<case>/src/` (hand-authorable, reviewable,
  diffable) zipped into `.docx` by `tests/make_fixtures.py` (Python `zipfile`; never PowerShell
  `Compress-Archive` — it writes backslash separators). `tests/run_golden.py` runs the exe and
  byte-compares against `expected.md`, and asserts documented exit codes on corrupt-input fixtures.
  `make_fixtures.py` must be able to emit **stored** and **deflated** entries so the first-party
  inflater is exercised on both.
- A milestone's DoD is **commands that pass**, not adjectives. Before claiming any change done:
  1. x64 Release builds with **zero warnings** at `/W3` (on Windows; on Linux say you could not build).
  2. New/changed files: prolog validates (r17 regexes), 3-space indent, no tabs, lines ≤150/180,
     CRLF, `Last Modified` bumped.
  3. `.vcxproj` + `.filters` updated together for any added file.
  4. Golden/unit tests pass once they exist; new conversion features land **with** a fixture pair.
  5. `CHANGELOG.md` updated — it exists from M1 on, so this is unconditional now.
- Never leave the repo in a non-building state at the end of a turn; every commit builds.

## Roadmap

Work the **first non-`[done]` milestone** unless the user directs otherwise. On completion — in the
same commit — flip its marker here, update the "Current state" section to match the repo, and update
the prolog `To Do:` lists. A Linux session that finishes a milestone's work but cannot run its DoD
commands flips the marker to `[done-unverified]` and lists the unrun commands in the commit message;
only a run whose DoD commands pass on Windows flips it to `[done]`, and the next Windows session
verifies (not reimplements) `[done-unverified]` milestones before starting new work.

- **M1 `[done-unverified]` Compliance bootstrap** — also carries the executable half of D4
  (D3's landed early):
  - add `CHANGELOG.md` (c2/c3), `.clang-format` per tc1 (IndentWidth 3, UseTab Never, ColumnLimit 180,
    BreakBeforeBraces Attach, AllowShortFunctionsOnASingleLine All, align decls/assigns/comments),
    `.editorconfig` per tc2 (UTF-8, CRLF, indent 3, max_line_length 180), `.gitattributes` (CRLF for
    source; leave the Markdown docs as they are);
  - (**D3** is **done and owner-verified** — the `Win32` `ProjectConfiguration`s and every `…|Win32`
    `PropertyGroup` / `ImportGroup` / `ItemDefinitionGroup` were deleted from `DOCXtoMD.vcxproj` on
    the owner's instruction, ahead of M1. The `.sln` already listed x64 only, so it needed no change,
    and `<LanguageStandard>stdcpp20</LanguageStandard>` was already set on both surviving configs.
    M1's `/p:Platform=Win32` DoD check is therefore already discharged);
  - **D4**: add `<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>`
    to both x64 `ItemDefinitionGroup`s, and put the `#ifndef __AVX2__` + `#error` guard in
    `DOCXtoMD.cpp` (temporary — it moves to `src/BuildGuards.h` at M2);
  - (the `<ClInclude>` ItemGroup in `DOCXtoMD.vcxproj.filters` is **already done** — all six headers
    sit under Header Files. This M1 sub-task landed early in the `include/` resync commit, because
    listing `include\spinlocks.h` obliged the MSBuild file-list rule to fix `.filters` anyway);
  - add the r17 prolog to `DOCXtoMD.cpp` (temporary — superseded by `src/main.cpp` at M2).

  DoD: x64 Debug **and** Release build clean at `/W3`; the prolog passes the r17 regexes; a
  `/p:Platform=Win32` invocation fails instead of building; temporarily clearing
  `EnableEnhancedInstructionSet` makes the build stop on the `#error`.
  **Status**: the work landed from Linux on 2026-08-19, so only the r17 regexes were actually run
  (they pass, and so does the `/p:Platform=Win32` check by way of D3's owner verification). The three
  msbuild checks — Debug clean, Release clean, and the `#error` firing when
  `EnableEnhancedInstructionSet` is cleared — are **unrun**. A Windows session runs those three, and
  only then flips this marker to `[done]`; it should also confirm `si32 main()` and
  `#include "typedefs.h"` compile clean at `/W3`, since neither was ever put through a compiler.
- **M2 `[todo]` CLI skeleton** — `wmain`, `src/` layout starts (`main.cpp`, `BuildGuards.h`,
  `CliOptions`, `Diag`), usage/help/version, exit codes 0/1/2. Retire `DOCXtoMD.cpp` in the same
  commit: delete it, carry its prolog and the `__AVX2__` guard forward into `src/main.cpp` /
  `src/BuildGuards.h` (updating `File:`/`Description:`), and swap the `.vcxproj` + `.filters` entries
  per the MSBuild file-list rule. Shape `CliOptions` for D7 now even while only one input is
  accepted: inputs are a **list**, `-o` means filename-or-directory by input count, and there is no
  positional output operand. DoD: no-args prints usage and exits 1; `--version` exits 0; the usage
  text matches the Target CLI block above.
- **M3 `[todo]` ZIP container + inflate** *(D1 settled: first-party)* — `Inflate` (RFC 1951: stored,
  fixed-Huffman and dynamic-Huffman blocks; canonical decode tables; 32 KiB window; overlapping match
  copies), `Crc32`, and `ZipReader` (EOCD search over the last 65,557 bytes, central directory, local
  headers, methods 0/8 only, ZIP64, data descriptors, duplicate names, encryption bit) with the
  decompression caps enforced *during* inflation. Plus the first test scaffolding:
  `tests/make_fixtures.py` and initial fixture part-trees (minimal valid doc + corrupt/encrypted/`.doc`
  negatives). DoD: extracts `word/document.xml` from both a stored-entry and a deflated-entry fixture
  `.docx` with CRC-32 verified; a dynamic-Huffman payload round-trips against a Python-`zlib`-generated
  fixture; corrupt/encrypted/`.doc` inputs exit 3 with clear messages.
- **M4 `[todo]` XML + package model** *(D2 settled: first-party `XmlPull`)* — `XmlPull`, `OpcPackage`,
  plus the unit-test harness (second console `.vcxproj` + tiny CHECK header under `tests/`). DoD: unit
  tests drive token streams from string literals; main part resolved via rels, not hardcoded.
- **M5 `[todo]` Paragraphs & headings** — `StyleModel` (chains + toggle XOR), minimal `DocWalker`/
  `Ir`/`MdEmitter`, plus `tests/run_golden.py` (exe runner + byte-compare + exit-code assertions).
  DoD: first golden fixture converts byte-exact.
- **M6 `[todo]` Inline formatting** — `RunCoalescer` (merge + hoist), `MdEscape` contexts,
  bold/italic/strike/code/sup/sub. DoD: fragmented-run and trailing-space-in-bold fixtures pass.
- **M7 `[todo]` Hyperlinks & images** — rels resolution, `MediaExtractor`, anchors/slugs.
- **M8 `[todo]` Lists** — `NumberingModel` (indirection, overrides, restarts, style-borne numPr).
- **M9 `[todo]` Tables** — grid normalization, gridSpan/vMerge policy, HTML fallback.
- **M10 `[todo]` Fields, notes, tracked changes** — field state machine, footnotes/endnotes, sdt,
  accept-all revisions.
- **M11 `[todo]` Hostile-input hardening** — bombs, traversal, XXE, producer-variance fixtures
  (Google Docs / LibreOffice / Pandoc exports).
- **M12 `[todo]` CI** — GitHub Actions `windows-latest`: msbuild x64 Release (the only platform) +
  fixture build + golden runner.
- **M13 `[todo]` Multi-file batch + bounded worker pool** *(D6 and D7 both ruled — specifiable)*
  — `Batch` over a list of inputs, threading per D6/D7a, `Diag` made
  `MT-safe` with `include/spinlocks.h`, `--threads` parsing with the virtual-core-count default,
  per-file failures listed on the console, exit code 6 for partial success. Land it **after** the
  converter is correct: every module below `Batch` must already be per-worker, which M2–M11 deliver
  by construction. DoD: `run_golden.py` converts a fixture set as one batch and file-by-file and
  byte-compares the two output trees; the same batch at `--threads 1` and `--threads 8` produces
  identical bytes and the same exit code; a fixture set mixing valid and corrupt inputs exits 6,
  converts every valid input, and names every failed one on the console; `--stdout` with two inputs
  exits 1. Note MSVC v143 ships no thread sanitizer (`/fsanitize=address` only), so "no data races"
  cannot be a DoD command — the determinism comparisons are what is actually checkable.

## Decisions (ruled rows are settled — do not re-litigate; open rows await the owner)

D1–D5 were ruled by the owner on 2026-08-18, D6 and D7 on 2026-08-19. Keep the IDs stable:
`docs/CONVERSION_REFERENCE.md` §6.2 cites "D2 in CLAUDE.md" by name. New questions get the next free
ID (D8, D9, …) with the same question/recommendation/status shape, and stay `Open — owner call`
until the owner rules.

| ID | Question | **Ruling** | Executed? |
|---|---|---|---|
| D1 | ZIP/DEFLATE: vendor miniz vs hand-rolled inflate vs zlib | **Hand-rolled inflate.** First-party `Inflate` + `Crc32` + `ZipReader`; no `third_party/`, no vendored code | M3 |
| D2 | XML: hand-rolled pull parser vs pugixml | **Hand-rolled pull parser.** First-party `XmlPull`; pugixml is off the table | M4 |
| D3 | Win32 configs vs GCS a2 ("32-bit unsupported") | **Drop the Win32 configurations** from `DOCXtoMD.vcxproj`; x64 is the only platform | **done** (owner-verified on Windows 2026-08-19: `/p:Platform=Win32` fails instead of building) |
| D4 | Adopt a3: `/arch:AVX2` + `__AVX2__` guard on x64 | **Adopt**, with the guard as `#ifndef __AVX2__` + `#error` (not `static_assert`) | **done-unverified** (M1: flag on both x64 configs, guard in `DOCXtoMD.cpp`; never built) |
| D5 | Does a2's tech cut-off (no 32-bit, no SSE-only, no single-threaded) bind this tool? | **Baseline is SIMD, single-threaded**: AVX2 floor with no sub-baseline fallback; single-threading is an owner-granted exception to a2 *(as ruled 2026-08-18; D6 later narrowed the threading half — the text here is left as the owner wrote it)* | standing |
| D6 | `include/spinlocks.h` was added "for future multithread code" — does it reopen D5 for DOCXtoMD? | **Yes, for multi-file processing only: one thread per file, `spinlocks.h` included.** *(Derived, not stated: a single document's conversion therefore stays sequential, and `$LoopMT*`//Qpar stay banned as compiler-directed threading — see the threading baseline.)* | M13 |
| D7 | What batch surface does D6 need? (a) literal thread-per-file or a bounded pool? (b) how are multiple inputs passed? (c) how do per-file failures aggregate? (d) what do `--stdout` and `-o` mean for N files? | **(a) A bounded pool** sized to a user-specified thread count, defaulting to the system's virtual (logical) core count. **(b)** Inputs are repeated command-line operands: `DOCXtoMD [options] <input.docx> [input2.docx […]]`; output filenames are derived automatically. **(c)** Failed conversions are printed to the console before the process terminates, and partial success gets its own exit code. **(d)** `--stdout` is single-file only; `-o` gives the output path — the filename for one input, the directory for many | M13 |

Consequences already folded into this file: the "no third-party code" line in Do NOT and the removal
of `third_party/` from the architecture (D1/D2); the first-party `Inflate`/`Crc32` modules and the
CRC-32-vs-CRC-32C trap in rule 11 (D1); the x64-only Build & run section and the two-configuration
`.vcxproj` (D3); the "ISA and threading baseline" subsection, which is where D4, D5 and D6 actually
live. **What the owner ruled** is D6 (threading is for multi-file processing, one thread per file,
`spinlocks.h` in scope) and D7 (a bounded pool sized by a `--threads` count defaulting to the virtual
core count; repeated input operands with derived output names; failures printed and a distinct
partial-success exit code; `--stdout` single-input only, `-o` a filename for one input and a
directory for many). Everything downstream of those — the `Thread-safety:` mapping under r17, the
`Batch` and `Diag` entries in the architecture, the per-worker classification, the `-j` flag
spelling, exit code 6's number, the all-inputs-failed rule, the duplicate-output pre-flight check
and milestone M13 — is **derived by a session, not
stated by the owner**, and may be revised without re-litigating D6.

## Repo conventions

- Commit messages: imperative summary line; never name AI models in commit messages, code comments,
  or PR text (standard Claude Code attribution trailers are fine).
- `CHANGELOG.md` (from M1): Keep-a-Changelog style per c3; prologs stay history-free (c1).
- License field in every prolog: `License: MIT  Copyright: David William Bull` (two spaces).
- `CONTRIBUTING.MD` and `GDC_GCS_v1_1_4.md` are owner-managed — do not edit them. The six shared
  headers in `include/` are owner-authored library files — do not reformat or re-version them. Raise
  conflicts as numbered decisions instead (like D1–D6 above).
