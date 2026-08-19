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
- `CONVERSION_REFERENCE.md` (repo root — there is no `docs/` directory yet) — the full DOCX→Markdown
  domain specification (OPC container, WordprocessingML element inventory, feature→GFM mapping table,
  escaping rules, edge cases ranked by real-world frequency, pipeline design). **Read the relevant
  sections before implementing any conversion milestone.**

Five owner-authored shared headers sit at the repo root and supply the GCS substrate (aliases,
allocators, SIMD helpers) that new code is expected to build on — see "Shared headers" below.

## Current state (do not assume more exists)

- `DOCXtoMD.cpp` — stub: the Visual Studio default `// DOCXtoMD.cpp : This file contains...` comment,
  `#include <iostream>`, empty `main()`. **No r17 prolog**, and it includes none of the shared headers.
- `DOCXtoMD.sln` — **exists** (VS 17.14, UTF-8 BOM, CRLF) and exposes **only** `Debug|x64` and
  `Release|x64`. The Win32 configurations are already unreachable through the solution.
- `DOCXtoMD.vcxproj` — v143, Unicode, Console, `/W3`, SDLCheck, ConformanceMode, Release
  WholeProgramOptimization. Still declares **four** `ProjectConfiguration`s (Debug/Release ×
  Win32/x64) — D3 says delete the Win32 pair; M1 executes it. The **x64** configs set
  `<LanguageStandard>stdcpp20</LanguageStandard>` + `<LanguageStandard_C>stdc17</LanguageStandard_C>`;
  the Win32 configs set neither (MSVC default C++14). **No `<EnableEnhancedInstructionSet>` anywhere**
  — `/arch:AVX2` is decided (D4) but not yet applied. No OutDir override.
- `DOCXtoMD.vcxproj.filters` — lists only `DOCXtoMD.cpp` under Source Files. It has **no
  `<ClInclude>` ItemGroup**, so the five headers below are in the build but unfiltered in the IDE
  tree; M1 adds the ItemGroup.
- Shared headers at repo root — all five listed as `<ClInclude>` in the `.vcxproj`, all CRLF, all
  tab-free, none exceeding 150 columns:
  - `typedefs.h` v1.0.1 — r1/r2/t1/t2 aliases, the full pointer lattice, `al1`–`al64`, `$LoopMT*`,
    `defpa`/`refpa` (m1/m2). r17 prolog, but `ISA: Scalar | SSE4.2 | AVX2 | AVX512` — `AVX512` is
    not a valid r17 token (see Known gaps).
  - `memory management.h` v1.2 — the p2 allocator family: `amalloc`/`salloc`/`mdealloc`,
    `malloc1..64`, `declare1d16/32/64`, `zalloc*`, `mzero`/`mset`, `Copy*`/`Stream*`,
    `LockedCopy`/`LockedSwap`. r17 prolog; `ISA: Scalar | SSE4.2 | AVX2 | AVX-512`.
  - `common functions.h` v1.1 — constants, `Min`/`Max`, `AllTrue`/`AllFalse`,
    `RoundUp/DownToNearest4..64`, sincos, `Idle`. r17 prolog; `ISA: Scalar | SSE4.2 | AVX2`.
  - `SIMD management.h` — `namespace simd` FMA wrappers (`fmadd_ps`/`fmsub_ps`/`fnmadd_ps`, 128- and
    256-bit). **Pre-r17 boxed banner**, no `ISA:` field.
  - `vector structures.h` — `VEC*`/`SSE*`/`AVX*` unions and vector structs. **Pre-r17 boxed banner**,
    no `ISA:` field.
- `GDC_GCS_v1_1_4.md`, `CONTRIBUTING.MD`, `CONVERSION_REFERENCE.md`, `LICENSE`
  (MIT, Copyright (c) 2026 David William Bull), this file.
- Line endings: every source/build file (`*.h`, `.cpp`, `.vcxproj`, `.filters`, `.sln`) is already
  **CRLF** as tc2 requires; the Markdown docs (`CLAUDE.md`, `CONVERSION_REFERENCE.md`,
  `CONTRIBUTING.MD`) are LF and `GDC_GCS_v1_1_4.md` is CRLF. `.gitattributes` still does not exist to
  hold that line, so a Linux session can still drift it — check before committing (M1 fixes this).
