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
  cap — and each of the eight ways a deflate stream can be corrupt gets its own wording.
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
- `/tests/build/` and `__pycache__/` in `.gitignore` — the `.docx` files `make_fixtures.py` writes are
  generated, the part trees are not, and CPython drops a `__pycache__/` wherever one script imports
  another, which `run_container.py` does. `__pycache__/` is deliberately the one pattern with no
  leading `/`: it can appear in any directory, while every build-output pattern is anchored to the
  repository root. And `*.py text eol=crlf` in `.gitattributes`, so the test scripts follow tc2 with the
  rest of the tree; they carry no shebang, because a CRLF shebang does not survive on a POSIX host.
- `src/Utf.h`/`.cpp` — UTF-8 validation over a 256-row lead-byte table built by a `constexpr` function,
  so no run-time initialiser exists for a worker to race. The table is Unicode 15.0 table 3-7: each row
  carries the sequence length, the range the *first* continuation byte may take, and what a byte inside
  `80..BF` but outside that range means, which is how an overlong form, an encoded surrogate and a code
  point above U+10FFFF are each reported as their own class rather than lumped together — and each with
  the offset of the offending sequence. Also `UtfDecode`/`UtfEncode` for one code point, `UtfFromWide`
  for the console and path boundary, and `UtfTranscodeUtf16` for a part that turns out to be UTF-16.
  The two UTF-16 directions differ deliberately: the console path substitutes U+FFFD for a lone
  surrogate, because a path that cannot be represented should still be reported; a part is refused,
  because that is document content. It is scalar on purpose — an AVX2 ASCII skip is a p4 performance
  *claim*, so it waits for a `bench/` diff (bd1/bd2).
- `src/XmlPull.h`/`.cpp` — the first-party streaming pull tokenizer decision D2 settled on. Namespaces
  resolve by **URI**, never by prefix, with the ECMA-376 Transitional and ISO 29500 Strict families
  mapped onto one value, so a Strict document walks the same code as a Transitional one. `<!DOCTYPE` is
  refused at the byte it starts, before its internal subset is looked at, which is the whole
  billion-laughs and XXE defence and costs nothing; there is no entity table, so any reference but XML's
  five and a valid character reference is an error. A decoded reference is never re-scanned — `&#38;#38;`
  yields the five bytes `&#38;` — which is a security property rather than a nicety. Elements, attributes
  and namespace bindings live in inline arrays with ceilings (256, 128, 128), so deep nesting is a
  refusal rather than a blown stack. Steady state allocates nothing: a text run or attribute value
  holding no reference, carriage return or CDATA is handed back as a view into the part itself, and the
  one scratch arena is allocated lazily at the part's own size and never grows, because decoding always
  produces fewer bytes than it consumes. XML 1.0's `Char` production is enforced, which matters above
  this module: a NUL is well-formed UTF-8, and `OpcPackage` copies attribute values into NUL-terminated
  storage.
- `src/OpcPackage.h`/`.cpp` — the package model, and where "resolve, never hardcode" is actually kept.
  The only names read by name are `[Content_Types].xml` and `_rels/.rels`, the two ISO/IEC 29500-2
  guarantees; the main document part comes from the `officeDocument` relationship. `OpcResolveTarget` is
  pure and allocation-free: dot segments are removed inside the package namespace only, a climb above
  the root is refused rather than clamped the way RFC 3986 discards it, percent escapes are decoded
  *after* normalising and the result re-checked (closing the `%2e%2e` bypass), and a URI scheme is
  refused outright — one rule covering `file://`, an `http://` target mislabelled Internal, and a bare
  `C:` drive letter, which is a grammatically valid one-letter scheme. Parts are inflated once and
  cached, which is a correctness matter and not only a speed one: `ZipReadEntry` charges its
  decompression cap on every read and never credits it back. `OpcLoadXmlPart` is the only door to a
  tokenizer, which is how M4's "rather than reaching the walker" is structural rather than a convention.
