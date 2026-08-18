# CLAUDE.md

Guidance for Claude Code sessions working in this repository. Keep this file truthful: list a command
only in the commit that makes it work, keep the "Current state" section matching the repo, and update
the Roadmap markers here whenever a milestone's state changes.

## Project

DOCXtoMD is a native Windows console application (C++, MSVC v143 / Visual Studio 2022) that converts
`.docx` (Office Open XML / WordprocessingML) files into GitHub-Flavored Markdown `.md` files, built
from scratch with no external converter tools. Two documents govern all work:

- `GDC_GCS_v1_1_4.md` — **Guild Coding Standard v1.1.4 (GCS)**. `CONTRIBUTING.MD` makes it mandatory:
  all submissions are reviewed against it. **Read it in full before writing or modifying any C++ code.**
- `docs/CONVERSION_REFERENCE.md` — the full DOCX→Markdown domain specification (OPC container,
  WordprocessingML element inventory, feature→GFM mapping table, escaping rules, edge cases ranked by
  real-world frequency, pipeline design). **Read the relevant sections before implementing any
  conversion milestone.**

## Current state (do not assume more exists)

- `DOCXtoMD.cpp` — stub: empty `main()` only. No other source files.
- `DOCXtoMD.vcxproj` + `.filters` — VS2022 project, toolset v143, Debug/Release × Win32/x64, Unicode,
  Console subsystem, `/W3`, SDLCheck, ConformanceMode. **No `.sln` exists.** No `<LanguageStandard>`
  is set, so MSVC compiles at its default **C++14**. No `/arch` flag is set.
- `GDC_GCS_v1_1_4.md`, `CONTRIBUTING.MD`, `LICENSE` (MIT, Copyright (c) 2026 David William Bull),
  `docs/CONVERSION_REFERENCE.md`, this file.
- **Not yet created** (GCS obligations, see Roadmap): `CHANGELOG.md`, `.clang-format`, `.editorconfig`,
  `.gitattributes`, `src/`, `tests/`, `bench/`, CI. Do not reference them as if they exist.

## Build & run

MSVC only; there is no CMake and no `.sln` — build the `.vcxproj` directly from the repo root
(VS Developer prompt, or run `vcvarsall.bat x64` first in plain cmd):

```bat
msbuild DOCXtoMD.vcxproj /m /p:Configuration=Release /p:Platform=x64   &:: canonical build
msbuild DOCXtoMD.vcxproj /m /p:Configuration=Debug   /p:Platform=x64
msbuild DOCXtoMD.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

Default output paths (no OutDir override): `x64\Release\DOCXtoMD.exe`, `x64\Debug\DOCXtoMD.exe`
(Win32 configs land in `Release\` / `Debug\`). **x64 is the canonical platform** — GCS a2 declares
32-bit unsupported (the same [MUST] sentence also declares SSE-only/single-threaded variants
unsupported, and a2/a3 mandate an AVX2+FMA3+BMI2 baseline — see D3/D4/D5); whether the Win32
configurations get deleted is an owner decision (D3 below).

**Linux/remote sessions cannot run MSVC — nothing in this project can be compiled or executed there.**
What you can still verify on Linux: `.vcxproj`/`.filters` XML well-formedness and mutual sync, GCS
mechanical rules (indent, tabs, line width, prolog regexes), and any Python fixture/golden scripts.
An optional `g++ -std=c++17 -fsyntax-only` smoke check of portable code is a courtesy, not a gate —
MSVC v143 is the only supported compiler. **Never claim the build passes when you could not run
msbuild; state exactly what was and was not verified.**

### MSBuild file-list rule (silent-failure trap)

MSBuild compiles **only** files listed in the `.vcxproj` — there is no globbing. Every new `.cpp`
needs a `<ClCompile Include="..."/>` and every new `.h` a `<ClInclude Include="..."/>` in
`DOCXtoMD.vcxproj`, plus a matching entry in `DOCXtoMD.vcxproj.filters` (the `.filters` file only
affects the IDE tree, but a mismatched entry breaks project load in VS). Update both **in the same
commit** that adds the file. The `.filters` file has no `<ClInclude>` ItemGroup yet — add one when the
first header lands.

## Coding standard (GCS v1.1.4) — the rules you will otherwise break

`GDC_GCS_v1_1_4.md` is the single source of truth; rule IDs below cite it. This cheat sheet exists
because standard C++ habits violate nearly all of these. Intentional deviations must be tagged
`// RULE-DEV:<rule-id> <why>` (en3) — never deviate silently.

