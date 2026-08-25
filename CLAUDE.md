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

- `src/` — **exists** and holds the CLI skeleton (M2), the container layer (M3), the XML and package
  layer (M4) and the converter (M5): thirty files, all
  CRLF, tab-free, ASCII-only, none over 150 columns, each carrying a validated r17 prolog at `v0.1.0`
  with `ISA: Scalar`. Unlike
  `include/`, `src/` is **not** exempt from the repository style, and all thirty are committed in
  the shape `.clang-format` produces — running the formatter over them is a verified no-op, so a
  format-on-save cannot manufacture a diff. Keep it that way: format after editing, then re-check the
  r17 prolog, since the formatter has no opinion about it. Two shapes are worth copying because they
  survive the formatter *and* stay inside e2's 150 columns: a data table gets a trailing `// range`
  comment on each row (the formatter will not join lines a comment ends, so the table keeps the shape
  the RFC prints it in), and a long call gets its arguments shortened into named constants rather than
  hand-wrapped, because the formatter rejoins any wrap that fits inside its 180-column limit. `DOCXtoMD.cpp` is **gone**: its prolog and
  D4's `#ifndef __AVX2__` + `#error` guard were carried into `src/main.cpp` and `src/BuildGuards.h` by
  the same commit that deleted it, and its note that r11 does not reach the entry-point name (the
  language spells it, so it is not an en3 deviation and needs no `RULE-DEV` tag) now sits above
  `wmain`.
  - `BuildGuards.h` — D4's guard and nothing else; `Thread-safety: N/A`, the token r17 reserves for a
    file with no executable code. Every project `.cpp` includes it first. That resolves without a new
    include path because MSVC searches the including file's own directory for a quoted include, so
    `src\` is deliberately **not** in `<AdditionalIncludeDirectories>`.
  - `Diag.h`/`Diag.cpp` — the diagnostic sink. Exports `EXIT_CODE` (all seven exit codes as one named
    enum, so the stable API lives in code rather than only in this file) and five writers:
    `DiagWriteOut`, `DiagWriteErr`, `DiagError`, `DiagErrorText` and `DiagNoteText`. Notes go to
    **stderr**, not stdout, so `--stdout` can hand a document to a pipe uncontaminated; `-q` suppresses
    them, and until this module owns that flag it is the caller that decides not to call.
    Wide text crosses to UTF-8 through `Utf`'s `UtfFromWide`, called twice — once to measure, once to
    convert — around an `amalloc`/`mdealloc` buffer (p2). That is the Win32 boundary, and `Utf` has
    owned it since M4 replaced this module's own `WideCharToMultiByte` call; there is no longer a
    `WideCharToMultiByte` anywhere in `src/`.
    `Thread-safety: Reentrant`: it holds no state and takes no lock, because at M2 nothing is shared.
    **M13 makes it `MT-safe` with `include/spinlocks.h`** (D6); do not read today's `Reentrant` as a
    promise that survives that.
  - `CliOptions.h`/`CliOptions.cpp` — `CLI_OPTIONS` plus `CliParse`, `CliFree`, `CliWriteUsage` and
    `CliWriteVersion`, over a `USAGE_TEXT` constant kept **byte-identical** to the Target CLI block
    below. The whole documented surface parses; only `--help`, `--version`, `--threads` validation and
    the `--stdout` conflict checks act at M2, and everything else is recorded for the milestone that
    consumes it. Inputs are a list from this first commit, and `-o` is *declared* filename-or-directory
    by input count — the usage text says so and `CLI_OPTIONS` has one field for it either way — but
    nothing derives an output path yet, because nothing is written yet. What M2 buys is that M5 and M13
    add derivation on top rather than re-cutting the operand grammar (D7b). Every long option that
    takes a value accepts `--name value` **and** `--name=value`; the short forms `-o` and `-j` take
    the following argument only. `--threads` defaults to
    `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)` (D7a) and refuses `0` or a value above it. `-h`,
    `--help` and `--version` are answered the moment they are seen, so they beat anything later on the
    line; a bad option *earlier* on the line still wins. `CliParse` returns an `EXIT_CODE`, not a bool,
    so `main` can tell a usage error (1, and print the usage text) from a failed allocation (5, and do
    not — the command line was fine).
  - `Crc32.h`/`Crc32.cpp` — ZIP's CRC-32: IEEE 802.3, **reflected polynomial `0xEDB88320`**, over a
    256-entry table built by a `constexpr` function, so there is no run-time initialiser for a worker to
    race. `Crc32Update` folds one range into a running value and `Crc32` does a whole buffer; the
    standard's pre- and post-inversion happens inside each call, so both the value passed in and the
    value returned are finished checksums. Two `static_assert`s pin the table against published
    anchors. Do **not** reach for the SSE4.2 `_mm_crc32_u*` intrinsics here: they implement CRC-32C,
    a different polynomial, and would validate nothing.
  - `Inflate.h`/`Inflate.cpp` — the first-party RFC 1951 decoder (D1). Stored, fixed- and
    dynamic-Huffman blocks over canonical decode tables with a 9-bit primary lookup and a bit-at-a-time
    canonical walk behind it; overlapping match copies byte at a time, because a match may overlap the
    bytes it is still producing. The 32 KiB window comes free: the whole output stays addressable, so a
    match is checked against everything produced rather than against a ring buffer. The bit reader
    separates *held* bits from *real* ones — peeking past the end of a stream is normal, since the
    primary table is indexed by a fixed-width peek, while consuming past it is what truncation means.
    `InflateRaw`'s `destBytes` is a hard cap and is where a bomb is stopped. The fixed tables are built
    **once per stream**, not once per block: an empty fixed block is ten bits and rebuilding two decode
    tables for each of them costs about 3.4 seconds a megabyte, which a hostile entry can spend for free
    because it produces no output for any cap to measure. The flag that remembers them is on the same
    stack frame as the tables, so nothing is shared between workers — and it **must** be cleared when a
    dynamic block rebuilds them, or a fixed block after a dynamic one decodes with the wrong codes.
    HLIT and HDIST are held to the RFC's 286 and 30 rather than to the 288 and 32 the fixed alphabets
    need, which is also what zlib enforces.
  - `ZipReader.h`/`ZipReader.cpp` — the container. EOCD discovery over the last 65,557 bytes preferring
    the record whose comment length accounts for the rest of the file, ZIP64 locator and end record,
    the central directory (which is authoritative, so a data descriptor needs no special case), local
    headers read for their own name and extra lengths, methods 0 and 8, OLE and encryption detection,
    and `ZIP_LIMITS` — archive, per-entry, total, ratio and entry-count caps. `ZipReadEntry` hands over
    bytes only when three checks agree: the stream may not write past the declared size, it must reach
    exactly that size, and the CRC-32 must match; the run total is charged when an entry is *accepted*,
    not when it succeeds, so an entry that inflates most of a cap's worth and then fails its CRC-32 still
    costs what it spent. Entry names are copied into the reader's own heap, sized by a first pass that
    walks the directory and sums the names — an archive that declares a directory far larger than its
    records use would otherwise get to choose a matching allocation. No caller holds a pointer into the
    archive bytes, and duplicate names resolve to the **first**
    record in central directory order. That last choice diverges from most readers, Python's `zipfile`
    among them, which take the **last**: first-wins is deterministic and refuses to let an appended
    record override an earlier one, which is the safer reading of a file no legitimate producer emits.
    It is a decision a session made, not one the owner ruled, so it is revisable — but change the
    header's documentation and `tests/build/duplicate-names.docx` together if it ever is.
  - `Utf.h`/`Utf.cpp` — UTF-8 validation and the UTF-16 boundary. `UtfValidate` walks a 256-row
    lead-byte table built by a `constexpr` function — Unicode 15.0 table 3-7, one row per lead byte
    carrying the sequence length, the range its *first* continuation may take, and what a byte outside
    that range means — so an overlong form, an encoded surrogate and a code point above U+10FFFF are all
    caught by the narrowed ranges rather than by a second pass over a decoded value, and each is reported
    as its own class with the offset of the offending sequence. `UtfDecode`/`UtfEncode` do one code point;
    `UtfFromWide` and `UtfTranscodeUtf16` cross from UTF-16, which is the only place in the project that
    assumes `wchar_t` is 16 bits (one file-scope `static_assert` pins it). The two directions differ on
    purpose: the console path replaces a lone surrogate with U+FFFD, because a path that cannot be
    represented should still be reported, while a **part** is refused, because that is document content.
    That split is **D8**, ruled 2026-08-24 — not a session's choice — and `docs/CONVERSION_REFERENCE.md` 5.12
    was corrected to match it, so the two documents no longer disagree.
    `Diag`'s `WideCharToMultiByte` is gone — every wide-to-UTF-8 conversion in the project is this module
    now. It is deliberately scalar: an AVX2 ASCII skip is the obvious next step and it is a p4 performance
    *claim*, so it waits for `bench/` (bd1/bd2).
  - `XmlPull.h`/`XmlPull.cpp` — the first-party pull tokenizer (D2). Namespace-aware by **URI**, never by
    prefix (correctness rule 2), with both the ECMA-376 Transitional and the ISO 29500 Strict families
    mapped onto one `XML_NS` value, so a Strict document walks the same code as a Transitional one.
    `<!DOCTYPE` is refused where it stands, before its internal subset is looked at, which is what makes
    the billion-laughs and XXE families cost nothing to defend against; there is no entity table at all,
    so any reference but XML's five and a valid character reference is an error. A decoded reference is
    **never re-scanned** — `&#38;#38;` yields the five bytes `&#38;` — which is a security property and
    the classic hand-rolled-parser bug. Element, attribute and namespace-binding tables are held inline
    and capped (256 deep, 128 attributes, 128 live bindings), so nesting is an array and a ceiling rather
    than a recursion that runs out of stack. Steady state allocates nothing: a text run or attribute value
    with no reference, CR or CDATA in it is handed back as a view straight into the part, and the one
    scratch arena is allocated lazily at the part's own size and never grows — decoding a reference, a
    CDATA section or a line end always produces fewer bytes than it consumes, so the part's size is a
    ceiling no token can reach past, which is also why a decoded view can be a pointer rather than an
    offset to be patched. Three relaxations of XML 1.0 are deliberate and documented in the header: a name
    may hold any byte above 0x7F without consulting the Unicode `NameChar` tables (every OOXML name is
    ASCII, and the bytes were validated as UTF-8 before the reader opened); the ban on a literal
    `]]>` in character data is not enforced, because enforcing it costs a scan and rejects nothing a
    producer emits; and neither is the ban on `--` inside a comment, for the same reason — a comment is
    skipped whole, so its contents never reach a caller. It refuses a NUL and every other byte XML's `Char` production excludes — which is
    **load-bearing above this module**, because a NUL is well-formed UTF-8 and `OpcPackage` copies
    attribute values into NUL-terminated storage.
  - `OpcPackage.h`/`OpcPackage.cpp` — the package model, and where correctness rule 1 is kept. The only
    two names read by name are the two ISO/IEC 29500-2 guarantees; the main document part comes from the
    `officeDocument` relationship in `_rels/.rels`. `[Content_Types].xml` is the cross-check, not the
    lookup: when the relationship names a part that is typed as something else the **relationship still
    decides** (a producer that omits the Override is common; one that misroutes it is not), and the
    content-type table takes over in exactly one case — the relationship *resolved* to a part the archive
    does not contain. A target that was refused outright, or one declared External, never reaches that
    path, so a traversal target can never turn into a silent conversion of whichever part happened to be
    typed as the body. That reading of "cross-check" is **D9**, ruled 2026-08-24, and `tests/build/content-type-mismatch.docx`
    pins it. `OpcResolveTarget` is pure, allocation-free and therefore the piece the unit tests hammer:
    dot segments are removed inside the package namespace only, a climb above the root is **refused**
    rather than clamped the way RFC 3986 discards it, percent escapes are decoded *after* normalising and
    the result is re-checked (which is what closes the `%2e%2e` bypass), and a URI scheme — a letter, then
    scheme bytes, then a colon — is refused outright, one rule covering `file://`, an `http://` target
    mislabelled Internal, and a bare `C:` drive letter, which is a grammatically valid one-letter scheme.
    Part names compare ASCII-case-insensitively as OPC requires, by folding on comparison rather than
    keeping a lowercase key heap — a declared divergence from `docs/CONVERSION_REFERENCE.md` 6.2 [3] that
    deletes an allocation and its failure path. Relationship ids are scoped per part, so every lookup
    takes the part the reference was found in. Parts are inflated **once and cached**, which is a
    correctness matter rather than a speed one: `ZipReadEntry` charges its decompression cap on every read
    and never credits it back, so re-reading `styles.xml` per paragraph would walk an innocent document
    into the bomb caps. `OpcLoadXmlPart` is the only door to a tokenizer — it validates UTF-8, transcodes
    a UTF-16 part in place, and is what M4's definition of done means by "rather than reaching the walker".
  - `StyleModel.h`/`StyleModel.cpp` — `styles.xml` as a resolved-property cache, and where the toggle
    XOR lives. Each style's whole `w:basedOn` chain is folded **once at load**, leaf-first into a local
    array and then applied root-first, so the nearest specification survives and the toggle parity —
    which is order-independent — comes out of the same pass; a cycle stops where it closes and the walk
    is capped at sixteen links either way. A run's effective properties are then ISO/IEC 29500-1 17.7.3
    as three bit masks: a toggle the run's own `w:rPr` names is final, a `w:docDefaults` true beats
    every style, and anything neither of them named takes the XOR of the paragraph-style and
    character-style chain parities. Everything that is not a toggle — `w:dstrike`, `w:vertAlign` — is
    nearest-wins over run, character style, paragraph style, docDefaults, in that order. Style **roles**
    come from the normalized `w:name` first and the normalized `w:styleId` second, which is what lets an
    English `w:name` over a localized identifier and a LibreOffice `Heading_20_4` both resolve; the raw
    identifier is what `StyleFind` matches, because decoding escapes on the lookup path would collapse
    `Source_20_Text` and `Source Text` into one key. A missing styles part is legal and yields an empty
    model; a **present** one that is malformed is a refusal, on the same reasoning as D8. Heap offset 0
    is seeded with the empty string, without which a style declaring no `w:basedOn` inherits whichever
    identifier happened to be stored first — a real defect the golden fixtures missed and the unit
    suite caught, because every fixture's first style is the one they would wrongly have inherited.
  - `Ir.h`/`Ir.cpp` — the intermediate representation the walker builds and the emitter reads: blocks
    and spans as arrays of POD records over one growable byte arena, addressed by offset so a growth
    invalidates nothing. `IrEndBlock` trims a block's leading and trailing break spans and then unwinds
    the whole block — records, spans and arena — when nothing but ASCII whitespace is left, which is
    what collapses runs of empty paragraphs at no cost. `IrMark`/`IrRewind` exist for one caller:
    `mc:AlternateContent`, where the first `mc:Choice` is walked speculatively and rewound if an
    `mc:Fallback` turns out to follow it. A non-breaking space counts as content, per mapping row 35.
  - `DocWalker.h`/`DocWalker.cpp` — the body walk, one dispatcher for both block and run level because
    every transparent wrapper appears at both and means the same thing at each. Accept-all revisions
    (correctness rule 8): `w:ins` and `w:moveTo` are transparent, `w:del` and `w:moveFrom` are dropped
    whole, and `w:sdt`, `w:smartTag` and `w:customXml` are transparent. A run whose effective
    `w:vanish` or `w:webHidden` is on is dropped with its text, which is what keeps Word's hidden field
    instructions out of the output; a run whose `w:caps` is on has its text uppercased, which is
    mapping row 37 and is a transform on the bytes rather than a delimiter, so it belongs here. A
    heading's spans have their bold bit cleared, because mapping row 1 rules that heading text is never
    additionally bolded and `IR_FMT` is the only channel M6 has. `w:t` text is taken literally —
    `xml:space` is the producer's business — with U+00AD removed. What is skipped whole and why:
    `w:tbl` (M9), `w:drawing`/`w:pict` (M7), `w:instrText` and `w:fldChar` (M10's field state machine),
    `w:sym` and `m:oMath` (neither has a milestone, and they are the two places text is lost rather
    than merely unformatted — both are named in `DocWalker.h`'s To Do), and anything this build has
    never heard of, which is the OOXML compatibility model. Descended into although their own meaning
    waits: `w:hyperlink`, `w:fldSimple`, the bidirectional containers `w:dir` and `w:bdo`, and a
    `w:ruby`'s `w:rubyBase`. `mc:Ignorable` and `mc:ProcessContent` are **attributes**, not elements,
    and nothing reads either yet — an element in an ignorable namespace is skipped rather than having
    its children promoted, which is a `To Do` and not a claim of MCE conformance.
  - `MdEscape.h`/`MdEscape.cpp` — correctness rule 6's context-aware writer, pure and allocating
    nothing: one core that measures when its destination is null and writes when it is not, so the two
    can never disagree about a length. The reference lists a `lineStart` context; this has none, and
    the reason is the reference's own pitfall 6 — "digits then a dot then a space" is a property of an
    assembled line, not of a run — so line starts and a heading's closing hash sequence are post-passes
    over a finished line instead. `MD_CONTEXT_HTML` is the inline set **plus** unconditional `&amp;`
    and `&lt;`, not instead of it: GFM passes a raw tag through but still parses the text between the
    tags. A setext underline needs a line above it, so the `=` rule takes a `continuation` flag while a
    thematic break does not.
  - `MdEmitter.h`/`MdEmitter.cpp` — one growable UTF-8 output buffer and one line buffer. A line is
    assembled **raw** and escaped in one piece rather than span by span, and that is a correctness
    matter: the ampersand rule looks ahead for an entity pattern, and Word fragments a run wherever a
    revision boundary falls, so escaping per span makes `A&amp;B` come out differently depending on
    where the producer happened to split it. Blocks are separated by exactly one blank line and each
    ends in one newline, so two paragraphs are `a\n\nb\n` and an empty document is zero bytes. Each
    line loses its leading and trailing ASCII padding — four leading spaces would be an indented code
    block, two trailing ones are Markdown's other hard break. A hard break with nothing after it is
    dropped and two with nothing between them collapse, because a Markdown line that is empty ends the
    paragraph and neither `--hard-break` spelling can carry an empty continuation line. Inside a
    heading a break becomes exactly one space.
  - `Convert.h`/`Convert.cpp` — the per-file pipeline: container, package, relationships, styles, walk,
    emit, write. This is the function one worker runs when M13 adds the bounded pool, which is why it
    is a module and not a lump of `main.cpp`. `ConvertOutputPath` is pure and allocation-free and is
    therefore what the unit suite hammers: D7b's rule is `-o` as a filename for one input and a
    directory for several, otherwise the input's own path with its extension replaced. A trailing
    separator names a directory whatever the input count, because no Windows file name may end in one.
    The output file is written with `CreateFileW`/`WriteFile` and **deleted again if the write does not
    finish** — a half-written `.md` that looks converted is worse than none. A derived path equal to
    the input is refused rather than overwritten.
  - `main.cpp` — `wmain`, `SetConsoleOutputCP(CP_UTF8)`, option handling, the input loop and the
    exit-code fold. There is no positional output operand (D7b) and no literal part name anywhere.
  - **What the binary does at M5**: `--help`/`--version` exit 0, a usage error exits 1 after printing
    the message and the usage text to stderr, an input that cannot be opened exits 2 and is named, an
    input that is not a usable DOCX exits **3** with a sentence saying which rule it broke **and which
    part broke it**, an output that cannot be written exits 4, and a sound package is **converted** and
    exits **0**, having written `<stem>.md` beside its input and said so in a note. `--stdout` writes
    the document to standard output instead, through `DiagWriteOutBytes`, which goes to the handle
    rather than the CRT stream so that Windows cannot turn the emitter's LF endings into CRLF. A run
    that converted something and failed something exits **6**, which D7c reserves for exactly that; a
    run in which everything failed returns the highest of their verdicts.
  - **What M5 converts and what it does not**: paragraphs, headings, hard breaks, tabs, hyphens and the
    escaping that keeps all of it from being re-read as markup. It emits **no inline delimiters at
    all** — no `**`, `*`, `~~`, backtick, `<sup>` or `<sub>` — and that is deliberate rather than
    unfinished: wrapping a span in delimiters is only safe once adjacent runs with equal formatting have
    been merged and whitespace hoisted out of the span, and both are M6's `RunCoalescer`. The effective
    formatting is resolved and carried on every span from this commit, so M6 extends the IR rather than
    rebuilding it. Lists, tables, links, images, notes and fields arrive at M7 through M10 and are
    skipped until then.

- `DOCXtoMD.sln` — **exists** (VS 17.14, UTF-8 BOM, CRLF, tab-indented) and exposes **only** `Debug|x64`
  and `Release|x64`, matching both project files exactly. It lists **two** projects since M4: `DOCXtoMD`
  and `DOCXtoMD.Tests`, each with all four configuration mappings.
- `DOCXtoMD.vcxproj` — v143, Unicode, Console, `/W3`, SDLCheck, ConformanceMode, Release
  WholeProgramOptimization. Declares **two** `ProjectConfiguration`s, `Debug|x64` and `Release|x64`
  — **D3 is executed**: every `Win32` `ProjectConfiguration`, `PropertyGroup`, `ImportGroup` and
  `ItemDefinitionGroup` is gone, and `<Keyword>Win32Proj</Keyword>` is the standard VS project
  keyword, not a platform. Both configs set `<LanguageStandard>stdcpp20</LanguageStandard>` +
  `<LanguageStandard_C>stdc17</LanguageStandard_C>` and
  `<AdditionalIncludeDirectories>$(ProjectDir)include;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>`,
  so any TU writes `#include "typedefs.h"` with no path prefix. Both configs also carry
  `<EnableEnhancedInstructionSet>AdvancedVectorExtensions2</EnableEnhancedInstructionSet>` — **D4 is
  applied** and **owner-verified on Windows**, so both x64 configurations compile with `/arch:AVX2`
  and both build clean at `/W3`. No OutDir override. Both configs also define
  `WIN32_LEAN_AND_MEAN;NOMINMAX` — added at M2, when `<windows.h>` first entered the project; neither
  hides a header this project needs, because `winnls.h` (`WideCharToMultiByte`) and `wincon.h`
  (`SetConsoleOutputCP`) sit outside the `WIN32_LEAN_AND_MEAN` guard in `windows.h`. Fifteen
  `<ClCompile>`s, all `src\…`, and twenty-one `<ClInclude>`s: the six `include\…` headers and fifteen
  `src\…` ones.
- `DOCXtoMD.vcxproj.filters` — lists the fifteen `src\*.cpp` files under Source Files and all twenty-one
  headers under Header Files, in the same order as the `.vcxproj`. Every `<ClCompile Include="…">` and
  `<ClInclude Include="…">` path matches the `.vcxproj` character-for-character; keep it that way, or
  the IDE tree stops reflecting the build. The tree is deliberately flat — there is no `src` filter
  folder, matching how the `include\` headers are already listed.
- `tests/DOCXtoMD.Tests.vcxproj` and `.filters` — **exist** as of M4: the second console project the
  roadmap asks for, modelled on `DOCXtoMD.vcxproj` line for line (v143, x64 only, Unicode, `/W3`,
  SDLCheck, ConformanceMode, `stdcpp20`/`stdc17`, `/arch:AVX2`, `WIN32_LEAN_AND_MEAN;NOMINMAX`) with its
  own `ProjectGuid`. Two things differ, both deliberately. Its
  `<AdditionalIncludeDirectories>` carries `$(ProjectDir)..\src` as well as `$(ProjectDir)..\include`,
  because a test file in `tests\unit\` cannot reach a `src\` header by the quoted-include rule the way
  a `src\*.cpp` can. And it **does** pin `<OutDir>`/`<IntDir>` to `$(ProjectDir)$(Platform)\$(Configuration)\`,
  because MSBuild's default is `$(SolutionDir)`-relative: without the pin the test binary lands in
  `x64\Release\` when the solution is built and in `tests\x64\Release\` when the project is, and a
  definition-of-done command cannot name a path that moves. The main project still sets no OutDir. It
  compiles every `src\*.cpp` except `main.cpp`, which owns `wmain`, plus the ten files in `tests\unit\`.
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
- Tooling and process files, all CRLF except `CHANGELOG.md`. The first five landed with M1;
  `.gitignore` landed with M2, when the project first produced build output worth ignoring:
  - `.clang-format` — `BasedOnStyle: LLVM` first, so anything neither tc1 nor the list below names
    is LLVM's default rather than the GCS's; check that before assuming a rule is covered. Then
    tc1's keys verbatim, and one entry per rule the formatter would otherwise break:
    `AlignConsecutiveMacros: Consecutive` (r12), `SpaceBeforeParens: Never` (r13),
    `AllowShortIfStatements`/`Loops`/`CaseLabelsOnASingleLine` so r3/r4's brace-less short forms
    survive, `AllowShortBlocksOnASingleLine: Never` so a braced control block never collapses to
    `while(n) { --n; }` and breaks r14 — it is deliberately `Never`, and setting it to `Always`
    makes the formatter manufacture r14 breaches in code that already conformed —
    `SortIncludes: Never` (the shared headers have a
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
  - `.editorconfig` — tc2's four properties plus `indent_style = space` (r8) and `tab_width = 3`
    (so a stray tab at least renders at the r8 width), with **six** `RULE-DEV`-tagged exemptions over
    six glob sections. Three are about line endings: Markdown, `LICENSE` and everything under
    `tests/fixtures/` keep their authored ones — a fixture part is *input data*, and its bytes are what
    `make_fixtures.py` compresses and computes a CRC-32 over, so an editor rewriting its line endings
    would silently change every fixture built from it. Two are about `*.sln`, which keeps Visual
    Studio's tab indentation and, with `*.filters`, is `charset = utf-8-bom` because both ship with a
    BOM and EditorConfig's plain `utf-8` means *no* BOM, so an honest `[*]` charset would strip it on
    the next save (`DOCXtoMD.vcxproj` has no BOM and is unaffected). The sixth is the one that
    contradicts a rule stated elsewhere in this file and so must not be discovered by surprise:
    **`[*.py]` sets `indent_size = 4`**, because four is Python's own convention and what the only
    audience that reads those two scripts expects. tc2's other three properties still bind them, and
    `.gitattributes` still stores them LF and checks them out CRLF like any other source file.
    The Markdown glob is `[*.{md,MD}]`: EditorConfig globs are **case-sensitive**, so a bare
    `[*.md]` silently misses `CONTRIBUTING.MD` and leaves that owner-managed LF file on `crlf`.
  - `.gitattributes` — see "Line endings" below.
  - `CHANGELOG.md` — c2/c3 Keep-a-Changelog, `[Unreleased]` only; nothing is released yet.
  - `.gitignore` — what an MSVC build or Visual Studio drops here: `/x64/` (the main project sets no
    OutDir, so binaries *and* intermediates share that tree, and D3 leaves no `Win32\` to ignore),
    `/tests/x64/` (the test project pins its own, one directory down), `/.vs/` and `*.vcxproj.user`,
    plus `/tests/build/`, which is where `tests/make_fixtures.py` writes
    the `.docx` files it zips, and an unanchored `__pycache__/`, which CPython drops wherever a script
    imports another. Every *build-output* pattern is anchored with a leading `/`, which is why
    `/x64/` did not already cover `tests/x64/` and the second entry was needed; `__pycache__/` is
    deliberately not anchored, because it can appear in any directory. Those last two are the only ones
    a Linux session produces.
  None of the six is a `<ClCompile>`/`<ClInclude>` candidate, so the MSBuild file-list rule does
  not reach them and neither project file mentions them.
- `GDC_GCS_v1_1_4.md`, `CONTRIBUTING.MD`, `docs/CONVERSION_REFERENCE.md`, `LICENSE`
  (MIT, Copyright (c) 2026 David William Bull), this file.
- Line endings: `.gitattributes` now holds the line, so this no longer needs checking by hand.
  Source and build files (`*.c`, `*.cpp`, `*.h`, `*.hpp`, `*.inl`, `*.sln`, `*.vcxproj`, `*.filters`,
  `*.props`, `*.py`, and the four tooling dotfiles) are `text eol=crlf`: Git stores LF and materialises
  **CRLF** in every working tree, on Linux exactly as on Windows, so tc2 cannot drift and a
  line-ending change can never reach a diff. Everything else is `* -text` — byte-for-byte as
  committed, whatever `core.autocrlf` a contributor has set — which is what leaves the Markdown docs
  (`CLAUDE.md`, `docs/CONVERSION_REFERENCE.md`, `CONTRIBUTING.MD`) LF, `GDC_GCS_v1_1_4.md` CRLF and
  `LICENSE` LF. The M1 commit ran `git add --renormalize .` so the stored blobs agree with the new
  attributes and nothing shows as modified; **what a checkout produces is byte-identical to before**,
  including for the six owner-authored headers, whose content was not touched.
- `docs/` **exists** and holds `CONVERSION_REFERENCE.md`; `include/` **exists** and holds the six
  shared headers; `src/` **exists** as of M2; `tests/` **exists** as of M3. None of the four is
  planned-only any more.
- `tests/` — the container and package test scaffolding, and the unit suite. `make_fixtures.py` builds
  every fixture; `run_container.py` runs the exe over them and checks the exit code and the message. Both
  are CRLF like the rest of the tree and carry **no shebang**, because a CRLF shebang does not survive on
  a POSIX host — run them as `python tests/<name>.py`. There are **two** part trees under `fixtures/`.
  `fixtures/minimal/src/` is the ordinary one: `[Content_Types].xml`, `_rels/.rels`, `word/document.xml`,
  `word/_rels/document.xml.rels` and `word/styles.xml`, hand-authored and reviewable.
  `fixtures/relocated/src/` is M4's definition-of-done fixture and is built to make a by-name
  implementation fail: there is **no `word/` folder anywhere**, the body is `parts/body.xml` reached
  through `rId7` rather than `rId1` (so an implementation that takes the first relationship picks the
  wrong one), the styles part is `shared/theme-styles.xml` reached through a `../` target, and the body's
  namespace prefix is `x:` rather than `w:` (so an implementation matching on the prefix instead of the
  URI fails). The hostile and package-level negatives are synthesised by the script
  instead, because a malformed archive is not expressible as a tree of files, which is why
  `make_fixtures.py` carries its own ~90-line ZIP writer rather than using Python's `zipfile`: the
  negatives need per-field control that `zipfile` does not offer. Being first-party on both sides is
  not circular — the compressed payloads come from Python's `zlib`, which is what actually pins the
  DEFLATE behaviour, and the sound fixtures are read back with `zipfile` on every run. Output goes to
  `tests/build/`, which is git-ignored.
  The expectation table lives in `make_fixtures.py` and `run_container.py` reads it, so a fixture and
  the exit code it should produce are declared in one place. Since M4 each row also carries a **`sound`**
  flag: whether the bytes are a well-formed ZIP an independent reader must read back. That used to be the
  same question as "does it exit 5" and no longer is, because a package can be a perfectly good archive
  and still not be a DOCX — without the flag every new package-level negative would silently drop out of
  the `zipfile` cross-check.
- `tests/unit/` — **exists** as of M4 and doubled at M5: `Check.h`/`Check.cpp` (one `CHECK` macro, a
  group heading and a pass/fail summary, over `typedefs.h` and `<stdio.h>` and nothing else — the header
  itself needs only `typedefs.h`, so a suite that includes it pulls in no I/O), `TestMain.cpp`, and one
  suite per module — `TestUtf.cpp`, `TestXmlPull.cpp`, `TestOpcPackage.cpp`, `TestStyleModel.cpp`,
  `TestDocWalker.cpp`, `TestMdEscape.cpp`, `TestMdEmitter.cpp`, `TestConvert.cpp`. Every case is driven
  from a string literal;
  nothing here opens a file, so the binary needs no working directory and no fixture path. `TestXmlPull`
  works by tokenizing a literal into a compact trace — `(name` opens, `)name` closes, `[text]` is
  character data, `$` is the end and `!n` is refusal *n* — so one string per case reads better than ten
  assertions. Both sentence tables are pinned against their enums by asserting the tail of specific
  rows, because a sentence table and the enum indexing it drift apart silently; that check caught a
  real one-row misalignment during M4. M5's suites reach the parser and the walker from string literals
  through `StyleLoadBytes` and `DocWalkBytes`, which are the halves of `StyleLoad` and `DocWalk` that
  work over bytes rather than over a package; `TestDocWalker` renders the whole intermediate
  representation into a compact trace — `H1{…}` a heading, `P{…}` a paragraph, `[text]` a span, `|` a
  break, and the letters before a bracket its formatting — so a case is one string comparison rather
  than ten assertions, and it is the only place the formatting bits are observable before M6 emits them.
- **Not yet created** (GCS obligations, see Roadmap): `bench/` and CI. Do not reference them as if they
  exist. `tests/run_golden.py` exists as of M5.

## Build & run

MSVC only; there is no CMake. A `.sln` now exists and is x64-only, so either invocation is fine —
build from the repo root (VS Developer prompt, or run `vcvarsall.bat x64` first in plain cmd):

```bat
msbuild DOCXtoMD.sln     /m /p:Configuration=Release /p:Platform=x64   &:: canonical build
msbuild DOCXtoMD.vcxproj /m /p:Configuration=Release /p:Platform=x64
msbuild DOCXtoMD.vcxproj /m /p:Configuration=Debug   /p:Platform=x64
msbuild DOCXtoMD.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

The solution builds **two** exes since M4. The main project overrides no output path, so it lands at
`x64\Release\DOCXtoMD.exe` and `x64\Debug\DOCXtoMD.exe`; the test project pins its own, so it lands at
`tests\x64\Release\DOCXtoMD.Tests.exe` whether the solution or the project was built.
Since M2 the binary has a real command line, since M3 it reads the container, since M4 it resolves the
package and since M5 it **converts**: `--help` and
`--version` exit 0, a usage error exits 1, an unopenable input exits 2, an input that is not a usable
DOCX exits 3 and is told which rule it broke and which part broke it, an output that cannot be written
exits 4, a sound package is converted and exits 0, and a run that both converted and failed exits 6.
The fixtures and their expected verdicts are checked by

```bat
python tests\make_fixtures.py                                   :: writes tests\build\*.docx
python tests\run_container.py                                   :: runs x64\Release\DOCXtoMD.exe over them
python tests\run_golden.py                                      :: converts every golden and byte-compares it
python tests\run_container.py --exe x64\Debug\DOCXtoMD.exe      :: or any other build
tests\x64\Release\DOCXtoMD.Tests.exe                           :: the unit suite; prints a tally, returns 0 or 1
```

`run_container.py` and `run_golden.py` each build the fixtures themselves, so either alone is enough.
The unit binary
is its own runner — it self-asserts and returns an exit code, so there is deliberately no
`run_unit.py` wrapping it; a wrapper would assert nothing `run_container.py` does not.
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

What a Linux session *can* do, and M2, M3 and M4 all did, is build the project's own `.cpp` files
against **shim**
headers in a scratch directory: a `windows.h` declaring only the Win32 entry points the code calls, a
`memory management.h` wrapping `posix_memalign`, and a `typedefs.h` derived from the real one by
rewriting `__intN` and `__declspec(align(N))`. That runs the code, so parser logic, control flow, exit
codes and AddressSanitizer/UndefinedBehaviorSanitizer all get exercised. It proves **nothing** about
the MSVC build: not `/W3`, not `/sdl`, and not the real shared headers. One gap M2 and M3 reported is
now closed: build the shim with **`-fshort-wchar`** and `wchar_t` is two bytes exactly as MSVC has it,
so `L"…"` literals, wide argv and `Utf`'s `static_assert(sizeof(wchar) == 2u)` all behave as they will
on Windows. glibc's own wide functions assume four bytes, so a shim built that way must not call them —
which costs nothing here, because no project file calls one either. Report the shim as what it is, and
never let it stand in for the msbuild DoD.

### MSBuild file-list rule (silent-failure trap)

MSBuild compiles **only** files listed in the `.vcxproj` — there is no globbing. Every new `.cpp`
needs a `<ClCompile Include="..."/>` and every new `.h` a `<ClInclude Include="..."/>` in
`DOCXtoMD.vcxproj`, plus a matching entry in `DOCXtoMD.vcxproj.filters` (the `.filters` file only
affects the IDE tree, but a mismatched entry breaks project load in VS). Update both **in the same
commit** that adds the file, and keep the two `Include=` paths byte-identical. Headers live in two
places and resolve differently. `include\` is on the compiler's include path via
`<AdditionalIncludeDirectories>`, so its `Include=` attributes carry the `include\` prefix while
`#include "typedefs.h"` does not. `src\` is **not** on that path: a project header is included by bare
name from a `src\*.cpp` only because MSVC searches the including file's own directory first. Either
way the `<ClInclude>` entry carries the directory prefix — the file list is about what MSBuild tracks,
not about how `cl` resolves the name.

## Coding standard (GCS v1.1.4) — the rules you will otherwise break

`GDC_GCS_v1_1_4.md` is the single source of truth; rule IDs below cite it. This cheat sheet exists
because standard C++ habits violate nearly all of these. Intentional deviations must be tagged
`// RULE-DEV:<rule-id> <why>` (en3) — never deviate silently.

| Rule | Requirement |
|---|---|
| r8 | Indent **3 spaces** in C and C++. Never tabs. (`.editorconfig` exempts `*.py` at 4 — see its bullet.) |
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
| r5/r6/d1 | `///` with `@param`/`@return`/`@tparam`/`@note` for API docs only (public APIs require it); `//` for notes; `//==`/`//--` grouping headers. Disable >5 lines of code with `/* */`, else `//`. House convention for a one-line accessor whose summary already names its only argument (`OpcRel`, `IrBlockAt`, `StyleName`, `MdBytes`): carry the `@return` and omit the `@param`, since a tag repeating the summary is noise. Anything with two arguments, or a return the summary does not spell, carries both. |
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
as a numbered decision (D13+) the way D1–D12 were raised. `include/.clang-format` enforces that
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

- No tabs; no 2- or 4-space indent in C or C++ (r8) — the two `tests/*.py` scripts are the one tagged exemption.
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
  two-argument form needs only C++11. C++20 was already set on the x64 configs, so the flag was the
  only thing missing; D4 settles the form as `#ifndef __AVX2__` + `#error` for a readable message.
  **M1 applied both** — the flag on both x64 configs, the guard in `DOCXtoMD.cpp`, which M2 moved to
  `src/BuildGuards.h` — and the owner verified both on Windows, so this gap is closed.
  `include/spinlocks.h` carries a guard of that shape — copy the **structure, not the text**: its
  `#error` message names `spinlocks.h`, and the `static_assert(__AVX2__, …)` underneath it is exactly
  the construct D4 ruled out.
- p3 literally says "keep scalar baseline; … run-time CPUID dispatch", which reads as a scalar
  fallback path; a2/a8 plus D5 override that for this project — scalar survives as an *oracle* and as
  the right choice where SIMD is not faster, never as a shipped fallback build.
- tc2 mandates **CRLF source files**, and until M1 nothing enforced it. `.gitattributes` now does
  (`text eol=crlf` on every source and build pattern), so a Linux session cannot drift a source file
  to LF: whatever it writes, the checkout is CRLF. What is still unenforced is tc2's *other* half —
  no tool checks `indent_size = 3` or `max_line_length = 180`; `.editorconfig` only asks editors
  nicely, and there is no CI or pre-commit hook to fail a violation. **D11 ruled who fixes this**: M12 commits
  the mechanical validator and runs it in CI, so this gap has an owner and a milestone rather than being a
  standing complaint. Until then it stays real — a Linux session cannot drift line endings, but nothing
  stops it from committing a 4-space indent.
- Two shared headers (`SIMD management.h`, `vector structures.h`) still carry the pre-r17 boxed
  banner, `typedefs.h` writes the nonconforming ISA token `AVX512` and un-numbered `To Do:` items,
  `spinlocks.h` declares `ISA: AVX2` although it carries no AVX2 code (its only intrinsics are
  `_mm_pause`, `__rdtsc` and the `_Interlocked*` family — by the rule above that reads as
  `ISA: Scalar`; the token appears to describe its `/arch:AVX2` build guard instead), and
  the allocator family is lowercase (`amalloc`, `salloc`, `mzero`) against r11's PascalCase. These are
  owner-authored files: **report them, do not fix them here.**
- `memory management.h` documents a dependency on `data tracking.h`, which is absent from this repo
  (see "Shared headers").
- Ill-formed UTF-8 in a part is **settled**: refuse and report, per **D8**, ruled 2026-08-24. This entry used to
  record `docs/CONVERSION_REFERENCE.md` 5.12 contradicting M4's definition of done; 5.12 was rewritten to match the
  ruling in the same commit, so there is no longer a conflict to navigate and neither document should be "fixed"
  toward the other. U+FFFD substitution is not gone, but it is now confined to one place with a stated reason: the
  console path in `Utf`, where a path that cannot be represented should still be reportable. Document content is
  refused; a filename being printed at a human is repaired.
- The project does **not** pass `/utf-8`, and the sources carry no BOM, so every narrow string literal
  must stay ASCII: a non-ASCII byte would be decoded in whatever code page the compiler runs under and
  re-encoded into the execution charset, and the tool's own output contract is UTF-8. Nothing enforces
  this — the r17 prolog is ASCII-only by rule, but a literal in the body is not. M2 keeps `USAGE_TEXT`
  ASCII by hand. Adding `/utf-8` to both configurations would settle it and is worth raising as a
  numbered decision the first time a non-ASCII literal is genuinely wanted.
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
| Heading styles / `outlineLvl` 0–8 | `#`–`######` (clamp 7–9 to `######`); heading text never additionally bolded — the walker clears the bold bit on a heading's spans, so M6 cannot re-add it |
| `Title` / `Subtitle` styles | `#` / `##`, with no demotion of a real `heading 1` — session-derived at M5, which is what `docs/CONVERSION_REFERENCE.md` row 2 asks for by calling it policy |
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
| Hidden text | Dropped, for `w:vanish` (a toggle) and `w:webHidden` (nearest-wins) alike |
| `w:caps` | The run's text is uppercased — ASCII and the Latin-1 supplement, which is where a 0x20 offset is exactly right; anything beyond needs Unicode's case tables and is a `To Do` |
| TOC (field or SDT), headers/footers, comments | Skipped |
| Soft hyphens | Removed; NBSP and smart punctuation kept verbatim |
| Output encoding | UTF-8, no BOM, LF line endings (tc2's CRLF governs source files, not program output) |
| Leading/trailing ASCII space or tab on an emitted line | Removed. Four leading spaces would be an indented code block; two trailing ones are Markdown's other hard break |
| Two hard breaks with nothing between them | Collapse to one, and a hard break with nothing after it is dropped. A Markdown line that is empty ends the paragraph, so neither `--hard-break` spelling can carry an empty continuation line |
| A hard break inside a heading | One space. An ATX heading is a single line by construction |

## Planned architecture (`docs/`, `include/`, `tests/` and sixteen `src/` modules exist — build the rest by Roadmap)

**Written so far (M2 + M3 + M4 + M5)**: `src/main.cpp`, `src/BuildGuards.h`, `src/CliOptions.h`/`.cpp`,
`src/Diag.h`/`.cpp`, `src/Crc32.h`/`.cpp`, `src/Inflate.h`/`.cpp`, `src/ZipReader.h`/`.cpp`,
`src/Utf.h`/`.cpp`, `src/XmlPull.h`/`.cpp`, `src/OpcPackage.h`/`.cpp`, `src/StyleModel.h`/`.cpp`,
`src/Ir.h`/`.cpp`, `src/DocWalker.h`/`.cpp`, `src/MdEscape.h`/`.cpp`, `src/MdEmitter.h`/`.cpp` and
`src/Convert.h`/`.cpp`, plus everything already in `docs/`, `include/` and `tests/`. Every other entry
below is still to be written — do not reference one as if it exists.

Two entries below are **not** in the list `docs/CONVERSION_REFERENCE.md` 6.3 maps the stages onto, and
both are session-derived rather than ruled. `Ir.cpp` exists because the representation needs growable
arrays, and growable arrays need real functions rather than a header full of `inline` the p1 rule does
not license. `Convert` exists because the per-file pipeline is M13's worker body: it has to be callable
from something other than `wmain` before M13 arrives, and putting the output-path derivation there is
what lets the unit suite drive it — the test project compiles every `src\*.cpp` but `main.cpp`.

```
src/
   main.cpp              wmain + SetConsoleOutputCP(CP_UTF8) + wiring only; wide APIs for all paths
   Batch.h/.cpp          input list → bounded worker pool, one file per worker at a time (D6/D7a);
                         interlocked work cursor and exit-code fold; failed-input list for the
                         end-of-run report; the only module in the tree that starts a thread
   BuildGuards.h         #ifndef __AVX2__ + #error (D4); included first by every project TU
   CliOptions.h/.cpp     argv → options struct; usage/version text
   Utf.h/.cpp            UTF-8 validate/transcode (UTF-16 only at the Win32 boundary)
                         [written at M4]
   Inflate.h/.cpp        first-party RFC 1951 DEFLATE (D1): stored/fixed/dynamic Huffman, 32 KiB window
                         [written at M3]
   Crc32.h/.cpp          ZIP CRC-32 (poly 0xEDB88320 — NOT SSE4.2 CRC-32C); entry verification
                         [written at M3]
   ZipReader.h/.cpp      EOCD/central directory/local headers, methods 0+8, ZIP64; bomb+traversal caps
                         [written at M3]
   XmlPull.h/.cpp        streaming pull tokenizer over the inflated buffer; zero-allocation, view tokens
                         [written at M4]
   OpcPackage.h/.cpp     [Content_Types].xml + rels graphs; part lookup; r:id resolution
                         [written at M4]
   StyleModel.h/.cpp     styles.xml → resolved-props cache (basedOn chains, toggle XOR, name normalization)
                         [written at M5]
   NumberingModel.h/.cpp numbering.xml → per-numId levels with overrides; runtime counters
   Ir.h/.cpp             intermediate representation (blocks/spans) — the walker never emits Markdown
                         [written at M5; the .cpp is a session addition, see above]
   DocWalker.h/.cpp      document/footnote walk → IR (fields, tracked changes, sdt, AlternateContent)
                         [written at M5]
   RunCoalescer.h/.cpp   effective-format resolution + adjacent-run merging + whitespace hoisting
   MdEscape.h/.cpp       the context-aware escaping writer (pure, unit-testable)  [written at M5]
   MdEmitter.h/.cpp      IR → Markdown text; blank-line discipline; delimiter sizing  [written at M5]
   Convert.h/.cpp        one file end to end: container → package → styles → walk → emit → write, and
                         D7b's output-path derivation. M13's Batch calls this per worker
                         [written at M5; a session addition, see above]
   MediaExtractor.h/.cpp referenced media parts → disk; content-type extensions; dedup; safe names
   Diag.h/.cpp           error codes/messages → stderr; exit-code mapping. MT-safe from M13: every
                         worker reports through this one sink, so it locks then (D6). Reentrant at M2
tests/                   fixtures/<case>/src/ (unzipped part trees) + expected.md; make_fixtures.py and
                         run_container.py [both written at M3, extended at M4 and M5]; run_golden.py
                         [written at M5];
                         unit/ holds the CHECK header and one suite per module, built by
                         tests/DOCXtoMD.Tests.vcxproj [written at M4, five more suites at M5]
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
`Ir`, `DocWalker`, `RunCoalescer`, `MdEscape`, `MdEmitter`, `MediaExtractor`, `Convert` — is
instantiated once per worker, holds no cross-file state and is never shared, so it needs no lock.
`Convert` is the whole of one worker's body from M13: everything it opens, it opens on its own stack. `CliOptions` holds
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
  --stdout               markdown to stdout - single input only
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

From M2 this block is **not just documentation**: `USAGE_TEXT` in `src/CliOptions.cpp` reproduces it
byte for byte, and `--help` prints it. Edit one and you must edit the other. It is pure ASCII on
purpose — the `--stdout` line carried an em dash until M2 — because the sources have no BOM and the
project does not pass `/utf-8`, so a non-ASCII byte in a narrow string literal would be read in
whatever code page the compiler happens to run under. Keep any new line ASCII, or add `/utf-8` first.

D7 settles this surface, so it is no longer provisional. Two consequences reach back into M2, where
`CliOptions` is first written: **hold the inputs as a list from the start** — retrofitting one later
means re-cutting argument parsing, `-o` semantics and the output-naming rule together — and **give
`-o` its two meanings from the start** (a filename when exactly one input is given, a directory
otherwise), because that branch is the whole reason the positional output operand had to go. M2 may
still accept only one input; what it must not do is assume there will only ever be one.

## Testing & definition of done

- Fixtures are **unzipped part trees** under `tests/fixtures/<case>/src/` (hand-authorable, reviewable,
  diffable) zipped into `.docx` by `tests/make_fixtures.py` — **never** PowerShell `Compress-Archive`,
  which writes backslash separators. As of M3 that script exists and writes its own ZIP records rather
  than calling Python's `zipfile`, because the hostile fixtures need per-field control `zipfile` does
  not offer; its payloads still come from Python's `zlib`, so the DEFLATE behaviour the inflater is
  measured against is not first-party. It emits **stored** and **deflated** entries, and forces every
  RFC 1951 block type — `strategy=Z_FIXED` for fixed Huffman, the default strategy on a large body for
  dynamic, and `level=0` for stored blocks inside a deflate stream, which is a different code path from
  a stored ZIP entry.
- Three runners, with different jobs. `tests/run_container.py` (M3, extended at M4 and M5) runs the exe
  over every fixture and asserts the exit code and a substring of the message, and reads every *sound*
  archive back with Python's `zipfile`; `tests/x64/Release/DOCXtoMD.Tests.exe` (M4, extended at M5) runs
  the unit suite, which drives every case from a string literal and touches no file;
  `tests/run_golden.py` (M5) converts every golden fixture twice — once to a file beside the input and
  once through `--stdout`, which are different code paths in `Convert.cpp` — and byte-compares both
  against the case's `expected.md`. None takes another's job.
- A golden fixture is a part tree under `tests/fixtures/<case>/src/` **plus** an `expected.md` beside it,
  and which built `.docx` compares against which case is declared in `make_fixtures.py`'s `GOLDENS`
  table, next to the exit-code table, so a fixture and what it must produce are named in one place. The
  mapping is many-to-one on purpose: fourteen container fixtures compare against `minimal/expected.md`,
  which is how a byte comparison came to assert what M4 asserted with a message substring.
- **Write an `expected.md` by hand, from the specification, before running the converter at it.** A
  golden generated from the implementation asserts only that the implementation is deterministic. All
  seven of M5's were derived by hand and all seven matched on the first run; when one does not, decide
  which side is wrong rather than regenerating the file.
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

- **M1 `[done]` Compliance bootstrap** — also carries the executable half of D4 (D3's landed early):
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
    `DOCXtoMD.cpp` (temporary — M2 moved it to `src/BuildGuards.h`);
  - (the `<ClInclude>` ItemGroup in `DOCXtoMD.vcxproj.filters` is **already done** — all six headers
    sit under Header Files. This M1 sub-task landed early in the `include/` resync commit, because
    listing `include\spinlocks.h` obliged the MSBuild file-list rule to fix `.filters` anyway);
  - add the r17 prolog to `DOCXtoMD.cpp` (temporary — M2 superseded it with `src/main.cpp`).

  DoD: x64 Debug **and** Release build clean at `/W3`; the prolog passes the r17 regexes; a
  `/p:Platform=Win32` invocation fails instead of building; temporarily clearing
  `EnableEnhancedInstructionSet` makes the build stop on the `#error`.
  **Status**: all four checks pass. The work landed from Linux on 2026-08-19 with only the r17
  regexes run there; the owner verified the rest on Windows the same day — x64 Debug and Release
  both build clean at `/W3` (so `si32 main()` and `#include "typedefs.h"` compile warning-free),
  clearing `EnableEnhancedInstructionSet` stops the build on the `#error`, and `/p:Platform=Win32`
  fails by way of D3.
- **M2 `[done]` CLI skeleton** — `wmain`, `src/` layout starts (`main.cpp`,
  `BuildGuards.h`, `CliOptions`, `Diag`), usage/help/version, exit codes 0/1/2. Retire `DOCXtoMD.cpp`
  in the same commit: delete it, carry its prolog and the `__AVX2__` guard forward into
  `src/main.cpp` / `src/BuildGuards.h` (updating `File:`/`Description:`), and swap the `.vcxproj` +
  `.filters` entries per the MSBuild file-list rule. Shape `CliOptions` for D7 now even while only one
  input is accepted: inputs are a **list**, `-o` means filename-or-directory by input count, and there
  is no positional output operand. DoD: no-args prints usage and exits 1; `--version` exits 0; the
  usage text matches the Target CLI block above.
  **Status**: the code landed from Linux on 2026-08-19. `CliOptions` was shaped past the minimum — it
  accepts N inputs, not one — because nothing is converted yet, so the list costs nothing to honour in
  full. **Verified on Linux**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only, CRLF,
  ≤150 columns; `.vcxproj`/`.filters` XML well-formedness and mutual sync against what is on disk; and
  `USAGE_TEXT` diffed byte for byte against the Target CLI block. The three `src/*.cpp` files were also
  compiled by `g++ -std=c++20 -Wall -Wextra` and run against **shim** `windows.h`/`typedefs.h`/
  `memory management.h` headers, giving 43 command-line cases the documented exit codes with no
  AddressSanitizer or UndefinedBehaviorSanitizer diagnostic. **That shim proves the parser's logic,
  not the build**: it is not MSVC, not the real shared headers, and `wchar_t` is 4 bytes there.
  The milestone's scope line above says "exit codes 0/1/2", and all three are reachable; the build also
  returns **5** for a readable input and for a failed allocation, for the reason given under "Current
  state". That is deliberate, and 5 was already in the published table before this commit.
  **Owner-verified on Windows, 2026-08-19**: the x64 build succeeds with no warnings and no errors,
  so the global DoD's zero-warnings-at-`/W3` check passes. That also settles the one real risk this
  commit carried: M2 is the **first** commit whose TUs compile `memory management.h`, and through it
  `common functions.h`, `vector structures.h` and `SIMD management.h` — roughly 2,000 lines of
  owner-authored code M1 never fed to a compiler — and all four come through `/W3` clean. Later
  milestones inherit that, so a warning appearing from one of them is a regression introduced by the
  new code, not a latent header problem.
  The three behavioural checks were run against `x64\Release\DOCXtoMD.exe` the same day and all
  three behave as documented: no arguments prints the usage text and exits 1, `--version` exits 0, and
  `--help` reproduces the Target CLI block. With the build and all three checks confirmed on Windows,
  M2's DoD is fully discharged and the marker is `[done]`.
- **M3 `[done]` ZIP container + inflate** *(D1 settled: first-party)* — `Inflate` (RFC 1951: stored,
  fixed-Huffman and dynamic-Huffman blocks; canonical decode tables; 32 KiB window; overlapping match
  copies), `Crc32`, and `ZipReader` (EOCD search over the last 65,557 bytes, central directory, local
  headers, methods 0/8 only, ZIP64, data descriptors, duplicate names, encryption bit) with the
  decompression caps enforced *during* inflation. Plus the first test scaffolding:
  `tests/make_fixtures.py` and initial fixture part-trees (minimal valid doc + corrupt/encrypted/`.doc`
  negatives). DoD: extracts `word/document.xml` from both a stored-entry and a deflated-entry fixture
  `.docx` with CRC-32 verified; a dynamic-Huffman payload round-trips against a Python-`zlib`-generated
  fixture; corrupt/encrypted/`.doc` inputs exit 3 with clear messages.
  **Status**: the code landed from Linux on 2026-08-19 as `[done-unverified]`, and the owner verified it
  on Windows the same day: **x64 Release and x64 Debug both build with zero warnings at `/W3`**, and
  `python tests\run_container.py` passes all 45 checks against the real MSVC binary. That discharges
  every DoD bullet — the 45 checks are what extract `word/document.xml` from a stored-entry and a
  deflated-entry fixture with CRC-32 verified, round-trip the dynamic-Huffman payload against the
  Python-`zlib`-generated fixture, and put the corrupt, encrypted and legacy-`.doc` inputs through exit 3
  with their documented message — so the marker is `[done]`. Two things that verification settles beyond
  the milestone: the shim build and MSVC agree on every exit code and every message substring those 45
  checks assert, which is the first evidence that a Linux session's harness predicts the real binary
  rather than only itself; and the new code comes through `/W3` clean on top of M2's headers, so a
  warning appearing from here on is a regression the commit that introduces it owns.
  What had been verified on Linux before that, kept because it is how the code was actually exercised:
  - **Verified on Linux, mechanically**: the r17 prolog regexes from the GCS, 3-space indent, no tabs,
    ASCII only, CRLF, and ≤150 columns on all twelve `src/` files; tabs, ASCII, CRLF and width only on
    both `tests/*.py`, which carry a tagged r17 deviation and a tagged 4-space `[*.py]` one;
    `.vcxproj`/`.filters` XML well-formedness and mutual sync with what is on disk; and
    `clang-format --style=file` a verified no-op on every file in `src/`.
  - **Verified on Linux, behaviourally, against the shim build**: `tests/run_container.py` passes all
    45 checks — 12 sound containers exit 5 after verifying their parts, 16 hostile ones exit 3 with the
    documented sentence, an absent input exits 2, four command lines behave as M2 published them, and
    Python's `zipfile` reads every sound fixture back with each entry's CRC-32 matching. On top of that,
    three scratch harnesses the
    commit does not carry: the inflater round-trips **660** payload/level/strategy combinations against
    Python's `zlib` byte for byte (33 payloads x levels 0/1/6/9 x the five zlib strategies), decodes a
    multi-block stream built with `Z_FULL_FLUSH`, and rejects all 1,788 proper prefixes of a valid
    stream plus 1,500 bit-flipped ones with no sanitizer report; `Crc32` matches `zlib.crc32` on 128
    payloads in both its whole-buffer and three-chunk forms; and 3,000 mutated archives through the
    exe produce only exit codes 3 and 5, with no AddressSanitizer or UndefinedBehaviorSanitizer
    diagnostic anywhere. And because both sides of the fixture pipeline are first-party, a
    cross-check the other way: four `.docx` files written by Python's own `zipfile` (stored, deflated,
    with directory entries, and one carrying a 3 MB binary member and a 2.7 MB part) all verify, and
    Python's `zipfile` reads back byte-identical `word/document.xml` from the deflated, ZIP64,
    data-descriptor and archive-comment fixtures. It reads a different one from `duplicate-names.docx`,
    which is the documented first-wins-versus-last-wins divergence and not a defect.
  - **Reviewed adversarially** before the commit, by ten independent readers over five dimensions (RFC
    1951 conformance, ZIP/APPNOTE conformance, memory and error paths, GCS compliance, and whether the
    milestone was actually delivered), each finding then put to two skeptics told to refute it. Twenty
    findings were raised and the two that mattered are fixed above: the fixed-table rebuild, measured at
    13.45 s versus 0.05 s on four megabytes of empty fixed blocks, and the name heap, measured at a
    41.9 MB allocation versus none on an archive whose directory declares far more extent than its five
    records use. The rest were either already fixed in the same working tree or refuted on the code.
  - **What the Linux run could not reach, and the owner's Windows run did**: `/W3`, `/sdl`,
    `/arch:AVX2`, the real `include/` headers and 2-byte `wchar_t`. The shim is a scratch
    `windows.h`/`typedefs.h`/`memory management.h` trio in a session directory, exactly as M2's was, and
    it is not committed — so a Linux session's evidence never stands in for the msbuild DoD, however
    much of it there is. That remains the rule for M4 onwards.
  Two scope notes. The fixture set is wider than the milestone asked for — it also covers ZIP64, data
  descriptors, archive comments, duplicate names, a truthful 300 MiB bomb, a 1024:1 ratio bomb and an
  over-count archive — because those paths are in `ZipReader` either way and a fixture is the only
  thing that proves them. And `tests/run_container.py` is a third file beyond the two the milestone
  names; it exists because "corrupt/encrypted/`.doc` inputs exit 3 with clear messages" is a DoD
  bullet, and a DoD bullet with no command behind it is an adjective.
- **M4 `[done]` XML + package model** *(D2 settled: first-party `XmlPull`)* — `Utf`,
  `XmlPull`,
  `OpcPackage`, plus the unit-test harness (second console `.vcxproj` + tiny CHECK header under
  `tests/`). **`Utf` is scheduled here** — owner ruling, 2026-08-19, closing the gap that no milestone
  named it. It belongs with `XmlPull` because the tokenizer runs over the inflated part bytes and must
  not tokenise what has not been validated; UTF-16 stays at the Win32 boundary only, and `Diag`'s local
  `WideCharToMultiByte` moves behind `Utf` once it exists. DoD: unit tests drive token streams from
  string literals; a part carrying invalid UTF-8 is rejected with a clear message rather than reaching
  the walker; the main part is resolved via rels, not hardcoded.
  **Status**: the code landed from Linux on 2026-08-24 as `[done-unverified]`, and the owner verified it
  on Windows the same day: both x64 configurations build with **zero warnings at `/W3`**,
  `python tests\run_container.py` passes all **89** checks against the real MSVC binary and
  `tests\x64\Release\DOCXtoMD.Tests.exe` passes all **356**. That discharges the milestone's own three
  DoD bullets and the global one, so the marker is `[done]` with nothing outstanding.
  - **The three DoD bullets, and what proves each.** (1) `tests/unit/` drives every case from a string
    literal and opens no file: 356 checks over the ill-formed UTF-8 classes, the XML token stream, the
    namespace rules and the relationship-target resolver. (2) `OpcLoadXmlPart` is the only door to a
    tokenizer — all three `XmlOpen` call sites in `src/` sit behind it — and `bad-utf8.docx` and
    `truncated-utf8.docx` assert the exit code and the sentence, which names the failing part.
    (3) `relocated-main.docx` has no `word/` folder at all, reaches its body through `rId7` rather than
    `rId1`, and spells it with the prefix `x:`; `src/` holds no literal part name but the two ISO/IEC
    29500-2 guarantees.
  - **Verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only,
    CRLF and ≤150 columns on all eighteen `src/` files and all six `tests/unit/` ones. The two
    `tests/*.py` files were checked for tabs, ASCII, CRLF and width only: neither carries an r17
    prolog — each opens with a `# RULE-DEV:r17` note and a module docstring, because r17's prolog is a
    C block comment Python cannot hold — and both are 4-space indented under `.editorconfig`'s tagged
    `[*.py]` exemption. `clang-format --style=file` a verified no-op on every C++ file in the commit; both
    `.vcxproj`/`.filters` pairs well-formed XML and mutually byte-identical, every listed file on disk;
    the `.sln`'s two project entries and four configuration mappings.
  - **Verified on Linux, behaviourally, against the shim build** (now with `-fshort-wchar`, so
    `wchar_t` is two bytes as on Windows): `tests/run_container.py` passes all **89** checks, which is
    49 fixtures + 4 command lines + 35 cross-checks + 1 absent input. Of the 49, **19** sound packages
    exit 5 after their main part is resolved and tokenized and **30** refused ones exit 3 with the
    documented sentence; the **35** are every fixture carrying the `sound` flag, read back by Python's
    `zipfile`. The unit binary passes all
    **356** checks. On top of that, three scratch harnesses the commit does not carry: 1.2M mutated XML
    parts over three bases (a minimal body, a rich one carrying namespaces, entities, CDATA, a PI and an
    `mc:AlternateContent`, and a `.rels` part) plus all 2,039 proper prefixes of those bases; 6M random
    relationship targets built from an alphabet of `/`, `.`, `\`, `%`, `:`, `?`, `#` and letters through
    `OpcResolveTarget`; and 6,000 mutated `.docx` archives through the exe, which produced only exit
    codes 3 and 5. No AddressSanitizer or UndefinedBehaviorSanitizer diagnostic anywhere.
  - **Cross-checked against independent implementations**, which is the evidence M3 got from Python's
    `zlib` and M4 gets from Python's expat and from RFC 3986: 12,000 generated documents — nested elements, prefixed and
    default namespaces, re-binding, entity and character references, CDATA, comments, processing
    instructions and mixed whitespace — tokenized by both `XmlPull` and expat produce byte-identical
    streams of namespace URI, local name, attribute and text, and the two agree on every document that
    is refused. Every part of both committed fixture trees matches too. Separately, 40,000 random
    relationship targets went through `OpcResolveTarget`, and every one of the 8,396 it accepted resolves
    to exactly what an independent RFC 3986 implementation produces. Between them those two harnesses
    found the two real defects in M4 that nothing else did: attributes were compared by their resolved
    namespace *value* rather than by their URI, so two attributes in two namespaces this build does not
    know looked like one attribute twice; and the banned-byte rule for part names ran only over bytes a
    percent escape produced, so a literal colon that did not follow a URI scheme reached a part name.
  - **Reviewed** by a survey-and-critique workflow before the code was written and by a six-dimension
    adversarial review after it, each finding then put to a skeptic told to refute it. What the critique
    changed: the `Diag` rewire had been overlooked, the failure sentences did not name the failing part,
    the `zipfile` cross-check silently stopped covering archives that had moved from exit 5 to exit 3,
    the `relocated` fixture was too weak to catch an ordinality or prefix assumption, and no fixture
    drove a package structural cap.
    The adversarial review then found eleven more, of which four matter and none was reachable from the
    tests as they stood: `mzero` dispatches on **size** and takes a path of *aligned* 256-bit stores when
    the size is a multiple of 32, which `sizeof(XML_READER)` is — so zeroing a stack-allocated reader was
    undefined behaviour and a probable fault under MSVC, invisible here because the shim replaces `mzero`
    with `memset`; the tokenizer rescanned to the end of a text run for every reference in it, so a 2.6 KB
    `.docx` took ten seconds and a 270 KB one would have taken a month; a namespace binding pointed into
    the arena the next token rewinds, so a decoded URI went stale and well-formed parts were refused; and
    two unit-test files called `printf` without including `<stdio.h>`, which real `<windows.h>` does not
    declare, so the test binary could not have built under MSVC at all. That last one was found only by
    making the shim **faithful rather than convenient** — a shim that includes more than the real header
    hides precisely this class of defect, and the lesson generalises: the shim's job is to be *stricter*
    than Windows where it cannot be identical.
  - **Known coverage gap, stated rather than papered over**: `OpcFindRelById` is on the probe's path, so
    relationship lookup by id is exercised, but the claim it is there to support -- that ids are scoped
    per part, so `rId3` in `document.xml` and `rId3` in `footnotes.xml` are unrelated -- has no test,
    because M4 loads only one part's relationships. It gets one at M7, when a second part's are loaded.
  - **What the Linux run could not reach, and the owner's Windows run did**: `/W3`, `/sdl`, `/arch:AVX2`,
    the real `include/` headers, and whether the second project builds at all. All four are now covered.
    Both configurations compile; the container suite returns the same **89** and the unit suite the same
    **356** the shim produced on Linux, so for the second milestone running a Linux session's harness
    predicted the real binary exactly rather than only itself. Two things the third command proves in
    passing, because it could not otherwise have been typed: the test project builds, and its pinned
    `<OutDir>` really does put the exe at `tests\x64\Release\`. Two of the four defects the adversarial
    review fixed were reachable **only** on this path — the `mzero` alignment fault, which the Linux
    allocator hid, and the two test files missing `<stdio.h>`, which g++ supplies transitively — and
    either one would have stopped this run dead had it survived to it.
  - **Zero warnings at `/W3`, confirmed on a rebuild.** The owner's first report said both configurations
    compiled without errors, which is not the same claim; a rebuild of Debug and Release the same day
    produced **no warnings** in either. That closes the last half of the global DoD, and it matters
    beyond bookkeeping: M2 established that the six shared headers come through `/W3` clean, so a
    warning appearing from here on belongs to the commit that introduces it rather than being a latent
    header problem. M4 puts six new `src/` files and six `tests/unit/` ones under that inheritance.
- **M5 `[done-unverified]` Paragraphs & headings** — `StyleModel` (chains + toggle XOR), minimal
  `DocWalker`/`Ir`/`MdEmitter`, plus `tests/run_golden.py` (exe runner + byte-compare + exit-code
  assertions). DoD: first golden fixture converts byte-exact.
  **Status**: the code landed from Linux on 2026-08-25. The next Windows session verifies rather than
  reimplements it, by running the four commands under "Build & run"; **only that run may flip this
  marker to `[done]`**.
  - **Scope taken past the minimum, and why each.** `MdEscape` is M6's line in the roadmap and lands
    here whole, because correctness rule 6 forbids an emitter concatenating raw text and the module is
    pure, fully specified by `docs/CONVERSION_REFERENCE.md` 4.1 and testable with no caller — M6 gets
    its remaining callers, not its code. `Convert` and `Ir.cpp` are additions to the architecture list,
    for the reasons given there. Exit code 6 is pulled forward from M13, because the published exit-code
    table is a stable API and, now that a run can partly succeed, returning anything else for a mixed
    run would make that table false. What is **not** taken early: no inline delimiter is emitted at all,
    because a delimiter is only safe after M6's coalescing and whitespace hoisting.
  - **Verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only,
    CRLF and ≤150 columns on all thirty `src/` files and all eleven `tests/unit/` ones; `clang-format
    --style=file` a verified no-op on every one of them; both `.vcxproj`/`.filters` pairs well-formed
    XML, mutually byte-identical in their `Include=` paths, and every listed file present on disk.
  - **Verified on Linux, behaviourally, against the shim build**: the unit suite passes all **809**
    checks, `tests/run_golden.py` all **47** and `tests/run_container.py` all **99**, every one of them
    under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection on and no diagnostic. The
    47 are twenty archives converted twice each — once to a file and once through `--stdout`, which are
    different code paths — plus the `-o`, `--stdout`, `-q`, exit-6 and total-failure command lines. The
    twenty archives are the five case fixtures, `relocated-main.docx`, and the fourteen container
    fixtures that carry the `minimal` document unaltered under fourteen different container and package
    shapes; those fourteen share one `expected.md`, which is how seven `expected.md` files cover twenty
    archives.
  - **The definition of done, and what discharges it**: `tests/run_golden.py` byte-compares seven
    fixture cases against a committed `expected.md`, and **each `expected.md` was written by hand from
    the specification before the converter was run against it**. All seven matched on the first run,
    which is the only reading of "byte-exact" worth having — a golden generated from the implementation
    asserts nothing.
  - **Cross-checked against an independent implementation**, which is what M3 got from Python's `zlib`
    and M4 from expat: 4,000 generated documents — nested transparent wrappers, tracked changes,
    `mc:AlternateContent` with and without a fallback, hidden runs, breaks, tabs, hyphens, and text
    chosen to collide with every Markdown construct — were converted and then re-parsed with
    `markdown-it-py`'s CommonMark mode, and the plain text it renders was compared against a Python
    reimplementation of the documented walk. All 4,000 agree. That is the check that actually tests the
    escaping: it proves no text was reinterpreted as markup and no markup appeared the source never had.
    It found one real difference — a heading whose break kept the padding on both sides of it, emitting
    two spaces where one renders — and one difference that is the *parser's*: `markdown-it-py` strips
    Unicode whitespace from a block's edges, so it drops a leading U+00A0 that CommonMark keeps and
    mapping row 35 rules we emit. The first was fixed; the second is normalised out of both sides of the
    comparison rather than papered over on one.
  - **The toggle XOR is differential-tested too**, which matters because it is the one algorithm in M5
    that a plausible implementation can get wrong in a way no fixture notices: 3,150 randomly generated
    style tables — random `w:basedOn` chains including cycles, self-references and chains past the
    sixteen-link cap, random `w:docDefaults`, random `w:rStyle` and random direct formatting — were
    resolved by the converter and by a second, independent implementation of ISO/IEC 29500-1 17.7.3
    written from the specification. The two agree on every paragraph of every table, on both things a
    style can observably do at M5: whether a run is hidden, and what heading level a paragraph is.
  - **Fuzzed**: 15,000 mutated archives and 7,500 generated well-formed documents through the exe, on
    top of those 4,000 and 3,150.
    Every mutated archive returns a documented exit code, every generated document satisfies the
    emitter's own invariants — UTF-8 out, LF endings, no trailing whitespace, no blank line inside a
    block, exactly one trailing newline — and neither sanitizer says anything.
  - **Reviewed** by a four-area design-and-critique workflow before the code was finished and by a
    six-dimension adversarial review after it, each finding put to a skeptic told to refute it. Three
    of what it found were defects that produced wrong Markdown and that every fixture and every unit
    check had missed. A **line end inside a `w:t`** reached the output as a line end: `<w:t>Total&#10;#
    5</w:t>` came out as a paragraph followed by an `<h1>`, and a `w:t` a producer pretty-printed came
    out as an indented code block. WordprocessingML spells a break `w:br`; a newline character in a
    `w:t` is interior whitespace, and it now folds to one space. A **GFM delimiter row** could attach
    to the line above it inside one paragraph, so `a|b` with a hard break and then `-|-` rendered as a
    table rather than as two lines of text; the line-start pass now escapes the head of anything
    shaped like a delimiter row, which is enough because a table needs both halves. And a **thematic
    break with interior spaces** -- `--- -`, which CommonMark counts as four hyphens -- was not caught,
    because the rule wanted one contiguous run; that one the differential oracle found on its own.
    What the review changed besides, beyond the heading-space defect above: escaping moved from
    per-span to per-assembled-line, because the ampersand rule looks ahead and Word fragments runs, so per-span escaping made
    `A&amp;B` depend on where the producer split it; `MD_CONTEXT_HTML` became the inline set plus two
    entities rather than instead of it, because GFM still parses the text between raw tags; the setext
    rule learned that an underline needs a line above it, which removed a backslash from every `===`
    line at the head of a block; the walker learned that mapping row 1 rules heading text is never
    additionally bolded; the complex-script twins `w:bCs` and
    `w:iCs` fold into the same formatting bit, because two runs that render identically must coalesce
    at M6; a container refusal now keeps the package's own exit code instead of always reporting 3;
    `w:caps` uppercases its run's text, which mapping row 37 rules and nothing was doing; `w:webHidden`
    hides a run the way `w:vanish` does, which `docs/CONVERSION_REFERENCE.md` 2.3 asks for; `w:dir`,
    `w:bdo` and a `w:ruby`'s `w:rubyBase` are descended into rather than skipped, because their text is
    content; a dead `mc:ProcessContent` *element* branch went, because MCE spells it an attribute and
    no conformant document can carry the element; and the `wrappers` fixture's `w:tbl` and dangling
    `r:id` were made valid WordprocessingML, so M7 and M9 inherit a fixture rather than a typo.
  - **One question the review raised is a ruling and not a fix, so it is D12 rather than a commit**:
    GitHub renders `$...$` as LaTeX math and has since 2022, `docs/CONVERSION_REFERENCE.md` 4.1
    predates that, and "costs $5 and $10" is corrupted on the one renderer this project's own mapping
    table names. Escaping `$` everywhere is visible on every ordinary document; the row is in the
    Decisions table awaiting the owner.
  - **The one real defect the tests found that the goldens could not**: `StyleModel`'s string heap never
    seeded offset 0 with the empty string, so a style declaring no `w:basedOn` read offset 0 as its
    parent and inherited whichever identifier happened to be stored first. Every golden fixture's first
    style is `Normal`, and every other style in them is based on `Normal`, so the bug was invisible to
    all seven; the unit suite, whose styles are ordered to make that untrue, failed seven checks at once.
  - **What a Linux session cannot reach, and a Windows session must**: `/W3` and its zero-warnings
    requirement, `/sdl`, `/arch:AVX2`, the real `include/` headers, and whether `mzero` on the three
    new `al32` structures behaves — the shim traps a misaligned 256-bit store rather than reproducing
    MSVC's fault, which is stricter, but it is not the same code. Two of the four defects M4's review
    fixed were reachable only on that path.
- **M6 `[todo]` Inline formatting** — `RunCoalescer` (merge + hoist), the remaining `MdEscape` callers,
  bold/italic/strike/code/sup/sub. DoD: fragmented-run and trailing-space-in-bold fixtures pass.
  M5 leaves three things waiting here by name: every span already carries its effective formatting, so
  this is an extension rather than a rebuild; `MD_CONTEXT_HTML`, `MD_CONTEXT_CODE_SPAN` and the code
  block have no caller yet; and escaping is per assembled line, which has to become per coalesced span
  once there is markup between the spans.
- **M7 `[todo]` Hyperlinks & images** — rels resolution, `MediaExtractor`, anchors/slugs.
- **M8 `[todo]` Lists** — `NumberingModel` (indirection, overrides, restarts, style-borne numPr).
- **M9 `[todo]` Tables** — grid normalization, gridSpan/vMerge policy, HTML fallback.
- **M10 `[todo]` Fields, notes, tracked changes** — field state machine, footnotes/endnotes, sdt,
  accept-all revisions.
- **M11 `[todo]` Hostile-input hardening** — bombs, traversal, XXE, producer-variance fixtures
  (Google Docs / LibreOffice / Pandoc exports). **D10 lands here**: the milestone owns the question of what a ZIP
  *entry name* carrying `\`, a leading `/`, `..`, a drive letter or an NTFS stream suffix should do — refuse the
  archive, or normalise while building the part index — and the ruling defers it to this point precisely so the
  producer-variance corpus can answer it rather than a guess. Do not close M11 without recording an answer and a
  fixture for it; "we looked and left it alone" is an answer, silence is not. Note what is *not* deferred: a
  relationship **target** of any of those shapes is already refused by `OpcResolveTarget`, and no archive name has
  ever reached disk. DoD: as before, plus a fixture per decided entry-name shape.
- **M12 `[todo]` CI** — GitHub Actions `windows-latest`: msbuild x64 Release (the only platform) +
  fixture build + golden runner. **D11 lands here too**: commit the mechanical GCS validator every session since M1
  has written into a scratch directory and thrown away — r17 prolog regexes, 3-space indent, no tabs, ASCII,
  CRLF, the 150/180 widths — and run it in CI over `src/` and `tests/`, with **`include/` exempt**, because a
  validator pointed at the owner-authored headers would fail `typedefs.h`'s `AVX512` token and two pre-r17 banners
  this file says to report rather than fix. The exemption is the ruled part, not an implementation detail: encode it
  in the validator itself, not only in the CI invocation, so running it by hand cannot produce a different verdict.
  DoD: a red CI run on a deliberately broken prolog, a green one on `main`, and the `.clang-format` no-op check
  alongside it.
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

## Decisions (every row is ruled and settled — do not re-litigate)

D1–D5 were ruled by the owner on 2026-08-18, D6 and D7 on 2026-08-19, and D8–D11 on 2026-08-24, the day M4
raised them: the owner accepted all four session recommendations as written. **D12 is open** — M5 raised it on
2026-08-25 and it awaits the owner. Keep the
IDs stable — `docs/CONVERSION_REFERENCE.md` cites D1, D2, D8 and D10 by name — and keep a ruled row's question and
the reasoning that was put to the owner rather than trimming it to the answer, because a ruling records what was
asked as much as what was decided. New questions get the next free ID (D13, D14, …) with the same
question/recommendation/status shape, and stay `Open — owner call` until the owner rules.

| ID | Question | **Ruling** | Executed? |
|---|---|---|---|
| D1 | ZIP/DEFLATE: vendor miniz vs hand-rolled inflate vs zlib | **Hand-rolled inflate.** First-party `Inflate` + `Crc32` + `ZipReader`; no `third_party/`, no vendored code | M3 |
| D2 | XML: hand-rolled pull parser vs pugixml | **Hand-rolled pull parser.** First-party `XmlPull`; pugixml is off the table | M4 |
| D3 | Win32 configs vs GCS a2 ("32-bit unsupported") | **Drop the Win32 configurations** from `DOCXtoMD.vcxproj`; x64 is the only platform | **done** (owner-verified on Windows 2026-08-19: `/p:Platform=Win32` fails instead of building) |
| D4 | Adopt a3: `/arch:AVX2` + `__AVX2__` guard on x64 | **Adopt**, with the guard as `#ifndef __AVX2__` + `#error` (not `static_assert`) | **done** (M1: flag on both x64 configs, guard in `DOCXtoMD.cpp`, moved to `src/BuildGuards.h` at M2; owner-verified on Windows 2026-08-19 — both configs build clean at `/W3`, and clearing the flag stops the build on the `#error`) |
| D5 | Does a2's tech cut-off (no 32-bit, no SSE-only, no single-threaded) bind this tool? | **Baseline is SIMD, single-threaded**: AVX2 floor with no sub-baseline fallback; single-threading is an owner-granted exception to a2 *(as ruled 2026-08-18; D6 later narrowed the threading half — the text here is left as the owner wrote it)* | standing |
| D6 | `include/spinlocks.h` was added "for future multithread code" — does it reopen D5 for DOCXtoMD? | **Yes, for multi-file processing only: one thread per file, `spinlocks.h` included.** *(Derived, not stated: a single document's conversion therefore stays sequential, and `$LoopMT*`//Qpar stay banned as compiler-directed threading — see the threading baseline.)* | M13 |
| D7 | What batch surface does D6 need? (a) literal thread-per-file or a bounded pool? (b) how are multiple inputs passed? (c) how do per-file failures aggregate? (d) what do `--stdout` and `-o` mean for N files? | **(a) A bounded pool** sized to a user-specified thread count, defaulting to the system's virtual (logical) core count. **(b)** Inputs are repeated command-line operands: `DOCXtoMD [options] <input.docx> [input2.docx […]]`; output filenames are derived automatically. **(c)** Failed conversions are printed to the console before the process terminates, and partial success gets its own exit code. **(d)** `--stdout` is single-file only; `-o` gives the output path — the filename for one input, the directory for many | M13 |
| D8 | Ill-formed UTF-8 inside a part: refuse the input, or substitute U+FFFD and carry on? CLAUDE.md's M4 definition of done says "rejected with a clear message"; `docs/CONVERSION_REFERENCE.md` 5.12 says "replace invalid sequences with U+FFFD rather than aborting". Sub-question: should the answer differ between a structural part (`[Content_Types].xml`, any `.rels`, the main part) and an optional one (`styles.xml`, `settings.xml`, an unreferenced footnote part)? | **Refuse**, as M4 implements, adopting the session recommendation in full. It is testable today as an exit code plus a substring, while U+FFFD substitution is only checkable against a golden `.md` that does not exist until M5; and refuse → replace is a strict relaxation still open later, while replace → refuse would break output users already had. The sub-question goes the same way: a part is a part, structural or optional. *(Consequence: `docs/CONVERSION_REFERENCE.md` 5.12 said the opposite and was corrected to match, which is what the ruling was for. U+FFFD survives only on the console path in `Utf`, where an unrepresentable path should still be reportable.)* | M4 (already implemented; `bad-utf8.docx` and `truncated-utf8.docx` pin it) |
| D9 | When the `officeDocument` relationship resolves to a part whose content type is **not** one of the four WordprocessingML main-document types, does the tool convert it (trusting the relationship and reporting the disagreement) or refuse it as not a valid DOCX? "Cross-check" in correctness rule 1 is ambiguous between *verify and fail* and *fall back*, and the two readings give opposite exit codes for the same file. | **Trust the relationship and convert**, as M4 implements: the relationship is the specification's discovery mechanism and `[Content_Types].xml` is metadata, and refusing loses documents from producers that omit the Override. The content-type table stays a cross-check in the one case M4 already gives it — a relationship that resolved to a part the archive does not contain. | M4 (already implemented; `content-type-mismatch.docx` pins it, so the choice cannot change silently) |
| D10 | ZIP **entry** names — not relationship targets — carrying `\`, a leading `/`, `..` or a drive letter. PowerShell's `Compress-Archive` writes `word\document.xml`; `docs/CONVERSION_REFERENCE.md` 5.12 names entry names as a traversal surface, and CLAUDE.md forbids *producing* such fixtures while saying nothing about *consuming* them. Refuse the archive, or normalise while building the part index? | **Leave it as it is until M11** and decide there with the producer-variance corpus in hand. Nothing is exposed meanwhile: part names are only ever compared in memory and no path reaches disk until M7's `MediaExtractor`, which generates its own names. Normalising is defensible; it is a leniency with no measured constituency, and strictness is the reversible direction. | **Deferred to M11 by the ruling** — that milestone owns the decision and must not close without recording it |
| D11 | Should the repository carry a committed mechanical GCS validator (r17 prolog regexes, indent, tabs, ASCII, CRLF, width), and would it run over the owner-authored `include/` headers? | **Yes, at M12 with CI, and `include/` exempt.** Every session since M1 has written one in a scratch directory and thrown it away. The exemption is a policy rather than a detail: a validator run over `include/` would fail `typedefs.h`'s `AVX512` token and two pre-r17 banners that this document says to *report, not fix*. Landing it earlier would oblige every future file to pass a session-authored checker with no CI behind it. | M12 |
| D12 | GitHub renders `$...$` and `$$...$$` as LaTeX math, and has since 2022. `docs/CONVERSION_REFERENCE.md` 4.1 predates that and does not list `$` among the characters to escape, so today a paragraph reading `costs $5 and $10` is emitted verbatim and github.com renders `5 and ` in math font, losing both dollar signs. Should `$` join the unconditional inline escape set, join it conditionally (only where a closing `$` could pair with it), or stay unescaped? Note the cost of each: unconditional puts a backslash in front of every price in every document, conditional needs a lookahead the line-assembly pass can do but the reference does not describe, and leaving it corrupts a real and common shape on the one renderer this converter names in its own mapping table. The same question reaches `docs/CONVERSION_REFERENCE.md`, which would gain the row either way. | *Open -- owner call.* **Recommendation: escape `$` conditionally**, where a later `$` on the same line could close it, and add the row to the reference. Unconditional escaping is the safe direction but it is visible on every ordinary document, and math is not a CommonMark feature -- it is one renderer's extension, so paying for it everywhere is out of proportion. | Not started -- awaiting the ruling |

Consequences already folded into this file: the "no third-party code" line in Do NOT and the removal
of `third_party/` from the architecture (D1/D2); the first-party `Inflate`/`Crc32` modules and the
CRC-32-vs-CRC-32C trap in rule 11 (D1); the first-party `XmlPull` and its baked-in namespace table
(D2); the x64-only Build & run section and the two-configuration
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

D8–D11's consequences, folded in on the day they were ruled: `docs/CONVERSION_REFERENCE.md` 5.12 now
refuses ill-formed UTF-8 instead of substituting U+FFFD, and the Known-gaps entry that recorded the two documents
disagreeing is replaced by the ruling (D8); the `officeDocument` relationship decides even when
`[Content_Types].xml` disagrees, which is what M4 already does (D9); M11 inherits the ZIP-entry-name question and
may not close without recording an answer (D10); and M12 gains the committed mechanical validator, with `include/`
exempt (D11). Two of the four are pinned by a fixture rather than by prose — `bad-utf8.docx` and
`content-type-mismatch.docx` — so a session that quietly reverses one fails a test rather than merely
contradicting this file. Note what the owner ruled and what a session then derived: the rulings are the four
recommendations as the table stated them; **which milestone owns D10 and D11's work, and the wording of the
roadmap and reference edits, is session-derived** and may be revised without re-litigating the rulings.

## Repo conventions

- Commit messages: imperative summary line; never name AI models in commit messages, code comments,
  or PR text (standard Claude Code attribution trailers are fine).
- `CHANGELOG.md` (from M1): Keep-a-Changelog style per c3; prologs stay history-free (c1).
- License field in every prolog: `License: MIT  Copyright: David William Bull` (two spaces).
- `CONTRIBUTING.MD` and `GDC_GCS_v1_1_4.md` are owner-managed — do not edit them. The six shared
  headers in `include/` are owner-authored library files — do not reformat or re-version them. Raise
  conflicts as numbered decisions instead (like D1–D12 above).