- `tests/unit/` and `tests/DOCXtoMD.Tests.vcxproj` — the unit-test harness the roadmap asks for: a
  `CHECK` macro over `typedefs.h` and `<stdio.h>` and nothing else, and one suite per module, 356 checks in all, every
  case driven from a string literal so the binary needs no working directory and no fixture path. The
  project is modelled on `DOCXtoMD.vcxproj` line for line and compiles every `src\*.cpp` but `main.cpp`.
  It differs in two ways, both deliberate: `$(ProjectDir)..\src` on the include path, because a file in
  `tests\unit\` cannot reach a `src\` header by the quoted-include rule; and a pinned `OutDir`/`IntDir`,
  because MSBuild's default is `$(SolutionDir)`-relative and the binary would otherwise move depending on
  whether the solution or the project was built. `DOCXtoMD.sln` gains the project and `.gitignore` gains
  `/tests/x64/`.
- `tests/fixtures/relocated/src/` — M4's definition-of-done fixture, built so that a by-name
  implementation cannot pass it: no `word/` folder anywhere, the body at `parts/body.xml` reached through
  `rId7` rather than `rId1`, the styles part at `shared/theme-styles.xml` reached through a `../` target,
  and the body spelled with the prefix `x:` rather than `w:`.
- Twenty-one new fixtures in `make_fixtures.py`, all of them sound archives so that the failure under test
  is the package's and never the container's: `relocated-main`, `strict-namespaces`, `bom-part`,
  `utf16-part`, `main-by-content-type` and `content-type-mismatch` convert as far as M4 goes; `bad-utf8`,
  `truncated-utf8`, `doctype`, `malformed-xml`, `bad-content-types`, `no-office-rel`, `external-main-rel`,
  `traversal-target`, `encoded-traversal`, `drive-letter-target`, `bad-document-rels`, `bad-rels-root` and
  `too-many-overrides` are refused with the sentence each one earns; and `decoy-main-rel` and
  `mixed-case-names` came later with the review fixes, each mutation-tested to prove it bites — the
  first fails if content-type resolution is disabled, the second if case folding is. 356 unit checks
  and 89 container checks in total, the latter being 49 fixtures + 4 command lines + 35 `zipfile`
  cross-checks + 1 absent input.
- Decisions **D8**, **D9**, **D10** and **D11**, raised by M4 and **ruled by the owner the same day**, who
  accepted all four recommendations as written. D8 settles a direct conflict between two governing
  documents over ill-formed UTF-8, in favour of refusing the part. D9 settles what "cross-check" means
  when a relationship and a content type disagree: the relationship decides. D10 leaves ZIP entry names
  carrying a backslash or a traversal shape exactly as they are and hands the question to M11, to be
  answered against the producer-variance corpus rather than guessed. D11 commits the mechanical GCS
  validator at M12 with CI, `include/` exempt. No decision is open.

- **M5: the converter.** Six new modules turn a resolved package into Markdown, so `DOCXtoMD` writes a
  `.md` file and exit code 0 is truthful for the first time.
  - `src/StyleModel.h`/`.cpp` — `styles.xml` as a resolved-property cache. It folds each style's whole
    `w:basedOn` chain once at load, with a cycle guard and a sixteen-link cap, and resolves a run's
    effective properties by ISO/IEC 29500-1 17.7.3: a toggle the run's own `w:rPr` names is final, a
    `w:docDefaults` true beats every style, and otherwise the value is the XOR of every explicit true
    across the paragraph-style and character-style chains, so an even number of them cancels. Style
    roles come from the normalized `w:name` and, failing that, the normalized `w:styleId`, which is what
    lets `Heading_20_4` and a localized identifier under an English name both resolve.
  - `src/Ir.h`/`.cpp` — the intermediate representation: blocks and spans over one growable arena, with
    a mark-and-rewind pair. A block that holds nothing but ASCII whitespace is unwound completely,
    arena and all, which is what collapses runs of empty paragraphs for free.
  - `src/DocWalker.h`/`.cpp` — the body walk. `w:ins` and `w:moveTo` are transparent and `w:del` and
    `w:moveFrom` are dropped, which is correctness rule 8's accept-all policy; `w:sdt`, `w:smartTag`,
    `w:customXml`, `w:hyperlink`, `w:fldSimple`, `w:dir`, `w:bdo` and a `w:ruby`'s `w:rubyBase` are
    transparent at the level each appears at; a hidden run is dropped with its text.
  - `src/MdEscape.h`/`.cpp` — the context-aware escaping writer of correctness rule 6, pure and
    allocation-free, with the line-start and heading-closing-sequence rules as post-passes over a
    finished line rather than as contexts, because those two patterns can only be judged once a whole
    line exists.
  - `src/MdEmitter.h`/`.cpp` — one growable UTF-8 buffer, the blank-line discipline, line assembly,
    hard breaks in both `--hard-break` spellings, and ATX headings.
  - `src/Convert.h`/`.cpp` — the per-file pipeline and the output-path derivation D7b's operand
    grammar needs. This is the function one worker will run when M13 adds the bounded pool.
- `DiagWriteOutBytes`, the door `--stdout` hands a converted document through. It bypasses the C
  runtime's stream deliberately: stdout is a text stream on Windows, so `fwrite` would turn every LF in
  the document into a CRLF and break the emitter's stated output contract.
- `tests/run_golden.py` — the golden runner M5 owes. It converts every golden fixture twice, once to a
  file beside the input and once through `--stdout`, and byte-compares both against the case's
  `expected.md`; the two are different code paths and only comparing both tests the contract. It also
  covers `-o` as a filename and as a directory, a trailing separator, `--stdout` with two inputs,
  `-q`, exit code 6 and a run in which everything failed.
- Five golden fixture trees — `headings`, `toggles`, `textflow`, `nostyles` and `wrappers` — and an
  `expected.md` for `minimal` and `relocated` as well. Fourteen container fixtures are registered
  against `minimal/expected.md`: stored and deflated entries, fixed-Huffman blocks, ZIP64, a data
  descriptor, an archive comment, ISO 29500 Strict URIs, a byte-order mark, a UTF-16 part, a content
  type that disagrees with its relationship, a decoy relationship, mixed-case part names and a
  duplicated entry name all have to produce the same bytes, which is a stronger statement than the exit
  code and the message substring they asserted before.
- Five unit suites — `TestStyleModel`, `TestDocWalker`, `TestMdEscape`, `TestMdEmitter` and
  `TestConvert` — driven entirely from string literals. `StyleLoadBytes` and `DocWalkBytes` exist so
  that they can be: they are the halves of `StyleLoad` and `DocWalk` that work over bytes rather than
  over a package, and the suite opens no file and builds no package.
- Exit code 6. A run that converted at least one input and failed at least one now returns it, as D7c
  says it should. The failures name themselves where they happen, so 6 is a summary rather than the
  only diagnosis.

### Changed

- `docs/CONVERSION_REFERENCE.md` 5.12 no longer says to "replace invalid sequences with U+FFFD rather
  than aborting". It says to refuse a part that is not valid UTF-8, naming the part and which rule the
  bytes broke, which is what the code has always done and what decision D8 ruled. This is the edit the ruling was for:
  until it, two governing documents told a session opposite things and CLAUDE.md had to carry a standing
  note not to "fix" either one toward the other. That note is gone with the conflict.
- `docs/CONVERSION_REFERENCE.md`'s path-traversal bullet now separates the two halves that decision D10
  found tangled: a relationship **target** of a traversal shape has been refused since M4, while what a
  ZIP **entry name** of that shape should do is deferred to M11 by the ruling.
- The roadmap entries for **M11** and **M12** carry the work D10 and D11 handed them, with a definition of
  done each, so a deferred decision is a scheduled task rather than a note. M11 may not close without
  recording an answer on entry names; M12's validator must hold `include/`'s exemption in the validator
  itself rather than only in the CI invocation, so running it by hand cannot produce a different verdict.
- The `To Do` items that named an unruled decision now name the milestone that owns the work:
  `Utf.h` drops its "substitute U+FFFD should D8 be ruled that way" item outright, and `ZipReader.h`,
  `ZipReader.cpp` and `OpcPackage.cpp` point their entry-name items at M11. `OpcPackage.cpp`'s item cited
  **D9** for a question that was always **D10**'s; that miscitation is corrected.
- `main.cpp`'s per-input check is a **package** probe rather than a container probe. It resolves the main
  document part through `_rels/.rels`, loads it through the validating loader, tokenizes it end to end and
  reports the entry count, the resolved part with its size and element count, and which of styles,
  numbering, settings, footnotes, endnotes and comments the main part relates to. `src/` now holds no
  literal part name but the two ISO/IEC 29500-2 guarantees. The two closes are on one path, because the
  package borrows the reader and a per-branch close would turn the next branch anyone adds into a
  use-after-free.
- Every package failure names the part it happened in: `not a valid DOCX; a part is malformed XML, in
  word/document.xml`. A message that says only that *something* is wrong is not a clear message when a
  package holds a dozen parts.
- `Diag`'s local `WideCharToMultiByte` is gone; wide arguments cross to UTF-8 through `Utf` like
  everything else, which is what the M4 roadmap entry asked for. Its prolog's `To Do` item 2 retires with
  it.
- `no-document.docx` inverts from exit 5 to exit 3. At M3 a package missing `word/document.xml` was sound,
  because only the two guaranteed entry points were required; at M4 `_rels/.rels` names that part and the
  archive does not contain it, so the package is refused. That inversion is the clearest single sign that
  the main part is now resolved rather than assumed.
- The expectation table carries a **`sound`** flag beside the exit code, and `run_container.py`'s
  independent `zipfile` cross-check selects on it. Until M4 "is this a well-formed ZIP" and "does this
  exit 5" were the same question; a package can now be a perfectly good archive and still not be a DOCX,
  and without the flag every new package-level negative would have dropped silently out of the
  cross-check. The dead body comparison beside it — which compared a value against itself for every row
  that was not in `SAME_BODY` — is gone.

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

- A sound container no longer exits 5 saying the converter does not exist. It converts, writes its
  Markdown and exits 0, and `tests/make_fixtures.py`'s expectation table says so for nineteen fixtures.
  The `sound` flag's default moved with it, from `code == 5` to `code == 0`.
- `src/main.cpp` is wiring again: the M4 package probe is gone, and the per-input loop calls
  `ConvertFile`.
- `StyleModel` and `DocWalker` report a container or encoding refusal with the package's own sentence
  and the package's own exit code, rather than folding every one of them into "the part could not be
  read" and exit 3. A failed allocation while reading a part is this program's fault, not the
  document's.
- Mapping row 1's ruling — heading text is never additionally bolded — is now kept in the walker, which
  clears the bold bit on a heading's spans. `IR_FMT` is the only channel the emitter has, so leaving it
  set would have M6 wrapping every heading in delimiters its style already carries.
- `-o` with a trailing separator names a directory even when there is one input. No Windows file name
  may end in a separator, so the other reading names something that cannot exist. A session-derived
  refinement of D7d rather than a departure from it.

### Fixed
- A line end inside a `w:t` reached the Markdown as a line end, so `<w:t>Total&#10;# 5</w:t>` came out
  as a paragraph followed by a heading, and a `w:t` a producer pretty-printed came out as an indented
  code block. WordprocessingML spells a break `w:br`; a newline character inside a `w:t` is interior
  whitespace and now folds to one space, a CR and LF pair to one.