| Rule | Requirement |
|---|---|
| r8 | Indent **3 spaces**. Never tabs. |
| e2/r7 | Lines ≤150 columns; hard cap 180. |
| r1 | Width/sign-encoded scalar aliases only: `ui8 ui16 ui32 ui64`, `si8 si16 si32 si64`, `fl32 fl64`. CI bans new `f32`/`f64` spellings (en2). |
| r2/t2 | const/volatile and indirection live in **typedefs, not identifiers**: `cui32` = `const ui32`, `ui32ptr` = `ui32*`, `cui32ptr` = `const ui32*`, `ui32ptrc` = `ui32* const`, `cui32ptrc` = `const ui32* const`. Leading `c` binds the pointee, trailing `c` binds the pointer, repeat per indirection. |
| t3 | Never mix alias forms with raw `const T*` style in the same TU (CI-checked, en2). |
| r11 / r12 | Functions **PascalCase**; tables/macros/global constants **UPPER_SNAKE**. |
| r13 | Control structures: no space before `(`, exactly one space after each `;` — `if(x)`, `for(ui32 i = 0; i < n; ++i)`. |
| r14/r15 | `{` on the same line as the control statement / function signature (functions: **exactly one space** before `{`; never on its own line). `}` on its own line, except a function body that fits on the signature line within e2 may close there. |
| r3/r4 | Spreadsheet-style padding where it locally helps readability; same-line statements only when r3 justifies them, separated by **exactly three spaces**. |
| r5/r6/d1 | `///` with `@param`/`@return`/`@tparam`/`@note` for API docs only (public APIs require it); `//` for notes; `//==`/`//--` grouping headers. Disable >5 lines of code with `/* */`, else `//`. |
| r17 | Every source file opens with the validated prolog (template below). |
| c1/c2 | **No history in prologs** — record changes in root `CHANGELOG.md` (`[Unreleased]` + Added/Changed/Fixed/Removed/Perf per c3). |
| p1 | `inline` in headers only when profile-hot and ODR/size safe; else in `.cpp`. |
| p2 | Explicit alignment-aware allocators with matching frees (family to be authored in `Alloc.h` — see Known gaps). |
| p4/bd1/bd2 | Performance-over-idiom, but every performance claim needs a benchmark diff in `bench/`; acceptance = ≥3% win or parity with meaningful simplification. |

**GCS sections that do NOT apply here:** g1–g10 (GPU/shader — this tool has no GPU code; the GPU gates
named in en1/en2 are no-ops). The graphics halves of a2/a6. The SIMD vector aliases (t1, and r2's
`vui512`-style forms) stay dormant until a SIMD path exists per p3 — ship correct scalar code first;
add AVX2 kernels behind CPUID dispatch only when a `bench/` microbenchmark proves them (see D4).

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
- No third-party `#include` outside the designated wrapper TU once a dependency is vendored (see the
  `third_party/` and `ZipReader` annotations under Planned architecture, and D1).
- No speculative SIMD: no intrinsics without a benchmark in `bench/` proving them (bd1/bd2).

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
ISA tokens come from {Scalar, SSE4.2, AVX2, AVX-512} separated by `" | "` (use the validation form,
e.g. `ISA: Scalar | AVX2`, not the template's bracket form); Thread-safety ∈ {N/A, Reentrant,
MT-safe}. `ISA: Scalar` until a file actually carries AVX2 code.

### Known gaps in the GCS you must not paper over

- a3's `static_assert(__AVX2__)` needs C++17 (single-argument form) **and** `/arch:AVX2` (else the
  macro is undefined). Tied to decision D4; prefer `#ifndef __AVX2__` + `#error` for a clean message.
- tc2 mandates **CRLF source files**; Linux sessions default to LF. M1 adds `.gitattributes`
  (`*.cpp`/`*.h` etc. `text eol=crlf`) so this cannot drift. Until it exists, check line endings
  manually before committing source.

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
   CliOptions.h/.cpp     argv → options struct; usage/version text
   Utf.h/.cpp            UTF-8 validate/transcode (UTF-16 only at the Win32 boundary)
   ZipReader.h/.cpp      sole includer of the ZIP dependency (D1); bomb/traversal caps live here
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
third_party/             vendored code — EXEMPT from GCS, never reformatted, byte-identical to upstream;
                         name/version/URL/license recorded in third_party/README.md; excluded from lint
tests/                   fixtures/<case>/src/ (unzipped part trees) + expected.md; make_fixtures.py;
                         run_golden.py; unit tests as a second console .vcxproj with a tiny CHECK header
