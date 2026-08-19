# Changelog

All notable changes to DOCXtoMD are recorded here, per GCS c2. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with the c3 grouping —
Added / Changed / Fixed / Removed / Perf. Nothing has been released yet, so every entry
sits under `[Unreleased]`. File prologs carry no history (GCS c1); this file is the history.

## [Unreleased]

### Added

- `.clang-format` per GCS tc1 — 3-space indent, no tabs, 180-column limit, attached braces,
  short function bodies on one line, and aligned declarations, assignments and trailing
  comments. `SpaceBeforeParens: Never` enforces r13, and `SortIncludes: Never` keeps the
  formatter from reordering the shared headers, whose include order is load-bearing.
- `.editorconfig` per GCS tc2 — UTF-8, CRLF, 3-space indent, 180-column limit. The Markdown
  documents, `LICENSE`, and `.sln` indentation carry `RULE-DEV` exemptions.
- `.gitattributes` — sources and MSBuild files are stored LF and checked out CRLF on every
  platform, so tc2's CRLF requirement can no longer drift in a session on a non-Windows host.
  Every other path is `-text`, which leaves the prose files at whatever they were authored with.
- This changelog.
- `/arch:AVX2` on both x64 configurations of `DOCXtoMD.vcxproj`
  (`<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>`),
  and the matching `#ifndef __AVX2__` / `#error` build guard in `DOCXtoMD.cpp` — decision D4
  adopting the GCS a2/a3 baseline of AVX2+FMA3+BMI2. The guard fails the build; it forks nothing.
- A GCS r17 file prolog on `DOCXtoMD.cpp`.

### Changed

- `DOCXtoMD.cpp` no longer includes `<iostream>`; it includes `typedefs.h` — resolved through
  the project's `$(ProjectDir)include` search path — and returns `si32` per r1.
- Source and MSBuild files are now stored in the repository with LF and materialised as CRLF
  in the working tree. The bytes a checkout produces are unchanged.

### Removed

- The `Win32` project configurations, and every `…|Win32` `PropertyGroup`, `ImportGroup` and
  `ItemDefinitionGroup`, from `DOCXtoMD.vcxproj` — decision D3, adopting GCS a2's ruling that
  32-bit targets are unsupported. x64 is the only platform, and `/p:Platform=Win32` now fails
  instead of building ([#2]).

[#2]: https://github.com/Zenefess/DOCXtoMD/pull/2