- A GFM delimiter row could attach to the line above it inside a single paragraph, so `a|b` followed by
  a hard break and `-|-` rendered as a table rather than as two lines of text. The line-start pass now
  escapes the head of anything shaped like a delimiter row, which is enough: a table needs both halves.
- A thematic break whose hyphens were not one contiguous run was not escaped. CommonMark counts three or
  more hyphens with any spacing between them, so `--- -` is a rule and not a line of text.
- A heading whose hard break fell between two padded runs emitted two spaces where one renders.
- `w:caps` was resolved and then discarded, so a run using it emitted lowercase where Word shows capitals
  — mapping row 37 rules that the text is uppercased. ASCII and the Latin-1 supplement are covered, which
  is where a 0x20 offset is exactly right; the rest needs Unicode's case tables and is a To Do.
- `w:webHidden` did not hide a run. It is not one of 17.7.3's toggles, so it resolves nearest-wins rather
  than by XOR, but `docs/CONVERSION_REFERENCE.md` 2.3 drops a run for it exactly as for `w:vanish`.
- `w:dir`, `w:bdo` and a `w:ruby`'s `w:rubyBase` were skipped whole, losing their text. All three are run
  containers whose content is content.
- `DocFindStyle` copied a whole 256-byte buffer into its cache when only the first few bytes had been
  written, reading indeterminate memory. Neither AddressSanitizer nor UndefinedBehaviorSanitizer sees
  that; MSVC's `/RTCu` does.