bench/                   GCS p4 microbenches (create with the first performance claim)
docs/                    CONVERSION_REFERENCE.md + module guides (d2/d3)
```

Allocation-conscious modules (GCS p2 hot set): `ZipReader`, `XmlPull` (zero-allocation steady state),
`DocWalker`, `RunCoalescer`, `MdEmitter` (single growable buffer), `Utf`. The parsed-once models
(`StyleModel`, `NumberingModel`, `OpcPackage`, `CliOptions`) use the same allocators but are not hot.

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
- A milestone's DoD is **commands that pass**, not adjectives. Before claiming any change done:
  1. x64 Release builds with **zero warnings** at `/W3` (on Windows; on Linux say you could not build).
  2. New/changed files: prolog validates (r17 regexes), 3-space indent, no tabs, lines ≤150/180,
     `Last Modified` bumped.
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

- **M1 `[next]` Compliance bootstrap** — add `CHANGELOG.md` (c2/c3), `.clang-format` per tc1
  (IndentWidth 3, UseTab Never, ColumnLimit 180, BreakBeforeBraces Attach,
  AllowShortFunctionsOnASingleLine All, align decls/assigns/comments), `.editorconfig` per tc2 (UTF-8,
  CRLF, indent 3, max_line_length 180), `.gitattributes` (CRLF for source), set
  `<LanguageStandard>stdcpp17</LanguageStandard>` in all four configs, add the r17 prolog to
  `DOCXtoMD.cpp` (temporary — superseded by `src/main.cpp` at M2). DoD: x64 Release builds clean;
  prolog passes the r17 regexes.
- **M2 `[todo]` CLI skeleton** — `wmain`, `src/` layout starts (`main.cpp`, `CliOptions`, `Diag`),
  usage/help/version, exit codes 0/1/2. Retire `DOCXtoMD.cpp` in the same commit: delete it, carry
  its prolog forward into `src/main.cpp` (updating `File:`/`Description:`), and swap the `.vcxproj` +
  `.filters` entries per the MSBuild file-list rule. DoD: no-args prints usage and exits 1;
  `--version` exits 0.
- **M3 `[todo]` ZIP container** *(blocked by D1)* — `ZipReader` with decompression caps, plus the
  first test scaffolding: `tests/make_fixtures.py` and initial fixture part-trees (minimal valid doc
  + corrupt/encrypted/`.doc` negatives). DoD: extracts `word/document.xml` from a fixture `.docx`;
  corrupt/encrypted/`.doc` inputs exit 3 with clear messages.
- **M4 `[todo]` XML + package model** *(blocked by D2)* — `XmlPull`, `OpcPackage`, plus the unit-test
  harness (second console `.vcxproj` + tiny CHECK header under `tests/`). DoD: unit tests drive
  token streams from string literals; main part resolved via rels, not hardcoded.
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
- **M12 `[todo]` CI** — GitHub Actions `windows-latest`: msbuild x64 Release + fixture build + golden
  runner.

## Open decisions (DECIDE before the blocked milestone; do not silently pick per session)

| ID | Decision | Recommendation | Status |
|---|---|---|---|
| D1 | ZIP/DEFLATE: vendor miniz vs hand-rolled inflate vs zlib | Vendor miniz into `third_party/miniz/` behind `ZipReader` — inflate is not where this project's risk budget goes; a GCS-pure inflate may replace it later via bd1/bd2 benchmarks | **Open — owner call** (vendoring vs GCS purity) |
| D2 | XML: hand-rolled pull parser vs pugixml | Hand-rolled `XmlPull` — narrow machine-generated XML, the tool's identity/hot path, GCS-conformant | Open |
| D3 | Win32 configs vs GCS a2 ("32-bit unsupported") | Drop Win32 configurations | **Open — owner call** |
| D4 | Adopt a3: `/arch:AVX2` + `__AVX2__` guard on x64 | Adopt once D3 resolves; guard via `#ifndef __AVX2__` + `#error` | **Open — owner call** |
| D5 | Does a2's full tech cut-off bind this tool? The same [MUST] that rules out 32-bit also rules out "SSE-only, single-threaded variants", yet the plan ships scalar single-threaded code first | Treat correct scalar as the p3 baseline/oracle path and tag interim code `// RULE-DEV:a2` until the owner rules | **Open — owner call** |

## Repo conventions

- Commit messages: imperative summary line; never name AI models in commit messages, code comments,
  or PR text (standard Claude Code attribution trailers are fine).
- `CHANGELOG.md` (from M1): Keep-a-Changelog style per c3; prologs stay history-free (c1).
- License field in every prolog: `License: MIT  Copyright: David William Bull` (two spaces).
- `CONTRIBUTING.MD` and `GDC_GCS_v1_1_4.md` are owner-managed — do not edit them; raise conflicts as
  decisions instead (like D1–D5 above).