- **Not yet created** (GCS obligations, see Roadmap): `CHANGELOG.md`, `.clang-format`, `.editorconfig`,
  `.gitattributes`, `src/`, `tests/`, `bench/`, `docs/`, CI. Do not reference them as if they exist.

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
**x64 is the only supported platform** — GCS a2 declares 32-bit unsupported, and D3 (settled) drops
the Win32 configurations from the project file at M1. Do not add them back, and do not add a new
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
commit** that adds the file. The `.filters` file has no `<ClInclude>` ItemGroup at all yet, so the
five existing headers are unlisted there — M1 adds the ItemGroup and all five entries.

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
| p3/a2/a3/a8 | AVX2 is the ISA floor and the build is single-threaded — see the next subsection (D4 + D5). |
| p4/bd1/bd2 | Performance-over-idiom, but every performance **claim** needs a benchmark diff in `bench/`; acceptance = ≥3% win or parity with meaningful simplification. |

**GCS sections that do NOT apply here:** g1–g10 (GPU/shader — this tool has no GPU code; the GPU gates
named in en1/en2 are no-ops). The graphics halves of a2/a6.

### ISA and threading baseline (D4 + D5) — the AVX2 floor

The owner's ruling on D5 is: **for this project the baseline is SIMD, single-threaded.** With D4
(adopt a3) that settles into five operative rules:

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
5. **Single-threaded is an owner-granted exception to a2**, whose [MUST] sentence otherwise calls
   single-threaded variants unsupported. It is a standing, project-wide deviation recorded here — do
   not re-litigate it per session and do not tag every file; write
   `// RULE-DEV:a2 single-threaded by owner ruling (D5)` only where a reader would otherwise expect
   threading (e.g. a loop over multiple input files). No threads, no thread pool, **no `$LoopMT*`
   macros from `typedefs.h`**, and do not enable `/Qpar`. p3's "expose thread status via atomics;
   document memory order" is vacuous here: prologs say `Thread-safety: N/A` or `Reentrant`, never
   `MT-safe`.

MSVC macro trap: MSVC defines `__AVX__`/`__AVX2__`/`__AVX512*__` but **never** `__FMA__` or
`__BMI2__`. Guard on `__AVX2__` alone — that is why the `defined(__FMA__) || defined(__AVX2__)` tests
in `SIMD management.h` resolve through their `__AVX2__` arm. PCLMULQDQ (the fast CRC-32 route) is
*not* in a2's named baseline even though every AVX2-class CPU carries it: use it behind an a8 CPUID
check, or raise a decision to widen the baseline — do not just assume it.

### Shared headers — how to use them

They are owner-authored library files shared with other projects, not repo-local code. **Do not
reformat, refactor, or re-version them**; if one needs a change, raise it as a numbered decision
(D6+) the way D1–D5 were raised. What sessions need to know:

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
- No scalar/SSE fallback paths, no CPUID dispatch below the AVX2 baseline, no `$LoopMT*`, no `/Qpar`
  (D4/D5, a8).
- No performance *claim* without a `bench/` diff (bd1/bd2) — using intrinsics needs no permission,
  asserting they are faster does.
- No hand-rolled allocators or bare `new`/`malloc` — `memory management.h` owns that (p2).
- No edits to `GDC_GCS_v1_1_4.md`, `CONTRIBUTING.MD`, or the five shared headers without owner sign-off.

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
 * Thread-safety: N/A
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
token wrong, do not copy it); Thread-safety ∈ {N/A, Reentrant, MT-safe}.

`ISA:` describes the code in the file, not the compiler flag: a file whose only vectorization comes
from `/arch:AVX2` auto-vectorizing scalar loops stays `ISA: Scalar`; write `ISA: Scalar | AVX2` once
the file actually carries intrinsics or vector aliases. `Thread-safety:` is `N/A` or `Reentrant` —
never `MT-safe` (D5).

### Known gaps in the GCS you must not paper over

- a3's `static_assert(__AVX2__)` needs C++17's single-argument form **and** `/arch:AVX2` (else the
  macro is undefined and the assert reads as `static_assert(0)` — or fails to compile). C++20 is
  already set on the x64 configs, so only the flag is missing; D4 settles the form as
  `#ifndef __AVX2__` + `#error` for a readable message. M1 applies both.