- `StyleModel`'s string heap did not reserve offset 0 for the empty string, so a style declaring no
  `w:basedOn` read offset 0 as its parent and inherited whichever identifier happened to be stored first.
  Every golden fixture's first style is `Normal` and every other style in them is based on `Normal`, so
  the bug was invisible to all seven of them.
- A dead `mc:ProcessContent` element branch, and the unit case that drove it. MCE spells `mc:Ignorable`
  and `mc:ProcessContent` as attributes, so no conformant document can carry the element; the attribute
  form is unimplemented and is now a To Do rather than a branch that looks like one.
- `tests/fixtures/wrappers` carried a `w:tbl` with no `w:tblPr` or `w:tblGrid` and an `r:id` naming a
  relationship its rels part does not declare. Neither is valid WordprocessingML, and M7 and M9 would
  have had to decide something real against a fixture typo.
- A raw `unsigned long long` cast reached the success note in `src/Convert.cpp`, in a translation unit
  that is otherwise alias-only. r1 permits the width-encoded aliases and t3 forbids mixing the two
  spellings in one file; the cast was also unnecessary, since `MdByteCount` already returns `cui64`.
- Five public accessors carried a `///` summary with no tag at all, which d1 requires of a public API:
  `IrBlockCount`, `IrFailed`, `StyleCount`, `MdByteCount` and M4's `OpcRelCount`. Every non-void
  declaration in `src/*.h` now carries a `@return`, and the convention that lets a one-argument
  accessor omit the `@param` is written down in `CLAUDE.md` rather than merely practised.
