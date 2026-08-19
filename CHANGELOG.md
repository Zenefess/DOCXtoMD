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

### Removed

- The `Win32` project configurations, and every `…|Win32` `PropertyGroup`, `ImportGroup` and
  `ItemDefinitionGroup`, from `DOCXtoMD.vcxproj` — decision D3, adopting GCS a2's ruling that
  32-bit targets are unsupported. x64 is the only platform, and `/p:Platform=Win32` now fails
  instead of building ([#2]).
- `DOCXtoMD.cpp`. Its r17 prolog and its `#ifndef __AVX2__` / `#error` guard live on in `src/main.cpp`
  and `src/BuildGuards.h`, and its `.vcxproj` and `.filters` entries moved with them in the same commit.

[#2]: https://github.com/Zenefess/DOCXtoMD/pull/2