- p3 literally says "keep scalar baseline; … run-time CPUID dispatch", which reads as a scalar
  fallback path; a2/a8 plus D5 override that for this project — scalar survives as an *oracle* and as
  the right choice where SIMD is not faster, never as a shipped fallback build.
- tc2 mandates **CRLF source files**. Every source/build file in the repo is CRLF today, but nothing
  enforces it — Linux sessions default to LF. M1 adds `.gitattributes` (`*.cpp`/`*.h` etc.
  `text eol=crlf`) so this cannot drift. Until it exists, check line endings manually before committing.
- Two shared headers (`SIMD management.h`, `vector structures.h`) still carry the pre-r17 boxed
  banner, `typedefs.h` writes the nonconforming ISA token `AVX512` and un-numbered `To Do:` items, and
  the allocator family is lowercase (`amalloc`, `salloc`, `mzero`) against r11's PascalCase. These are
  owner-authored files: **report them, do not fix them here.**
- `memory management.h` documents a dependency on `data tracking.h`, which is absent from this repo
  (see "Shared headers").

## Conversion engine — non-negotiable correctness rules

Full detail with rationale lives in `CONVERSION_REFERENCE.md`; these are the invariants every
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

## Planned architecture (none of this exists yet — build it via the Roadmap)

```
src/
   main.cpp              wmain + SetConsoleOutputCP(CP_UTF8) + wiring only; wide APIs for all paths
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
   Diag.h/.cpp           error codes/messages → stderr; exit-code mapping
tests/                   fixtures/<case>/src/ (unzipped part trees) + expected.md; make_fixtures.py;
                         run_golden.py; unit tests as a second console .vcxproj with a tiny CHECK header
bench/                   GCS p4 microbenches (create with the first performance claim)
docs/                    module guides (d2/d3); CONVERSION_REFERENCE.md currently lives at the repo
                         root — moving it here must update every reference in the same commit
```

There is **no `third_party/`** and there will not be one (D1/D2): the shipped binary is first-party
code plus the CRT/Win32 and the five shared headers at the repo root, which every module may include.

Allocation-conscious modules (GCS p2 hot set): `Inflate`, `ZipReader`, `XmlPull` (zero-allocation
steady state), `DocWalker`, `RunCoalescer`, `MdEmitter` (single growable buffer), `Utf` — all
allocating through `memory management.h`. The parsed-once models (`StyleModel`, `NumberingModel`,
`OpcPackage`, `CliOptions`) use the same allocators but are not hot.

### Target CLI (implemented incrementally from M2)

```
Usage: DOCXtoMD [options] <input.docx> [output.md]
  -o, --output <path>    explicit output path        --media-dir <dir>   image dir (default <stem>_media\)
  --no-images            alt text only               --hard-break=<backslash|spaces>  (default backslash)
  --stdout               markdown to stdout          -q, --quiet         errors only
  --version              print version, exit 0       -h, --help          usage, exit 0
```

Exit codes (stable API): 0 success · 1 usage error · 2 input not found/readable · 3 not a valid DOCX ·
4 output write failure · 5 internal error.

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
  5. `CHANGELOG.md` updated (once it exists, from M1 on).
- Never leave the repo in a non-building state at the end of a turn; every commit builds.

## Roadmap

Work the **first non-`[done]` milestone** unless the user directs otherwise. On completion — in the
same commit — flip its marker here, update the "Current state" section to match the repo, and update
the prolog `To Do:` lists. A Linux session that finishes a milestone's work but cannot run its DoD
commands flips the marker to `[done-unverified]` and lists the unrun commands in the commit message;
only a run whose DoD commands pass on Windows flips it to `[done]`, and the next Windows session
verifies (not reimplements) `[done-unverified]` milestones before starting new work.