- `--stdout` reported success after a write that failed or stopped part way. `DiagWriteOutBytes`
  returned `void` and bailed out silently on a bad handle or a short `WriteFile`, so `ConvertFile`
  could not see it and the process exited 0 having emitted nothing, or half a document. It now
  returns whether every byte reached the handle and the caller returns exit 4, which is what the
  file path has always done -- it deletes a half-written `.md` on the same reasoning. Reproduced on
  Linux as `DOCXtoMD --stdout x.docx >&-`, which exited 0 before the fix and exits 4 after it; on
  Windows the same shapes are a volume that fills mid-write and a consumer that closes the pipe.
- Two of the unit suite's checks could not fail: `OpcResultText` has no path that returns null, so
  asserting it non-null asserted nothing and would have survived the whole sentence table shifting
  by a row -- the drift the same check caught in `Utf` at M4. Three rows are now pinned by content.
- `src/Convert.h` cited D7b for the rule that `-o` is a filename for one input and a directory for
  many, which is D7d; `src/Convert.cpp` already said D7d, so the module contradicted itself.
- `DocWalker.h` told the reader to see its own To Do for `m:oMath` and `w:sym`, which are named in
  `DocWalker.cpp`'s To Do instead.
- `tests/unit/Check.cpp` did not include `BuildGuards.h`, though its own prolog declared the dependency
  and every other project `.cpp` includes it first. It was the one file that could have compiled without
  D4's `#ifndef __AVX2__` guard, which is precisely the file list that guard exists to cover.
