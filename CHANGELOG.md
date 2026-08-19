# Changelog

All notable changes to DOCXtoMD are recorded here, per GCS c2. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with the c3 grouping —
Added / Changed / Fixed / Removed / Perf. Nothing has been released yet, so every entry
sits under `[Unreleased]`. File prologs carry no history (GCS c1); this file is the history.

## [Unreleased]

### Added

- `.clang-format` per GCS tc1 — 3-space indent, no tabs, 180-column limit, attached braces,
  short function bodies on one line, and aligned declarations, assignments and trailing
  comments, over a `BasedOnStyle: LLVM` base — so every option tc1 does not name is LLVM's
  default. Further keys keep the formatter from breaking rules stated elsewhere:
  `AlignConsecutiveMacros` (r12), `SpaceBeforeParens: Never` (r13),
  `AllowShortIfStatements`/`Loops`/`CaseLabelsOnASingleLine` to preserve r3/r4's brace-less short
  forms, `AllowShortBlocksOnASingleLine: Never` so a braced control block never collapses and
  breaks r14, `SortIncludes: Never` (the shared headers' include order is load-bearing), and
  `ReflowComments: false` (an r17 prolog is regex-validated byte for byte).
  `NamespaceIndentation: All` matches `SIMD management.h`.
- `include/.clang-format` with `DisableFormat: true` — the six headers there are owner-authored
  and must not be reformatted, and the repository style would otherwise rewrite them by
  thousands of lines. Verified: formatting all six is now a no-op.
- `.editorconfig` per GCS tc2 — UTF-8, CRLF, 3-space indent, 180-column limit, plus
  `indent_style = space` (r8) and `tab_width = 3`. The Markdown
  documents and `LICENSE` keep their authored line endings, `.sln` keeps Visual Studio's tab
  indentation, and `.sln` and `.filters` are `utf-8-bom` because both ship with a BOM that a plain
  `utf-8` charset would strip — four `RULE-DEV`-tagged exemptions in all. The Markdown glob is
  `[*.{md,MD}]`, not `[*.md]`: EditorConfig globs are case-sensitive and the repository holds a
  `CONTRIBUTING.MD`.
- `.gitattributes` — sources, MSBuild files and the tooling dotfiles (`.clang-format`,
  `.editorconfig`, `.gitattributes`) are stored LF and checked out CRLF on every platform, so
  tc2's CRLF requirement can no longer drift in a session on a non-Windows host. Every other
  path is `-text`, which leaves the prose files at whatever they were authored with.
- This changelog.
- `/arch:AVX2` on both x64 configurations of `DOCXtoMD.vcxproj`
  (`<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>`),
  and the matching `#ifndef __AVX2__` / `#error` build guard in `DOCXtoMD.cpp` — decision D4
  adopting the GCS a2/a3 baseline of AVX2+FMA3+BMI2. The guard fails the build; it forks nothing.
- A GCS r17 file prolog on `DOCXtoMD.cpp`.
- `src/` and the first four modules of the planned architecture, wired together by one `wmain`:
  `src/BuildGuards.h` (the `#ifndef __AVX2__` / `#error` guard, carried over from `DOCXtoMD.cpp`),
  `src/CliOptions.h`/`.cpp` (argument parsing and validation, usage and version text),
  `src/Diag.h`/`.cpp` (UTF-8 stdout and stderr writers, and the stable `EXIT_CODE` enum) and
  `src/main.cpp` (`wmain`, `SetConsoleOutputCP(CP_UTF8)`, input-readability checks, exit-code mapping).
- The whole documented command line is parsed: `-o`/`--output`, `-j`/`--threads`, `--media-dir`,
  `--hard-break`, `--no-images`, `-q`/`--quiet`, `--stdout`, `-h`/`--help` and `--version`. Every long
  option that takes a value accepts both `--name value` and `--name=value`: `--hard-break=` is
  documented with `=`, and handling that spelling uniformly is less code than special-casing one flag.
  Only `--help`, `--version`, `--threads` validation, the `--stdout` conflict checks and the
  input-readability check act at M2; the rest is recorded in `CLI_OPTIONS` for the milestones that
  consume it, exactly as D7 requires. `-h`, `--help` and `--version` are answered the moment they are
  seen, so they beat anything later on the line — including an unreadable input — while a bad option
  earlier on the line still wins.
- Inputs are held as a list from this first commit, and `-o` is declared filename-or-directory by input
  count — the usage text says so and there is one field for it either way — though nothing derives an
  output path yet, because nothing is written yet. What this buys (D7b) is that M5 and M13 add
  derivation on top instead of re-cutting the operand grammar. `--threads` defaults to
  `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` (D7a), not `GetSystemInfo`, which sees only the
  calling thread's processor group; `0` or a value above the reported core count is a usage error
  rather than being silently coerced.
- `WIN32_LEAN_AND_MEAN` and `NOMINMAX` on both x64 configurations. `<windows.h>` enters the project at
  M2 and CLAUDE.md asks for both when it does; neither hides a header this project needs, because
  `winnls.h` (`WideCharToMultiByte`) and `wincon.h` (`SetConsoleOutputCP`) sit outside that guard.
- `.gitignore`, plus the `.gitattributes` line that keeps it CRLF alongside the other tooling
  dotfiles. It covers the three things an MSVC build or Visual Studio drops into the working tree:
  `/x64/` — the project sets no OutDir, so binaries and intermediates share that one tree, and D3
  leaves no `Win32\` tree to ignore — together with `/.vs/` and `*.vcxproj.user`. Both directory
  patterns are anchored with a leading `/`, so a future path such as `tests/x64/` is not swallowed.
- The container layer, in three first-party modules (decision D1 — no vendored ZIP or inflate code):
  `src/Crc32.h`/`.cpp`, `src/Inflate.h`/`.cpp` and `src/ZipReader.h`/`.cpp`.
- `Crc32` — ZIP's CRC-32, IEEE 802.3 with the reflected polynomial `0xEDB88320`, over a 256-entry table
  a `constexpr` function builds at compile time, so no run-time initialiser exists for a worker to race
  under D6. `Crc32Update` folds one range into a running value and `Crc32` covers a whole buffer; the
  standard's pre- and post-inversion happens inside each call, so the value in and the value out are
  both finished checksums. Two `static_assert`s pin the table to published anchors. The SSE4.2
  `_mm_crc32_u*` intrinsics are deliberately unused: they implement CRC-32C, a different polynomial,
  and would validate nothing.
- `Inflate` — a first-party RFC 1951 decoder: stored, fixed-Huffman and dynamic-Huffman blocks over
  canonical decode tables with a 9-bit primary lookup and a bit-at-a-time canonical walk behind it, and
  byte-at-a-time match copies because a match may overlap the bytes it is still producing. The 32 KiB
  window falls out of keeping the whole output addressable, so a match is bounded by everything
  produced so far. The bit reader counts *held* bits separately from *real* ones: peeking past the end
  of a stream is normal, since the primary table is indexed by a fixed-width peek, while consuming past
  it is what truncation means — conflating the two would reject valid streams at their last symbol.
- `ZipReader` — end-of-central-directory discovery over the last 65,557 bytes, preferring the record
  whose comment length accounts for exactly the rest of the file; ZIP64 locator, end record and extended
  information extra field; the central directory, which is treated as authoritative so a data descriptor
  needs no special case; local headers read for their own name and extra lengths, because writers are
  allowed to differ there; methods 0 and 8 only; OLE compound-file and encrypted-entry detection; and
  the `ZIP_LIMITS` caps on archive size, per-entry size, run total, compression ratio and entry count.
  Entry names are copied into the reader's own heap so no caller holds a pointer into the archive
  bytes, and duplicate names resolve to the first record in central directory order. The name heap is
  sized by a first pass that walks the directory and sums the names, so an archive declaring a directory
  far larger than its records use cannot choose a matching allocation: on a 41.9 MB archive whose end
  record claims a 41.9 MB directory holding five short-named entries, sizing from the declared extent
  asked for 41,943,366 bytes and summing the names asks for 96.
- An archive larger than the reader will load reports that, rather than being called a suspected ZIP
  bomb: it is a size problem and may be nothing else.
- Rule 11 ("verify what you inflate") is implemented as three checks that must agree before
  `ZipReadEntry` hands over any bytes: the output buffer is exactly the declared size, so a stream that
  produces more is stopped at the byte that overruns rather than measured afterwards; the stream must
  reach exactly that size; and the CRC-32 must match what the directory entry declares. That is what
  makes a declared size safe to allocate against — it is checked, never believed. The run total is
  charged when an entry is accepted rather than when it succeeds, so an entry that inflates most of a
  cap's worth and only then fails its CRC-32 has still spent it.
- Exit code 3 is now reachable. An input that is not a usable DOCX is reported with a sentence naming
  the rule it broke — not a ZIP, an OLE compound file (so an encrypted `.docx` or a legacy `.doc`),
  encrypted entries, an unsupported compression method, a truncated archive, a malformed structure, a
  size that disagrees with the directory, a corrupt deflate stream, a failed CRC-32, or a decompression
  cap — and each of the nine ways a deflate stream can be corrupt gets its own wording.
- `DiagNoteText`, a progress writer on **stderr** so that `--stdout` can hand a document to a pipe
  uncontaminated. `-q` suppresses notes; until `Diag` owns that flag it is the caller that decides not
  to call.
- `tests/`, with the container scaffolding: `make_fixtures.py` builds every fixture and `run_container.py`
  runs the exe over them, asserting the exit code and a substring of the message. The one part tree so
  far is `tests/fixtures/minimal/src/` — five hand-authored, reviewable parts. `make_fixtures.py`
  writes its own ZIP records rather than calling Python's `zipfile`, because the hostile fixtures need
  per-field control `zipfile` does not offer; the compressed payloads still come from Python's `zlib`,
  which is what actually pins the DEFLATE behaviour the inflater is measured against. Its 28 fixtures
  force every RFC 1951 block type (`Z_FIXED`, the default strategy on a large body, and `level=0` for
  stored blocks inside a deflate stream) and cover ZIP64, data descriptors, an archive comment,
  duplicate names, a truthful 300 MiB bomb, a 1024:1 ratio bomb, an over-count archive, and the
  corrupt, encrypted and legacy-`.doc` negatives the milestone asked for. Four of them exist because a
  claim needed to be true rather than asserted: `multi-block.docx` really carries eleven blocks of mixed
  type, where the dynamic-Huffman fixture turns out to be a single one; `overlapping-matches.docx` really
  produces 64 matches whose distance is shorter than their length, which nothing else in the set did; and
  `bad-directory.docx` and `overlong-directory.docx` reach the two `ZipReader` results no other fixture
  could produce.
- `run_container.py` also reads every sound fixture back with Python's `zipfile`. `make_fixtures.py`
  writes ZIP records itself and `ZipReader` reads them itself, so the two agreeing proves less than it
  looks — one shared misreading of the format would satisfy both. An independent implementation
  decompressing every entry and checking it against the CRC-32 in its header is what closes that.
- `/tests/build/` in `.gitignore` — the `.docx` files `make_fixtures.py` writes are generated, the part
  trees are not. And `*.py text eol=crlf` in `.gitattributes`, so the test scripts follow tc2 with the
  rest of the tree; they carry no shebang, because a CRLF shebang does not survive on a POSIX host.

### Changed

- `DOCXtoMD.cpp` no longer includes `<iostream>`; it includes `typedefs.h` — resolved through
  the project's `$(ProjectDir)include` search path — and returns `si32` per r1. A note records that
  r11 does not reach `main`: the entry point is spelled by the language, not chosen by the author.
- Source and MSBuild files are now stored in the repository with LF and materialised as CRLF
  in the working tree. The bytes a checkout produces are unchanged.
- Exit codes are a named enum (`EXIT_CODE` in `src/Diag.h`) rather than prose in a document. M2 can
  return 0 (`--help`, `--version`), 1 (usage error), 2 (an input that cannot be opened) and 5. It
  returns 5, not 0, when the input is readable: the converter arrives across M3 to M11, and exit
  code 0's published contract is "all inputs converted", which this build cannot honour. `CliParse`
  returns an `EXIT_CODE` rather than a bool so that a failed allocation reports 5 without printing the
  usage text — the command line was not the problem — while a real usage error still reports 1 with it.
- The `--stdout` line of the Target CLI block in `CLAUDE.md` uses an ASCII hyphen where it used an em
  dash, so the block and the program's usage text are now byte-identical. The sources carry no BOM and
  the project does not pass `/utf-8`, so a non-ASCII byte in a narrow literal would be read in whatever
  code page the compiler is running under.
- `main.cpp`'s per-input check is now a container probe rather than a `CreateFileW` readability test.
  It opens the input as an OPC package, requires the two part names ISO/IEC 29500-2 guarantees
  (`[Content_Types].xml` and `_rels/.rels`), inflates and CRC-verifies them, and inflates
  `word/document.xml` as well when it happens to be there — purely to exercise the inflater on a real
  stream, since nothing is resolved through relationships until M4's `OpcPackage`. A sound container
  still exits 5: the converter arrives across M4 to M11, and exit code 0's contract is "all inputs
  converted".
- A run in which several inputs fail now returns the **highest** of their per-file verdicts. Exit code 6
  stays unreachable, because D7c reserves it for a run that converted something.

### Removed

- The `Win32` project configurations, and every `…|Win32` `PropertyGroup`, `ImportGroup` and
  `ItemDefinitionGroup`, from `DOCXtoMD.vcxproj` — decision D3, adopting GCS a2's ruling that
  32-bit targets are unsupported. x64 is the only platform, and `/p:Platform=Win32` now fails
  instead of building ([#2]).
- `DOCXtoMD.cpp`. Its r17 prolog and its `#ifndef __AVX2__` / `#error` guard live on in `src/main.cpp`
  and `src/BuildGuards.h`, and its `.vcxproj` and `.filters` entries moved with them in the same commit.

[#2]: https://github.com/Zenefess/DOCXtoMD/pull/2