- **M1 `[next]` Compliance bootstrap** — now also carries the executable half of D3 and D4:
  - add `CHANGELOG.md` (c2/c3), `.clang-format` per tc1 (IndentWidth 3, UseTab Never, ColumnLimit 180,
    BreakBeforeBraces Attach, AllowShortFunctionsOnASingleLine All, align decls/assigns/comments),
    `.editorconfig` per tc2 (UTF-8, CRLF, indent 3, max_line_length 180), `.gitattributes` (CRLF for
    source; leave the Markdown docs as they are);
  - **D3**: delete the `Debug|Win32` and `Release|Win32` `ProjectConfiguration` entries and every
    `Condition="'$(Configuration)|$(Platform)'=='…|Win32'"` `PropertyGroup` / `ImportGroup` /
    `ItemDefinitionGroup` from `DOCXtoMD.vcxproj`. The `.sln` already lists x64 only — no change there.
    `<LanguageStandard>stdcpp20</LanguageStandard>` is already set on both surviving configs (it
    supersedes the C++17 this file used to call for, and satisfies a3's single-argument
    `static_assert`), so nothing to add;
  - **D4**: add `<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>`
    to both x64 `ItemDefinitionGroup`s, and put the `#ifndef __AVX2__` + `#error` guard in
    `DOCXtoMD.cpp` (temporary — it moves to `src/BuildGuards.h` at M2);
  - add the missing `<ClInclude>` ItemGroup to `DOCXtoMD.vcxproj.filters` with all five headers under
    Header Files;
  - add the r17 prolog to `DOCXtoMD.cpp` (temporary — superseded by `src/main.cpp` at M2).

  DoD: x64 Debug **and** Release build clean at `/W3`; the prolog passes the r17 regexes; a
  `/p:Platform=Win32` invocation fails instead of building; temporarily clearing
  `EnableEnhancedInstructionSet` makes the build stop on the `#error`.
- **M2 `[todo]` CLI skeleton** — `wmain`, `src/` layout starts (`main.cpp`, `BuildGuards.h`,
  `CliOptions`, `Diag`), usage/help/version, exit codes 0/1/2. Retire `DOCXtoMD.cpp` in the same
  commit: delete it, carry its prolog and the `__AVX2__` guard forward into `src/main.cpp` /
  `src/BuildGuards.h` (updating `File:`/`Description:`), and swap the `.vcxproj` + `.filters` entries
  per the MSBuild file-list rule. DoD: no-args prints usage and exits 1; `--version` exits 0.
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

## Decisions (settled — do not re-litigate per session)

Ruled by the owner on 2026-08-18. Keep the IDs stable: `CONVERSION_REFERENCE.md` §6.2 cites "D2 in
CLAUDE.md" by name. New questions get the next free ID (D6, D7, …) with the same
question/recommendation/status shape, and stay `Open — owner call` until the owner rules.

| ID | Question | **Ruling** | Executed? |
|---|---|---|---|
| D1 | ZIP/DEFLATE: vendor miniz vs hand-rolled inflate vs zlib | **Hand-rolled inflate.** First-party `Inflate` + `Crc32` + `ZipReader`; no `third_party/`, no vendored code | M3 |
| D2 | XML: hand-rolled pull parser vs pugixml | **Hand-rolled pull parser.** First-party `XmlPull`; pugixml is off the table | M4 |
| D3 | Win32 configs vs GCS a2 ("32-bit unsupported") | **Drop the Win32 configurations** from `DOCXtoMD.vcxproj`; x64 is the only platform | M1 |
| D4 | Adopt a3: `/arch:AVX2` + `__AVX2__` guard on x64 | **Adopt**, with the guard as `#ifndef __AVX2__` + `#error` (not `static_assert`) | M1 |
| D5 | Does a2's tech cut-off (no 32-bit, no SSE-only, no single-threaded) bind this tool? | **Baseline is SIMD, single-threaded**: AVX2 floor with no sub-baseline fallback; single-threading is an owner-granted exception to a2 | standing |

Consequences already folded into this file: the "no third-party code" line in Do NOT and the removal
of `third_party/` from the architecture (D1/D2); the first-party `Inflate`/`Crc32` modules and the
CRC-32-vs-CRC-32C trap in rule 11 (D1); the x64-only Build & run section (D3); and the "ISA and
threading baseline" subsection, which is where D4 and D5 actually live (D4/D5).

## Repo conventions

- Commit messages: imperative summary line; never name AI models in commit messages, code comments,
  or PR text (standard Claude Code attribution trailers are fine).
- `CHANGELOG.md` (from M1): Keep-a-Changelog style per c3; prologs stay history-free (c1).
- License field in every prolog: `License: MIT  Copyright: David William Bull` (two spaces).
- `CONTRIBUTING.MD` and `GDC_GCS_v1_1_4.md` are owner-managed — do not edit them. The five shared
  headers are owner-authored library files — do not reformat or re-version them. Raise conflicts as
  numbered decisions instead (like D1–D5 above).