- Nineteen documentation statements that an eight-dimension audit found false against the repository, each
  one put to a skeptic told to refute it before it was acted on. The material ones follow; the two bullets
  below this one break out the rest. `CLAUDE.md`'s `Diag`
  bullet still described the `WideCharToMultiByte` call M4 deleted and called `Utf` "planned", two
  paragraphs above the bullet that says `Utf` owns every wide conversion; `.editorconfig` was described
  as carrying four `RULE-DEV` exemptions when it carries six, and the two unlisted ones included a
  4-space `[*.py]` indent that flatly contradicted two absolute statements of r8 elsewhere in the same
  file; M4's 89-check breakdown named components that summed to 93; M3's and M4's "verified mechanically"
  bullets both claimed the r17 prolog regexes and the 3-space rule had passed on `tests/*.py`, which
  carry a tagged r17 deviation and a tagged 4-space one and could not have passed either; the `XmlPull`
  bullet said two relaxations of XML 1.0 where the header documents three; and `docs/CONVERSION_REFERENCE.md`
  still called stage 2 an "in-memory DOM" and listed three main-document content types where the code
  recognises four.
- The claim, introduced with the D8 consequence edit, that a refused part is reported "naming the part
  and the byte offset". `UtfValidate` computes the offset and `OpcLoadXmlPart` discards it: the message
  names the part and which rule the bytes broke, and nothing prints an offset. Both the reference and this
  changelog said otherwise for one commit.
- Four stale counts in this file, each correct when written and never updated by the commit that changed
  it: "274 checks" (never true at any commit; 356), "297 unit checks and 85 container checks" (356 and
  89), "Eighteen new fixtures" (twenty-one, and the entry's own list named nineteen), and "nine ways a
  deflate stream can be corrupt" (eight; `INFLATE_RESULT` has eight error values and the ninth row of the
  sentence table is `INFLATE_OK`). The `.gitignore` entry also recorded one of the two patterns its commit
  added, silently dropping `__pycache__/`.
- `src/XmlPull.h`'s first `To Do` still asked for an attribute's namespace URI to be reported, which the
  review fix that made attribute identity URI-based had already delivered.

- `XmlPull` compared two attributes by their resolved namespace *value* rather than by their URI, so two
  attributes from two namespaces the build does not know — `w14:id` and `w15:id`, say, which both resolve
  to `XML_NS_OTHER` — were refused as the same attribute twice. Attribute identity is now the URI plus the
  local name, as Namespaces in XML defines it, and the URI is reported beside each attribute. Found by
  differential-testing the tokenizer against Python's expat over generated documents.
- `XML_READER` and `OPC_PACKAGE` are zeroed with `mzero`, which dispatches on **size**: a size that is a
  multiple of 32 takes a path of *aligned* 256-bit stores. `sizeof(XML_READER)` is such a size, and every
  instance is a stack local, which MSVC aligns to 8 or 16 — undefined behaviour, and a fault the moment
  the compiler lowers the store to `vmovdqa`. Both structs now carry `al32`, with a `static_assert` that
  keeps the requirement stated. This could not have been found on Linux: the shim replaces `mzero` with
  `memset`, so the real function has never run anywhere. It is exactly what the "never claim the build
  passes when you could not run msbuild" rule exists for.
- `XmlPull` rescanned to the end of a run of character data for *every* reference in it, so a run of *n*
  references cost *n* times the run's length. A 2.6 KB `.docx` holding 512 KB of `&amp;` took ten seconds;
  a 270 KB one would have taken about a month, well inside every ZIP cap. The scan is hoisted and
  recomputed only when the cursor passes it: 640 KB went from 15.9 s to 0.007 s, and the scaling is linear.
- A namespace binding stored its URI as a view into the per-token scratch arena, which the next token
  rewinds — so a URI that had to be decoded went stale, prefixes resolved to whatever the following token
  had written, and two distinct namespaces could compare equal. A well-formed part was refused as carrying
  a duplicate attribute, and a genuinely duplicated one was accepted. The arena is now rewound only to the
  floor the innermost open element set, so a binding's bytes survive as long as the binding does.
- Main-document discovery scanned the part table once per `officeDocument` relationship, and content types
  were resolved eagerly for every part against every Override row. A package could spend two minutes being
  refused, or eight seconds being *accepted*, entirely inside the ZIP caps. Candidates are now capped at
  eight — ISO/IEC 29500-2 allows exactly one — and content types resolve on the first ask and are
  remembered, which removes the product rather than shrinking it.
- A control byte in a ZIP entry name reached the console intact, so a carriage return in one could
  overwrite the line a message was printed on. Entry names are filtered where messages are composed.
- Content types were compared case-sensitively, where the specification says they are not.
- `OpcResultText(nullptr, OPC_ERROR_ZIP)` answered "the container is intact", which is the opposite of what
  it was asked.
- A truncated `<!` reported a document type declaration that was not there. It now says the part is cut off.
- `tests/unit/TestXmlPull.cpp` and `TestOpcPackage.cpp` called `printf` without including `<stdio.h>`.
  Real `<windows.h>` under `WIN32_LEAN_AND_MEAN` does not declare it, so the unit-test binary — a
  definition-of-done deliverable — could not have built under MSVC. Found by making the Linux shim
  faithful rather than convenient: a shim that includes more than the real header hides exactly this.
- `OpcResolveTarget` applied its banned-byte rule only to bytes a percent escape produced, never to
  literal ones, so a colon that did not follow a URI scheme reached a part name: `document.xml:stream`
  was refused as a scheme, but `1:stream` was not. The same held for `*`, `"`, `<`, `>` and `|`. The rule
  now runs over every byte of the finished name. A decoded name is also checked as UTF-8, because a
  percent escape can spell a byte that is not text, and such a name would simply never match an entry —
  a silent "not found" where the truth is "not a name". Found by checking every accepted target against
  an independent RFC 3986 reference.
- `UtfTranscodeUtf16` asked `UtfBomBytes` for the mark to skip, which also reports the three bytes of a
  UTF-8 one; skipping three put every UTF-16 code unit one byte out of phase with the even-length
  invariant checked immediately above it. It now recognises only the two-byte marks it can act on.
- `UtfTranscodeUtf16` returned `UTF8_OK` with a null buffer for a zero-length part, though the header
  promises a buffer on success and the caller treats a null one as "not loaded yet".
- `UtfFromWide` reported zero bytes produced after `UTF8_ERROR_SPACE`, even where it had already filled
  part of the destination.

### Removed

- The `Win32` project configurations, and every `…|Win32` `PropertyGroup`, `ImportGroup` and
  `ItemDefinitionGroup`, from `DOCXtoMD.vcxproj` — decision D3, adopting GCS a2's ruling that
  32-bit targets are unsupported. x64 is the only platform, and `/p:Platform=Win32` now fails
  instead of building ([#2]).
- `DOCXtoMD.cpp`. Its r17 prolog and its `#ifndef __AVX2__` / `#error` guard live on in `src/main.cpp`
  and `src/BuildGuards.h`, and its `.vcxproj` and `.filters` entries moved with them in the same commit.
- The M4 package probe from `src/main.cpp`, with the two notes it printed. What it proved -- that the
  main part is resolved through relationships and not by name -- is proved better by `relocated`'s
  golden, which compares the converted bytes rather than a substring of a message.


[#2]: https://github.com/Zenefess/DOCXtoMD/pull/2
