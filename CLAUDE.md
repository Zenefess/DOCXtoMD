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
  layer (M4), the converter (M5/M6), M7's reference resolution, M8's lists, M9's tables and M10's fields,
  notes and tracked changes — neither of the last two needed a module of its own: thirty-eight files, all
  CRLF, tab-free, ASCII-only, none over 150 columns, each carrying a validated r17 prolog at `v0.1.0`
  with `ISA: Scalar`. Unlike
  `include/`, `src/` is **not** exempt from the repository style, and all thirty-eight are committed in
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
    enum, so the stable API lives in code rather than only in this file) and six writers:
    `DiagWriteOut`, `DiagWriteOutBytes`, `DiagWriteErr`, `DiagError`, `DiagErrorText` and
    `DiagNoteText`. `DiagWriteOutBytes` is the only one that returns anything, because it is the only
    one whose failure loses a document rather than a message. Notes go to
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
    `OpcFindPart` goes through an **open-addressed part-name index built once at `OpcOpen`**, folded the
    way `OpcNameEqual` compares so that OPC's case-insensitive part names land in one slot. That is a
    scale matter and it was found at M7. `MediaPlan` looks one part up per picture and nothing caps how
    many pictures a document draws, so that path was quadratic in the archive; `OpcOpen`'s own
    content-type and relationship passes look one up per candidate too, but M4 had already capped those
    at `OPC_MAX_MAIN_CANDIDATES` for this exact reason, so the index makes them cheaper without making
    them asymptotically better. A 1.1 MB document drawing 100,000
    pictures out of a 9,000-entry package took 5.10 seconds and now takes 0.38, with the part count no
    longer registering at all. A first-wins probe keeps the index answering exactly as the scan did, for
    the same reason `ZipReader` resolves a duplicate name to its first record, and a failed allocation
    falls back to the scan rather than failing the package.
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
    `Source_20_Text` and `Source Text` into one key. Since M6 a role also depends on **what kind of
    style declares the name**, and that is load-bearing rather than tidy: "Source Text" is LibreOffice's
    *character* style for inline code (mapping row 11) and an ordinary paragraph style name otherwise,
    and `tests/fixtures/headings` has carried a paragraph style called exactly that since M5. The
    monospace verdict is a tri-state read off `w:rFonts/@ascii` at parse time, so the model stores no
    font names; a `w:rFonts` naming only `w:asciiTheme` specifies nothing rather than specifying false,
    because a specified false would cancel a monospace family an outer layer had established. It layers
    nearest-wins like `w:dstrike` with **one guard**, and the guard is about the document's font
    *baseline* rather than about any one declaration of it. `StyleReadBaseline` folds that baseline once
    at load, nearest-wins over two places: the `w:default="1"` paragraph style, down its own `w:basedOn`
    chain, and `w:docDefaults` behind it. Both halves are needed and the first is the likelier — Word
    normally puts a *theme* slot in `w:docDefaults`, which specifies no family at all, and carries the
    real one on `Normal`. Where that baseline is monospace the heuristic is switched *off* for the whole
    document instead of on: set Courier that way — which legal filings do — and without the guard every
    run is code, every paragraph satisfies row 12, and the file converts to a single fence with every
    delimiter dead inside it. Nearest-wins runs the other way too, and that is the same rule rather than
    an exception: a *proportional* default paragraph style over a monospace `w:docDefaults` puts the
    heuristic back on, because an unstyled paragraph is proportional there and a monospace run really
    does stand out. When the guard is on, only a run's own `w:rFonts` and its **character** style may
    still say monospace — each is a statement about one run rather than about the document, which is the
    whole of what the heuristic is for, so a code character style is unaffected. That last exemption is
    why a `w:basedOn` across two style *types* is dropped at load (ISO/IEC 29500-1 17.7.4.3 requires it
    anyway): a character style based on a monospace default *paragraph* style inherits its family, and
    the character layer is read before the guard, so the link is a hole straight through it. The test
    needs both styles to have **said** what they are, because `w:type` is optional and an absent one
    reads as paragraph — comparing the stored types alone would drop a typeless style's link to a real
    character style and silently lose the code span it carries.
    That match goes through an **open-addressed index built once at load**, and the index is a
    correctness matter more than a speed one: the walker looks
    up a style per styled paragraph behind a one-entry cache, so a linear scan makes a spec-legal 45 KB
    `.docx` — many long-prefixed identifiers, paragraphs alternating between two of them — spin for
    seconds with no output and no refusal. An identifier is stored capped at `STYLE_MAX_NAME_BYTES`,
    which is what the walker's lookup key holds, so a value longer than that cannot be stored in a form
    the document could never match. A missing styles part is legal and yields an empty
    model; a **present** one that is malformed is a refusal, on the same reasoning as D8. Heap offset 0
    is seeded with the empty string, without which a style declaring no `w:basedOn` inherits whichever
    identifier happened to be stored first — a real defect the golden fixtures missed and the unit
    suite caught, because every fixture's first style is the one they would wrongly have inherited.
    Since M8 a style also carries a `w:numPr`, which is how Word's Multilevel List reaches a document:
    `numId` and `numLevel` layer nearest-wins over the run of the `w:basedOn` chain like `w:dstrike`
    rather than by parity, because numbering is not a toggle, and `-1` means unspecified while **0 is a
    real value** meaning "no numbering". `StyleResolveParagraph` takes the paragraph's own pair and
    settles them **before** its six early returns, so a paragraph whose style resolves to nothing else
    still keeps its list membership. `w:docDefaults` is deliberately **not** read for it, and that is a
    guard rather than an omission: a document default `w:numPr` would make every paragraph in the
    document an item, which is the shape of M6's monospace catastrophe. No producer writes one; the
    header's To Do says what a session that wants it must build first.
  - `NumberingModel.h`/`NumberingModel.cpp` — `numbering.xml` as resolved per-`numId` levels, and the
    counter pass that turns them into markers. The indirection is the milestone: a `w:numPr` names a
    `w:numId`, a `w:num` of that id names a `w:abstractNumId`, a `w:abstractNum` of *that* id carries the
    levels, and a `w:numStyleLink` on the abstract definition delegates to a style whose own `w:numPr`
    names another `w:numId` — Word's list-style indirection, chased through `StyleNumberingOf` and
    bounded at sixteen links exactly as `StyleModel` bounds a `w:basedOn` chain, because the
    specification sets no limit and malformed files carry loops. A `w:lvlOverride` replaces a level of
    one instance outright, and a `w:startOverride` folds into that level's **start** rather than being
    kept as a one-shot seed, because ISO/IEC 29500's own gloss on `w:start` is that the value is taken
    when a level first starts *and whenever it is restarted*; *when* it fires is the separate question,
    and it is a bit per level on the instance, applied the first time that `numId` is used. Counters are
    keyed on the resolved **abstract** definition and never on the `numId`, which 2.9 states in as many
    words — two `numId`s over one abstract definition continue one sequence, which is how Word spells
    "continue previous list", while a `startOverride` on a new `numId` over the same definition is how it
    spells "restart at 1". Both halves are needed and they are the two commands Word's list UI offers.
    `w:numFmt` collapses to three values, because GFM can spell a bullet and a decimal and nothing else:
    `bullet` (and a level whose marker is a `w:lvlPicBulletId` picture) is a bullet, `none` is a
    marker-less continuation paragraph, and **everything else — an unrecognised token and an absent
    `w:numFmt` alike — is ordered**, because every one of ST_NumberFormat's sixty-odd tokens but those
    two counts, and degrading an unknown one to a bullet would throw away ordering the counter already
    has while degrading it to a decimal loses only a glyph the renderer discards anyway. Every way the
    graph can break degrades rather than refuses (5.4): a `numId` no `w:num` declares is not numbered at
    all, and a missing abstract definition or a delegation loop is a bullet at every level, because the
    document has said the paragraph is an item and only the format is unknown. A **present** part that
    is malformed is still a refusal, on `StyleLoad`'s reasoning — 5.4's latitude is about a broken
    *reference*, not about bytes that are not well-formed XML. A refusal leaves **no definitions
    behind it**, which the header promises and which is a safety rule rather than tidiness: an instance
    read before the part broke has no counter key, and a refused load never sizes the counter table
    those keys index. A load clears the model on the way in as well as on the way out, which is the
    same rule from the other side and is what keeps a second load from stranding the first one's index.
    `NumFind` goes through an
    open-addressed index built once at load, for the reason M5 found in `StyleFind` and M7 found twice
    more in `OpcPackage`. `NumAssignMarkers` is the pass, and it is a pass rather than part of the walk
    because of `IrRewind`: `DocWalker` walks the first `mc:Choice` of an `mc:AlternateContent`
    speculatively and rewinds it when an `mc:Fallback` follows, an `IR_MARK` carries no walker state,
    and a table of nine counters per definition cannot be unwound the way a span arena can — a discarded
    Choice would consume a number the document never showed, which is M6's monospace-vote defect one size
    larger. It also puts the numbers where `docs/CONVERSION_REFERENCE.md` 6.2 puts them, in stage [9].
    Incrementing a level clears every deeper one subject to that level's own `w:lvlRestart` — `0` never,
    `N` only under a level shallower than `N`, absent under any shallower level — and clearing sets a
    counter back to *unstarted* rather than to a value, so the start that seeds it next is chosen by
    whichever `numId` is in force there, which is what makes an abstract-keyed counter and a num-keyed
    start agree. A counter saturates at `NUM_MAX_NUMBER`, nine digits, which is where CommonMark stops
    reading an ordered marker: a tenth digit is not a list at all, so a hostile `w:start` costs a wrong
    number rather than a lost list.
  - `Ir.h`/`Ir.cpp` — the intermediate representation the walker builds, RunCoalescer rewrites and the
    emitter reads: blocks and spans as arrays of POD records over growable byte arenas -- one for span
    text, since M7 a second for destinations and anchor names, and since M9 a third for column
    alignments -- each addressed by offset so a growth invalidates nothing. The split between the first
    two is load-bearing rather than tidy: every text span of a block lies end to end in the text arena,
    which is the invariant `RunCoalescer` merges on, and a destination written between two runs would
    put a gap in the middle of it. Five block kinds since M6 — paragraph, heading, quote, code
    and rule — and **M8 added no sixth**: a block carries the `numId` the walk read, its level, the
    number the counter pass settled and a flag byte instead, because being an item of a list is a second
    fact a document may state about a paragraph whose kind is already something else. That is not a
    hypothetical — `tests/fixtures/liststyles` carries a blockquote that is an item and an inline-code
    paragraph that is an item, and a sixth kind would have had to choose between the two facts.
    **M9 added the sixth**, `IR_BLOCK_TABLE`, and it is a kind rather than a flag for the opposite
    reason: a table is not a second fact about a paragraph, it is not a paragraph at all. It carries no
    spans -- its content is the blocks of its cells -- and beside it sit three record arrays, `IR_TABLE`,
    `IR_ROW` and `IR_CELL`, plus the third byte arena, which holds one `IR_ALIGN` per column of the tables
    that have any. **A cell's blocks are ordinary blocks in the one flat array, in document order**,
    which is what leaves `RunCoalesce`, `LinkResolveRefs`, `LinkResolveAnchors`, `MediaPlan` and
    `NumAssignMarkers` untouched: each reads one array in the order the document is read, and a
    paragraph in a cell is a paragraph. A list inside a cell therefore continues a list outside it,
    which is what Word draws.
    **M10 added a seventh span kind and no block kind.** `IR_SPAN_NOTE` is a footnote or endnote
    reference, a marker like a link's brackets: it carries the `w:id` as written, with `IR_SPAN_FLAG_END`
    for an endnote, until `LinkResolveNotes` rewrites it into the label, and `IrHasContent` counts it as
    content unless it is muted -- a paragraph holding nothing but a reference holds something a reader
    sees. A note itself is an `IR_NOTE` record -- its story, its `w:id`, the part it was read from and the
    label it was given -- and **a note's blocks are ordinary blocks in the one flat array**, after every
    block of the body, each stamped with the note's index in `IR_BLOCK.note`. That is the table's bargain
    struck again for the same reason: every pass between the walk and the emitter reads one array, and
    only two readers group a note's blocks -- `LinkResolveNotes`, which numbers the body's references
    first and then each note's in the order the notes were numbered, and the emitter, which writes a
    note's blocks somewhere other than where the walk put them. The part is on the record because
    a note's relationship ids are scoped to its own part, and `LinkResolveRefs` resolves each block
    against the part it came from.
    A table's rows and a row's cells are **chains** and not ranges, and those are the two places this
    module gives up a contiguous array. A cell's content is walked where it stands, so a table inside the
    first cell of a row appends its own rows and cells before the outer row's next cell and the outer
    table's next row are appended -- and no order of appending fixes it, because a second nested table in
    a second cell interleaves again. A chain costs one field per record, and every walk of one goes
    forward: the pipe form walks a table's rows once, and the raw-HTML form also walks forward from a
    `w:vMerge` restart through the rows below it, which is `MdRowSpanOf` and is bounded as the
    `MdEmitter` bullet says.
    The tails of those chains live on the **walker** and not here, which is what makes a rewind cost two
    integers: an `mc:AlternateContent` may wrap a `w:tr` or a `w:tc`, so a discarded `mc:Choice` can build
    rows that `IrRewind` then throws away, and the next row has to link behind the row that really
    precedes it. Everything a table says about its own shape -- its column count, whether any cell
    merges, whether any cell holds another table, and what the first row said about each column's
    alignment -- is **derived in `IrEndTable`** by chasing the chains it has just terminated, and none
    of it is accumulated as the walk goes. That is a correctness rule rather than tidiness, and
    `IR_TABLE_NESTED` is why it has to cover every flag: marked on the parent by each nested table as
    it closed, it survived a rewind that removed the table itself -- `IrRewind` restores eight counters
    and no flags -- so a table whose only nested table an `mc:Fallback` discarded was emitted as raw
    HTML it did not need. `IrEndRow` and `IrEndTable` write the terminator from the caller's own tail rather than
    from whatever was appended last, so a discarded row is simply never named.
    `IrDropEmptyBlocks` **never looks inside a table**: it moves the whole of one as a unit and shifts
    every record of it -- and of every table nested inside it -- by the single delta that applies where
    the table stands, which is exact because the only blocks that go are outside every table. What a cell
    then has to cope with is a block that emits nothing, which is an empty cell: a shape a table has
    anyway and one the emitter already writes. Four kinds of block are exempt from the emptiness test: a
    rule is an empty paragraph by construction, a table has no spans of its own because its content is
    the blocks of its cells, an empty code paragraph is a blank line inside a fence, and an **empty list
    item** is a marker on a line of its own — Word writes them, the counter has already counted one, and
    unwinding the block would leave a hole in the numbers. `IrHasContent` is the public twin of the test
    `IrEndBlock` applies to itself, because the emitter asks the same question when it trims a list's two
    edges, and `IrSetListRef` is what the walker records a reference through. `IrEndBlock` trims a
    block's leading and trailing break spans — except inside a fence, where a break *is* a newline and
    no marker is written for it, so the reason to trim one never arises and trimming loses a line — and
    then unwinds the whole block — records, spans and arena — when nothing but ASCII whitespace is left,
    which is
    what collapses runs of empty paragraphs at no cost. `IrRewind` has four callers, all in `DocWalker`,
    and `IrMark` the first three of them: `mc:AlternateContent`, where the first `mc:Choice` is walked
    speculatively and rewound if an `mc:Fallback` turns out to follow it; since M7, the picture walk,
    which opens an image span before it knows whether the container holds a reference and rewinds it
    when none turns up; since M9, the table walk, which opens a table's block before its first row and
    rewinds the whole table when no row survives -- and since M10 when it stood wholly inside a TOC; and
    since M10, the paragraph walk, which rewinds to the mark `IrBeginBlock` returned when a paragraph
    that began inside a field nobody sees came to nothing, list marker and all, which `IrEndBlock` alone
    would have kept. A non-breaking space counts as content, per mapping
    row 35.
  - `DocWalker.h`/`DocWalker.cpp` — the body walk and, since M10, the notes walk, one dispatcher for both
    block and run level because
    every transparent wrapper appears at both and means the same thing at each. Accept-all revisions
    (correctness rule 8): `w:ins` and `w:moveTo` are transparent, `w:del` and `w:moveFrom` are dropped
    whole, and `w:sdt`, `w:smartTag` and `w:customXml` are transparent. A run whose effective
    `w:vanish` or `w:webHidden` is on is dropped with everything it holds but its field structure, which
    is read even there, as the fields paragraph below says; a run whose `w:caps` is on has its text
    uppercased, which is mapping row 37 and is a transform on the bytes rather than a delimiter, so it
    belongs here. A
    heading's spans have their bold bit cleared, because mapping row 1 rules that heading text is never
    additionally bolded and `IR_FMT` is the only channel the emitter has; a code run has its bold and
    italic bits cleared for a different reason, which is that row 11 drops them and two runs that come
    out as the same code span have to coalesce. The walker also classifies the paragraph: a quote or a
    code style gives its own block kind, so does a paragraph whose every text-bearing run is monospace
    (row 12's second detection, settled here because the font is a run property the IR does not carry).
    That vote is taken once a whole run is read and only by a run that produced a character a reader can
    see: Word splits a logical run at every rsid boundary and the gap *between* two monospace runs
    routinely lands in the body font, so counting it would break the fence on exactly the fragmentation
    correctness rule 4 exists to absorb — and a space renders identically in every face, so ignoring it
    loses nothing. What abstains is the tab, the Zs category and the two things the walker removes on the
    way out, a CR or LF folding to one space and a soft hyphen dropped outright — with **one** exclusion,
    U+00A0, which mapping row 35 makes content and which therefore settles a paragraph like any visible
    character. That is the third of the three questions this project asks about whitespace and the only
    one where U+00A0 goes the other way: `RunCoalescer` hoists it, `IrEndBlock` counts it as content, and
    here it counts as content too. A lone `w:pBdr` bottom or between border on a paragraph that came to
    nothing gives the horizontal rule of row 25; both halves of `CT_PBdr` are matched **by name** from
    two tables rather than one half by exclusion, so a vendor extension or an `mc:AlternateContent`
    inside a `w:pBdr` is ignored instead of counting as a fourth border and suppressing the rule.
    A heading beats all of them. `w:t` text is taken literally —
    `xml:space` is the producer's business — with U+00AD removed.
    Since M7 the walk also reads references. A `w:hyperlink` becomes a link span pair around its content,
    carrying the reference **as written** — an `r:id`, a `#` and a `w:anchor`, or both joined by the `#`
    that will separate them in the output — because ids are scoped per part and the lookup belongs where
    the part is known. A `w:bookmarkStart` becomes an anchor span where it stood, or is held for the next
    block when it stood between two. A `w:drawing`, a `w:pict`, a `w:object` and an `mc:AlternateContent`
    standing in for one all become a single image span through **one scan**: a picture is identified by
    the markers inside it rather than by the element it arrived in, so both markup families are looked for
    at once and the first alt text and the first relationship win — which is what emits a picture
    described in two vocabularies exactly once, closing 5.8's double-emit by arithmetic rather than by
    understanding the branches. A container holding no picture reference comes to nothing. **An `a:blip`
    counts only as the direct child of a `pic:blipFill`**: the same element under an `a:blipFill` is the
    bitmap a drawn shape, a chart wall or a table cell is *painted with*, and taking it emitted a shape's
    wallpaper as the figure the paragraph shows — which also contradicted the "comes to nothing" rule,
    since a drawn shape is exactly what that names.
    Since M8 the walk also reads `w:numPr`, and what it records is the **reference** — the `w:numId` and
    the `w:ilvl` — rather than a number, which `NumAssignMarkers` settles afterwards. It does take the
    numbering model, for **one bit only**: whether a `w:numId` names a list this document can resolve.
    That is not fastidiousness. A reference that resolves to nothing is not an item of anything, so it
    must cancel neither row 25's horizontal rule nor row 12's font detection — decided on the raw
    reference, a broken numbering graph deleted a `---` from the document outright and demoted a fence
    to an inline code span, which is a defect in a *reference* losing output that has nothing to do with
    it. Precedence is settled here in `DocListSurvives`: a **heading cancels a list outright**, which
    5.4 rules in as many words and which is the common case rather than an edge one, because Word's
    Multilevel List linked to headings puts a `w:numPr` on every `Heading N` style and without the rule
    every heading in such a document becomes an item and the structure inverts; and a `w:numId` of 0 is a
    specification of "no numbering" that cancels whatever the style chain supplied, which is why the test
    is `> 0` and not `>= 0`. A quote style and a code style are *not* cancelled — a paragraph may be both
    — but **row 12's font heuristic is**, on `StyleReadBaseline`'s own reasoning one milestone on: the
    font is a guess at what a paragraph is and a `w:numPr` is a statement, so a list of code lines set in
    Consolas stays a list instead of becoming a run of fences that have each lost their marker. A
    paragraph carrying a live `w:numPr` also gets its block even when it held nothing, because a marker
    on a line of its own is content — which is the same reason it is not the row 25 horizontal rule when
    it carries a lone bottom border as well: Word draws the marker and the border both, and emitting
    `---` there would delete the item and invent a rule the document never had.
    Since M9 the walk also reads `w:tbl`, and it is the one element whose children are not runs or
    blocks but a shape of their own: a `w:tblGrid` is counted for its columns, a `w:tr` becomes a row
    and a `w:tc` a cell, and a cell's content is then ordinary block content walked where it stands.
    Two levels were added to the dispatcher rather than two loops, because every transparent wrapper
    appears around a row and a cell as it does around a paragraph -- a `w:sdt`, a `w:customXml`, a
    `w:ins` and an `mc:AlternateContent` all mean at those levels exactly what they mean at the others.
    A row a tracked change **deleted** is dropped whole with its content, which is correctness rule 8
    read the only way that keeps a row a reader still sees out of the output. Alignment is the first
    `w:jc` among a cell's paragraphs that names left, centre or right -- a `both`, a `distribute` or an
    unknown value does not settle it, so a later paragraph still can -- kept for the first row only,
    because a GFM delimiter row is the only place an alignment can be written and it stands under the
    header. A table nested past `IR_MAX_TABLE_DEPTH` is skipped whole, which is the bound that keeps
    this walk's *stack* off the document's content -- the tokenizer's own element cap would stop a
    runaway eventually, but only after a great many frames. A table that turns out to have no rows is
    unwound entirely, so an empty `w:tbl` costs no block and no blank line.
    Since M10 the walk runs **fields** through correctness rule 7's begin/separate/end state machine, whose
    stack lives on the walk rather than on a paragraph because a result may span several. Everything
    between a begin and its separate is instruction and never content. A `HYPERLINK`'s result -- and a
    `REF`'s that carries `\h`, which is its author asking for a link -- becomes a link span pair whose
    destination is built from the instruction: the first argument, a `\l` location joined on by `#`, or
    a `REF`'s bookmark behind one. A `TOC` vanishes result and all, and so does a `w:sdt` whose
    `w:docPartGallery` says it is one. Every other field -- `PAGE`, `SEQ`, `DATE`, `PAGEREF`, a `REF`
    without `\h`, `INCLUDEPICTURE` -- is the cached result it was showing, and a field with no separate
    shows nothing. A `w:fldSimple` is the same machine in one element, and a field left open inside one
    is closed with it. The instruction is read the way Word writes one -- split over any number of
    `w:instrText`, its keyword in either case, a switch glued to its argument, `\"` and `\\` escaped inside
    quotes -- and every switch that takes an argument consumes it wherever it stands, so an argument
    written in front of the target is never taken for it. A link a field's result opens is closed at the
    end of each block and opened again at the start of the next, because Markdown cannot spell one
    across two; links do not nest, so a field inside a `w:hyperlink`, or inside another field's link,
    keeps its text and the outer link wins. Eight fields deep are tracked, past that a field is counted
    rather than tracked and what it holds is dropped rather than guessed at -- every end still closes
    something -- and an instruction that outgrows its 2,048 bytes is never linked, because a truncated URL
    is worse than none. **A field's structure is read even in a hidden run**: Word sets `w:webHidden` on
    every run of the `PAGEREF` in each TOC entry, `w:fldChar` included, and dropping a hidden begin whose
    end was read would misread every field after it. A paragraph that **began inside a field nobody sees**
    and came to nothing is unwound whole -- which is every paragraph of a TOC after the one it begins in,
    list marker, blank code line and border included -- a table standing wholly inside one goes the same
    way, and a bookmark inside one marks a place the output does not have and is dropped. The field
    stack, the open link and the pending join below are all put back when an `mc:Choice` is rewound; at
    the end of each story the field stack and the open link are forgotten, and a paragraph the join left
    waiting ends as written.
    Since M10 the walk also takes the last two of correctness rule 8's revisions that are not wrappers. A
    paragraph whose **mark** a tracked change deleted -- a `w:del` or a `w:moveFrom` in the `w:rPr` of its
    `w:pPr` -- runs on into the next paragraph (5.11): its block stays open, the next `w:p` adopts it, and
    the paragraph that survives gives the pair its classification, because the mark is where a
    paragraph's style lives. The adopted block keeps the moment the first of the two began, so one that
    began inside a TOC is still a TOC entry. Where no paragraph turns up to take it -- a table, the end of
    a cell, a note or the body -- it ends as written, which is a producer's malformation, since Word will
    not delete those marks. A cell a `w:cellDel` removed is dropped with its content, as a deleted row
    already was.
    And since M10 the walk reads **notes**. A `w:footnoteReference` or `w:endnoteReference` becomes a note
    reference span carrying the `w:id`; the `w:footnoteRef` a note's own body opens with is skipped,
    because the `[^n]:` label replaces it. `DocWalkNotes` then reads a notes part after the body, and
    reads **only the notes something already walked references** -- a notes part holds Word's
    separators and every note whose reference a user deleted, and GitHub drops a definition nothing
    references, so reading one would put its pictures on disk and its items in the list counters for
    text nobody sees; a part nothing references a note of is never even validated. The footnotes are
    read before the endnotes, so an endnote cited from a footnote is found; a note cited only from
    another note of its own story, or a footnote cited only from an endnote, is not, because its
    reference is not seen until the note holding it is.
    A note's `w:type` decides whether it is one -- a separator, its continuation and the continuation
    notice are machinery whatever their `w:id` -- and a second note of one `w:id` is not read, which is
    the first-wins rule every duplicate in this project goes by. A note is a story of its own: a field it
    left open, a paragraph join it left waiting and a bookmark after its last paragraph all end with it.
    What is skipped whole and why:
    `w:sym` and `m:oMath` (neither has a milestone, and they are two of the places text is lost rather
    than merely unformatted, beside a text box's `w:txbxContent` and a text-bearing `mc:AlternateContent`
    inside a run, which the picture scan drops -- all four are named in the To Do lists of `DocWalker.h`
    and `DocWalker.cpp`), the comment references and ranges (dropped by mapping row 30), and anything
    this build has
    never heard of, which is the OOXML compatibility model. Descended into although their own meaning is
    layout this mapping has no spelling for: the bidirectional containers `w:dir` and `w:bdo`, and a
    `w:ruby`'s `w:rubyBase`. `w:hyperlink` was on that list until M7 and `w:fldSimple` until M10; each has a
    handler of its own now, and the paragraphs above say what they do. `mc:Ignorable` and `mc:ProcessContent` are **attributes**, not elements,
    and nothing reads either yet — an element in an ignorable namespace is skipped rather than having
    its children promoted, which is a `To Do` and not a claim of MCE conformance.
  - `MdEscape.h`/`MdEscape.cpp` — correctness rule 6's context-aware writer, pure and allocating
    nothing: one core that measures when its destination is null and writes when it is not, so the two
    can never disagree about a length. M9 added a **`pipes` argument** beside D12's `dollars` and for
    the same reason: being inside a table cell is not a *place* text is written but a fact that composes
    with every place there is, because a cell holds code spans, link text, alt text and raw-HTML
    fallbacks exactly as a paragraph does -- a context per combination would have been five more of
    them. GFM splits a row into cells *before* it parses any inline content, so a literal pipe ends the
    cell wherever it stands, including inside a code span, where `\|` is the one escape GFM honours and
    the whole reason a code span in a cell is expressible at all. `MD_CONTEXT_TABLE_CELL` keeps its own
    unconditional rule so that a caller writing a cell's ordinary text is safe with or without the
    argument. M9 also added `MD_CONTEXT_HTML_BLOCK`, which is the one context where nothing Markdown
    says is true: a CommonMark HTML block runs to the next blank line and every byte of it is passed
    through unparsed, so a backslash there is a backslash a reader sees and the only escapes are the
    four entities. It is not `MD_CONTEXT_HTML` with more of them -- that one is for an element *inside*
    a paragraph, where GFM still parses the text between the tags. The reference lists a `lineStart` context; this has none, and
    the reason is the reference's own pitfall 6 — "digits then a dot then a space" is a property of an
    assembled line, not of a run — so line starts and a heading's closing hash sequence are post-passes
    over a finished line instead. `MD_CONTEXT_HTML` is the inline set **plus** unconditional `&amp;`
    and `&lt;`, not instead of it: GFM passes a raw tag through but still parses the text between the
    tags. A setext underline needs a line above it, so the `=` rule takes a `continuation` flag while a
    thematic break does not. The dollar rule is D12's, and it is a count rather than a grammar: a line
    holding two or more `$` has every one of them escaped and a line holding one keeps it bare, because
    a math span needs two delimiters and a price is the common case. Two things it rests on — the
    backslash must stay unconditionally escaped, or a source `\` before a `$` would swallow the one
    this rule inserts, and the count must be taken over a whole assembled line. Three of the four
    contexts M5 wrote without a caller got one at M7, and re-cutting them against real hyperlinks changed
    none of them: link text and alt text are the inline set, because what 4.1 asks of them beyond it is
    that a closing bracket may not appear unescaped and the inline set escapes both brackets already.
    `MD_CONTEXT_TABLE_CELL` got its caller at M9, and re-cutting it changed nothing in it either — what
    changed instead is that the pipe became an argument, for the reason the paragraph above gives.
  - `RunCoalescer.h`/`RunCoalescer.cpp` — the coalescing pass, and the reason a delimiter is safe. It
    merges adjacent text spans carrying equal formatting (correctness rule 4 / reference 5.1) and then
    hoists leading and trailing whitespace out of every formatted one (5.3), **in that order**: merged
    first, a bold `one ` beside a bold `two` is one span reading `**one two**`; hoisted first it comes
    apart into `**one** **two**`. Whitespace here is the tab and the whole Unicode Zs category — U+0020,
    U+00A0, U+1680, U+2000–U+200A, U+202F, U+205F and U+3000 — because CommonMark counts every Zs for
    flanking, so a closing delimiter behind any of them may not parse; U+200B is deliberately excluded,
    being Cf rather than Zs. U+00A0's membership is a deliberate asymmetry with `IrEndBlock`, where it
    is *content*. A span left holding nothing but whitespace loses its formatting rather than
    its bytes, which makes 5.5's "never emit delimiters around empty content" structural instead of a
    test the emitter has to remember. Nothing is hoisted inside a fenced block, where the whitespace is
    the indentation. The merge is a length extension over the arena and never moves a byte, which is
    sound only because the walker appends every span's bytes in span order and leaves no gap — so the
    pass **checks that the two ranges really meet** and declines the merge if they ever do not, rather
    than trusting an invariant a later milestone could quietly break. Hoisting splits a span in three,
    so the span array is rebuilt rather than rewritten in place, and every block's `spanAt` moves with
    it; that is `IrAdoptSpans`, and it is this module's one privilege. A table's cells change nothing
    here and that is the point: a cell's paragraphs are blocks in the same flat array, so Word's
    fragmentation inside a cell is merged by the same rule that merges it outside one, and this pass
    never learns that a table exists.
    An anchor is transparent to a merge and a link's brackets and an image are not, which is right while
    those reach the output -- and a **muted** span is transparent too, which is why `Convert` runs this
    pass a second time after `LinkResolve`. Muting removes a link's brackets *after* the merge decision
    was taken on the strength of them, so the two spans they separated end up adjacent; left unmerged a
    bold run either side of one emitted `**A****B**`, and an entity split across the pair went unescaped
    because `MdEscape`'s lookahead is span-local. That is M7's two coalescer rules -- brackets block a
    merge, an unresolved link is muted -- each right alone and wrong together.
    M10 changed nothing here either, and the prolog To Do that waited for it to add a field barrier is
    gone rather than done: a plain field's cached result is ordinary text, which Word splits from the
    text around it as readily as it splits any run, so it must merge like one; a `HYPERLINK` field's
    result is bounded by the same link markers a `w:hyperlink`'s is. A note reference is a marker like a
    link's brackets and stops a merge -- until `LinkResolveNotes` mutes one whose note does not exist,
    which is the second reason the pass runs twice. `TestRunCoalescer` pins all three.
  - `MdEmitter.h`/`MdEmitter.cpp` — one growable UTF-8 output buffer and one line buffer. Since M6 a
    line is assembled span by span in its **output** form — delimiters and escaped text together —
    rather than raw and escaped in one piece, because there is now markup between the spans and a pass
    over the assembled line would escape that markup too. The two rules that need to see more than one
    span are handled by looking wider rather than by escaping later: the ampersand lookahead is safe
    within a span because the coalescer has already merged every adjacent pair with equal formatting,
    so a split entity can only be separated by markup that stops it being one; and D12's dollar count
    is taken over the whole line and passed into each span's escape call. Blocks are separated by
    exactly one blank line and each ends in one newline, so two paragraphs are `a\n\nb\n` and an
    empty document is zero bytes — with one exception, a bare `>` between two consecutive quote blocks,
    because a blank line there would close the blockquote and open a second. Each
    line loses its leading and trailing ASCII padding — four leading spaces would be an indented code
    block, two trailing ones are Markdown's other hard break. A hard break with nothing after it is
    dropped and two with nothing between them collapse, because a Markdown line that is empty ends the
    paragraph and neither `--hard-break` spelling can carry an empty continuation line. Inside a
    heading a break becomes exactly one space.
    Delimiters nest in one fixed order, outermost first: `<sup>`/`<sub>`, then the strikethrough, then
    the emphasis, then a code span's backticks — whose run is one longer than the longest run inside
    the content, padded with a space when the content begins or ends with a backtick. **Where a
    Markdown delimiter cannot parse where it stands, an HTML element takes its place**; see the mapping
    table's four rows on it, and `MdStrikeAsHtml`, `MdFlankingSafe` and `MdEdgeBehind` for the rules.
    The element also stands in where the flanking classes cannot see the problem at all: CommonMark
    reads adjacent runs of one delimiter character as a single run and then pairs openers to closers by
    *length* — its rule of three — so three emphasis spans meeting with no text between them can leave a
    run no pairing resolves, and `**bo*****th****ree*` comes out as
    `<strong>bo</strong>***th***<em>ree</em>` -- six literal asterisks in the reader's text, and the
    middle span lost outright. A span abutted by an identical run on both sides is therefore written as an element,
    which has neither a length nor a flanking rule and also keeps the two Markdown runs apart. That
    fallback is session-derived, not ruled, and it is the one place M6 writes markup no DOCX feature
    asked for -- the four rows the mapping table carries for it were added to record it, which is why
    this bullet can point at them.
    M7's four span kinds emit here too: a link is its content between brackets and its destination in
    parentheses, percent-encoded rather than backslash-escaped; an image is that with a `!` in front and
    its alt text between the brackets; an anchor is the raw `<a id>` of mapping row 22; and a muted span
    emits nothing at all. Two rules come from a link needing more than the span it stands on. A link that
    runs over a hard break is **closed at the end of its line and opened again on the next**, because
    Markdown cannot spell one that does — and a break at the very *edge* of a link leaves a half with
    nothing between its brackets, so that bracket is unwound instead of closed: `[](url)` is a link a
    reader can neither see nor click. And an **exclamation mark immediately before a link's `[` is
    escaped**, because the pair is an image marker and "see this!" followed by a link renders as a broken
    picture with the text gone. That is `docs/CONVERSION_REFERENCE.md` 4.2's pitfall 7, and `MdEscape`
    leaves the mark alone on purpose: it is only dangerous next to a bracket the emitter itself writes,
    which is knowledge a run does not have.
    M7 re-cut the two lookaheads the fallback rests on, which its roadmap entry asks for by name: both
    used to read past a non-text span to the text behind it, which was right while every neighbour *was*
    text, and a link start now writes `[`, a link end `]`, an image `!` and an anchor `<` — every one of
    them punctuation, so reading past one would report a letter where a bracket stands. A delimiter run
    cannot merge with one on the far side of a bracket either, so anything but text ahead reports no
    formatting at all. A link that runs over a hard break is closed at the end of its line and opened
    again on the next, because Markdown cannot spell one that does; a span `LinkResolve` muted emits
    nothing.
    A fence is sized from the longest backtick run **across** its blocks' spans rather than within each,
    because a code block's spans need not carry equal formatting — a bold ` `` ` beside a plain `` ` ``
    stays two spans, and measuring them apart sizes the fence at three, which the content's own three
    then closes. Its outermost blank lines are trimmed by whether a block holds a byte worth a line of
    its own, not by its byte count, so a code paragraph of nothing but padding does not open the fence.
    M8 gave it the **per-line prefix stack** its own To Do item 2 asked for, and it is what makes nesting
    work rather than a tidiness. A child list must be indented to its parent item's **content column** —
    the marker's own width plus the space after it — so `- ` is two columns, `1. ` is three and `10. ` is
    four; a fixed two-space step is wrong the moment a list reaches item ten, which is not exotic,
    because it flattens the whole list into one level. The stack carries the column each open level's
    marker actually landed at, so the indentation is computed from what was written, and a `w:ilvl` is
    mapped onto an emitted depth **through the stack of levels still open** rather than used as one. Two
    separate things force that. A level the document skipped over would put a marker four columns past
    its parent's content column, and four past it is an indented code block — the list would not merely
    look wrong, it would stop being a list (5.4 allows a skip to be normalised to one Markdown level per
    step, and this is that). And a run of items that *begins* at a deep `w:ilvl` has no parent to indent
    under at all, so using the level directly emits a shallower item further in than the deeper one above
    it and inverts the document's own nesting. An ordered marker is capped at nine digits, where
    CommonMark stops reading one. Row 17's `<!-- -->` separator is written between two **ordered** lists
    only, at the level's own indentation and with no blank line on either side: what a merge costs is the
    second list's start number, which two bullet lists do not have, so a comment between those would be
    markup written for no one, and a pair whose marker kinds differ separates itself. Three shapes inside
    a list take a blank line in front of them because each is a block that cannot interrupt a paragraph —
    a marker-less continuation paragraph, a nested list whose first number is not 1, and a nested list
    whose first item is empty. The last is the worst and it is silent: a lone `-` under a line of text is
    a **setext underline**, so the line above becomes a heading rather than merely losing its structure.
    A quotation that is also an item takes its marker first and its `> ` after it, and `MdSeparate` takes
    both neighbours now rather than one, because M6's bare `>` between two consecutive quote blocks is
    right for a quotation a producer broke in two and wrong for two quoted items, where it would put a
    stray `>` between two markers. `MdEmitFence` takes a prefix and rolls it back off a line that turned
    out to be blank, so a fence inside an item is indented without its blank lines gaining trailing
    spaces. A run of items is grouped the way a run of code paragraphs is, because what separates two
    items of one list is nothing at all and no block separator can write that; the run of code
    paragraphs now stops *before* an item, so a fence inside an item is emitted in its item rather than
    at column zero, and a run of marker-less code continuations whose lines land in one column is one
    fence rather than several, which is row 12's merge inside an item.
    M9's table comes out in one of two forms, and the top loop steps over every block the table owns
    once it has written one. The **pipe** form is a leading `|`, one padded cell per grid column, a
    delimiter row under the first row, and a row per row after it. The delimiter row is what makes the
    lines around it a table at all -- GFM reads one only where it holds exactly as many cells as the
    header -- so the width it is written at is the wider of what `w:tblGrid` declares and what the
    widest row's cells reach, and every row is padded to it by walking its cells once rather than
    scanning for each column. A pipe table's cell is inline content, so a cell's blocks are flattened
    into one line joined by `<br>`: a hard break is the same element, a list item keeps its marker as
    literal text because losing `3.` from a cell loses the document's count, a code paragraph becomes a
    code span because that is the inline form of the fence it would have been, and a heading, a
    quotation and a horizontal rule keep only what they say. A merge is padded -- the content in the
    first column the cell covers and an empty pad in the rest, with a vertical merge's continuation
    empty because that is what Word draws.
    The **raw-HTML** form fires for a table holding another table always, since a pipe table has no way
    to say one, and for a table holding a merge under `--tables=html-on-merge`. Everything inside it is
    written as HTML rather than as Markdown -- `<strong>`, `<em>`, `<del>`, `<code>`, `<a href>`,
    `<img>`, `<br>`, and text through `MD_CONTEXT_HTML_BLOCK` -- because a CommonMark HTML block passes
    every byte of itself through unparsed, so a `**` there would reach the reader as two asterisks.
    No line is blank, or the block would end and the rest would be read as Markdown again; every row is
    one line except where a cell holds a nested table, which opens on that cell's line and puts each of
    its own rows, and its `</table>`, on a line of its own. An open-merge count per column is what makes
    the grid exact: a cell a rowspan already covers writes nothing, and a `w:vMerge` continuation that
    *nothing* covers -- which a producer writes when an intervening row spans across the column the merge
    was opened in -- is an ordinary empty cell rather than nothing at all, because dropping it leaves the
    row a column short.
    A `rowspan` is written only where **every** column the restart covers is continued below it, since
    that is the whole of what a rectangle can promise: a restart wider than its continuation claimed
    columns nothing continued, and a browser then pushed the next cell of that row past them, so the
    raw-HTML form rendered a column wider than the pipe form of the same document. `MdRowSpanOf`'s inner
    walk also stops at the first column no continuation claims and never looks past the restart's own
    end, and the function is never asked at all for a cell outside the grid, which is what keeps it
    linear: without both, 64,000 merges in a 21 KB `.docx` took sixteen seconds. A cell's trailing
    padding and the `<br>` it hid are trimmed in **both** forms by one function over whichever buffer
    holds the cell -- the line buffer for a pipe row, the output itself for raw HTML -- because a break
    at the end of a cell has no next line to start, and a break with nothing after it is dropped
    everywhere else in this emitter. A content-free item is a marker
    on a line of its own in the middle of a list and
    nothing at all at either **end** of one, so a list's two edges are trimmed: an empty last item is
    the paragraph a user leaves behind on pressing Enter to get out of a list, and an empty first one is
    the same artefact at the top — **unless the item after it is deeper**, in which case it is the
    parent those items hang from, and trimming it promotes them to the outer list where the next
    shallower item becomes their sibling and a renderer counts it on from their numbers. A marker-less
    continuation with nothing in it is skipped whole, because it has no marker to stand for it the way
    an empty *marked* item does; and because it is skipped, every question about "the block before this
    one" is asked of the last block that actually **emitted a line** rather than of the previous record
    — the two are not the same, and reading the record suppressed a blank line the block before it had
    earned, merging two paragraphs of one item into one.
    M10's notes emit here too. A note reference is `[^n]`, n being the label `LinkResolveNotes` gave its
    note, and the definitions follow the body in **label order**, each opening `[^n]: ` with every later
    line of it indented four columns. A definition is a container like a list item, and it is carried by
    the line-start mechanism rather than by a block kind: every line start in this module writes a
    **base** before its own prefix -- nothing in the body, and in a definition the `[^n]: ` marker on its
    first line and four columns on every line after -- so every block kind is writable inside a note
    without one of them learning that notes exist: a second paragraph, a list at its content column, a
    fence, a pipe table, a quotation, a heading, a rule. A note that came to nothing is `[^n]:` alone,
    which GFM reads as an empty definition rather than leaving every reference to it as literal text. Two
    bytes after a reference are escaped because they change what it is -- `(` would make `[^n](x)` a
    link, and `:` where the reference opens a line would make `[^n]: x` a definition of its own -- and
    nothing else is. The blank line before the definitions is a plain one even between two quotations,
    because a bare `>` there would carry the body's blockquote into the note. Inside a raw-HTML table a
    reference is `<sup>n</sup>`, because GFM parses no Markdown in an HTML block, and inside a fence it
    writes nothing, because a fence emits its text and nothing else; the note is defined either way, and
    what GitHub then does with it is under Known gaps. **Every line of a raw-HTML table takes the table's
    prefix, the line a nested table's `</table>` leaves the rest of its cell on included**: at column zero
    that line ended the definition and moved the rest of the note into the body. M10's hostile-input pass
    found it before commit; in the body a table's prefix is always empty, so no earlier milestone could
    reach it.
  - `LinkResolver.h`/`LinkResolver.cpp` — where a reference becomes a destination, and where correctness
    rule 1 is kept for content. `LinkResolveRefs` looks a relationship id up in the part it was read in,
    because ids are scoped per part; a hyperlink to an External target becomes that URI (with the
    `w:anchor` fragment appended when the element carried both), a hyperlink to a part inside the package
    becomes nothing, and an image becomes the part name for `MediaPlan` to turn into a file path. A
    reference that resolves to nothing leaves an empty destination, which every later stage reads as "no
    link": the text stays and the brackets go. `LinkResolveAnchors` is mapping row 22's two halves. A
    bookmark that sits in a heading resolves to that heading's own GFM slug and then emits nothing, since
    the heading already carries the anchor a renderer generates; one that sits anywhere else resolves to
    its own sanitised name and is emitted as an `<a id>` element where it stands. An anchor nothing points
    at is **muted**, which is what keeps `_GoBack` -- in every document Word saves -- out of the output
    without this module knowing its name; a link naming a bookmark the document does not define is muted
    too, and `IrDropEmptyBlocks` then removes a block that held nothing else. `LinkResolveAnchors` mutes
    empty links before it resolves anything and again after its last pass, because resolution is itself
    a way for a link to lose its destination. Names are paired
    through an **open-addressed index built once**, for the reason M5's review found in `StyleModel`: a
    document may carry tens of thousands of bookmarks and as many references, and pairing them by
    scanning is quadratic in a way no fixture notices. **Relationship ids are indexed the same way and for
    the same reason**, found one milestone later by the same kind of probe: `OpcFindRelById` is a scan,
    which suits the handful of lookups every earlier milestone made, and M7 makes one per hyperlink and
    one per picture against a part declaring one relationship for each -- 32,000 links took 2.34 seconds
    and now take 0.10. The index is built here rather than in `OpcPackage` because the scan is still the
    right shape for a caller that makes three lookups, and a failed allocation falls back to it: the index
    is a speed measure and never a reason to fail a conversion. The slug is the piece that has to agree
    with a machine nobody here controls -- we write `#dont-panic` and GitHub writes the heading's own id
    -- so both halves of github-slugger's rule come from the Unicode character database rather than from
    a guess: 753 ranges of code points a slug keeps and 181 runs of simple lower-case mappings, all
    **1,112,064** code points agreeing with Python's `unicodedata`. The keep set is **L, M, Nd and
    connector punctuation** -- `Nd` and not the whole of `N`, which is the one place a plausible reading
    of "a number" is wrong: github-slugger's removal class takes out the superscripts and the vulgar
    fractions while leaving the feminine ordinal, the micro sign and the masculine ordinal in the gaps
    between its Latin-1 ranges, and Bengali says it again with the digits kept and the currency
    numerators beside them dropped. A heading's leading and trailing padding is dropped before any of it,
    because an ATX heading's content is its line stripped of whitespace at both ends and a renderer never
    sees what a producer left there; interior padding is kept, one hyphen per space, and a character the
    keep test drops does not break the run. Duplicates are numbered by github-slugger's own loop and not
    by a counter, because a heading may be called "Introduction 1" — and the base slug stops short of its
    buffer by `LINK_MAX_NUMBER_BYTES`, so every numbered form of a slug this module accepts has somewhere
    to go. That margin is not tidiness: without it the counter's digits went past the end of a 512-byte
    stack array for a heading whose slug filled it, and the over-long length then handed to `IrStoreDest`
    published the adjacent stack in the document. This module is **not** in
    `docs/CONVERSION_REFERENCE.md` 6.3's stage list and
    is a session addition like `Ir.cpp` and `Convert`: heading slugs are numbered over the whole document,
    so the pass has to see all of it, which neither the streaming walker nor the per-block emitter can.
    Since M10 `LinkResolveRefs` resolves **block by block** rather than against one part: a block of the
    body against the main part and a block of a note against the part its `IR_NOTE` names, through one
    relationship index per part, so `rId3` in `footnotes.xml` and `rId3` in `document.xml` are two
    relationships -- which is what finally tests correctness rule 1's scoping, because
    `tests/fixtures/footnotes` gives both parts an `rId5` and an `rId2` meaning different things. And
    `LinkResolveNotes` is mapping row 24's "renumbered 1..n": **one** sequence for footnotes and endnotes
    together, in the order the references are *read* -- the body first, then each note in the order it
    was numbered, so a reference inside a note to one not yet reached takes the next label. That is the
    order GitHub itself numbers footnotes by on the rendered page, and it is why the two stories are
    interleaved rather than the endnotes following the footnotes as `docs/CONVERSION_REFERENCE.md` row 24
    offers: GitHub renumbers by first reference whatever the labels say, so any other order puts one
    number in the `.md` and a different one on the page. A note referenced twice keeps one label; a
    reference to a note the document does not hold -- an unknown `w:id`, a separator's, a missing part --
    is muted, which is 5.4's degradation, and the text either side of it meets. It runs before
    `LinkResolveAnchors` because a heading's slug includes the labels in it: GitHub builds a heading's id
    from its rendered text, and a reference renders as its number.
  - `MediaExtractor.h`/`MediaExtractor.cpp` — stage [11], split in two so that nothing reaches disk before
    the document does. `MediaPlan` is the half that reads and writes nothing: it gives each distinct part
    a name, `imageN.ext` in the order the document first draws it, and rewrites the span to point at it. A
    part drawn twice keeps one file (5.8). The extension comes from `[Content_Types].xml` and **never**
    from the entry name, which producers get wrong -- `tests/fixtures/images` carries a part named
    `mystery.png` typed `image/jpeg`, which is 1.2's Google Docs trap -- and falls back to the entry
    name's own extension only where it is short and alphanumeric, and to `.bin` otherwise. A picture whose
    part the archive does not hold, and one whose reference resolved to nothing, degrade to their alt text
    exactly as `--no-images` makes every picture do; neither is a refusal, because a picture that cannot
    be found is a defect in the document. **So does a picture inside a fenced code block**, before
    anything is planned: a fence emits its text and nothing else, so extracting the file would put a
    picture on disk that no line of the document refers to and lose the alt text as well. The path this
    module writes into the document percent-encodes `#`, `%` and `?` -- the three bytes
    `MD_CONTEXT_LINK_DEST` deliberately leaves alone, because a *target* arrives already encoded far more
    often than it arrives holding a literal one, and none of that reasoning holds for a name this module
    generates from a document called `draft #2.docx`. `MediaWrite` is the half that touches the
    filesystem, and it runs **after** the `.md` is written, so a conversion that fails before that point
    leaves nothing behind at all; a half-written picture is deleted the way a half-written `.md` is. The
    other order is not promised: a media directory that cannot be created leaves the document beside a
    picture it names and does not have, which is the right way round -- the text is what the conversion
    was for -- and the run still reports a failure. No archive entry name ever reaches disk, which is
    correctness rule 10's other half.
  - `Convert.h`/`Convert.cpp` — the per-file pipeline: container, package, relationships, styles,
    numbering, walk, notes, coalesce, resolve, plan the media, number the items, emit, write, extract. M9
    added no stage to it: a table needs no part of its own, and its cells' blocks go through every pass
    already there. This is
    the function one worker runs when M13
    adds the bounded pool, which is why it
    is a module and not a lump of `main.cpp`. The styles, numbering, footnotes and endnotes parts are all
    resolved through the main part's relationships, by `ConvertRelatedPart` over their four
    `OPC_REL_*` kinds -- all four looked up at once, straight after the main part's own relationships
    are loaded, each coming back as a part index -- and an absent one is legal. Since M10 the notes are
    walked after the body, footnotes before endnotes, by `ConvertNotes`, which loads a notes part's own
    relationships **only once one of its notes was read**: a malformed relationships part is then a
    refusal naming that part, exactly as the main part's is.
    The passes between the walk and the
    emitter run in
    the one order that works: references resolve against the part they were read in, note references take
    their labels, anchors resolve once
    every reference is a destination (a heading's slug is numbered over the whole document), the coalescer
    runs a second time because muting a link or a note reference makes two spans adjacent that were not,
    the media plan
    turns a part name into a path and can turn a picture back into its alt text, `NumAssignMarkers` runs
    before the drop, which spares every block that pass leaves a list reference on, and dropping the
    emptied blocks last is what restores the invariant the emitter rests on —
    that every block it is handed
    produces at least one byte. `ConvertOutputPath` is pure and allocation-free and is
    therefore what the unit suite hammers: D7d's rule is `-o` as a filename for one input and a
    directory for several, otherwise the input's own path with its extension replaced. A trailing
    separator names a directory whatever the input count, because no Windows file name may end in one.
    `ConvertMediaDir` is pure too and shares `ConvertSplitPath` with it, which is not tidiness: a document
    written as `report.md` must find its pictures in `report_media`, so one function has to answer both.
    With no `--media-dir` the directory is the document's own stem with `_media` on it, beside the
    document, and the path a reader follows is a single leaf, so the pair survives being moved together;
    with `--media-dir` the directory is exactly what was asked for and the emitted path is the same string,
    which is relative to the working directory rather than to the document — the user chose it, and
    second-guessing a path they typed would be worse than honouring it.
    The output file is written with `CreateFileW`/`WriteFile` and **deleted again if the write does not
    finish** — a half-written `.md` that looks converted is worse than none. A derived path equal to
    the input is refused rather than overwritten, and `ConvertTargetTaken` is the pre-flight over the
    whole input list: D7b derives every name from an input's own leaf, so two inputs called
    `report.docx` in two directories both target one `report.md`, and left alone the second silently
    destroys the first. The first input to name a path keeps it; a later one, and any input whose
    derived output is another input of the same run, are refused into D7c's failure list. One input
    named twice is not a collision, because it writes the same bytes over its own output. The
    architecture note gives that pre-flight to `Batch` at M13; `main.cpp`'s loop is what `Batch`
    replaces, so it lives there until then.
  - `main.cpp` — `wmain`, `SetConsoleOutputCP(CP_UTF8)`, option handling, the input loop and the
    exit-code fold. There is no positional output operand (D7b) and no literal part name anywhere.
  - **What the binary does at M10**: `--help`/`--version` exit 0, a usage error exits 1 after printing
    the message and the usage text to stderr, an input that cannot be opened exits 2 and is named, an
    input that is not a usable DOCX exits **3** with a sentence saying which rule it broke **and, except
    where the styles or numbering part is not well-formed XML, which part broke it**, an output that
    cannot be written exits 4, and a sound package is **converted** and
    exits **0**, having written `<stem>.md` beside its input and said so in a note. `--stdout` writes
    the document to standard output instead, through `DiagWriteOutBytes`, which goes to the handle
    rather than the CRT stream so that Windows cannot turn the emitter's LF endings into CRLF. A run
    that converted something and failed something exits **6**, which D7c reserves for exactly that; a
    run in which everything failed returns the highest of their verdicts. Since M7 a document that draws
    pictures also writes them, into `<stem>_media\` beside the `.md` or into `--media-dir`, and says how
    many in a note; `--no-images` turns that off and keeps the alt text. M8 changed none of that surface
    and **M9 changes one thing about it**: `--tables=<gfm|html-on-merge>` joins the option set, with
    `gfm` the default, and a bad value for it is a usage error like any other. A table needs no part of
    its own and a malformed one converts rather than refusing, so no new exit code and no new note
    arrived with M9 either. **M10 adds no option, no exit code and no note**, but it reads two more
    parts, and a footnotes or endnotes part a document needs -- one that something already walked
    references a note of, or the relationships part of one a note was read from -- refuses the document
    with exit 3 exactly as a malformed body does. The walk's sentence for a part that is not well-formed
    XML now **names the part**, because a walk reads more than one -- a malformed body says
    `..., in word/document.xml` -- and so does its sentence for a notes part whose root is not the story
    its relationship names. A notes part nothing references is never read, so it
    can refuse nothing.
  - **What M10 converts and what it does not**: paragraphs, headings, hard breaks, tabs, hyphens and the
    escaping that keeps all of it from being re-read as markup; bold, italic,
    strikethrough, superscript, subscript, inline code, fenced code blocks, blockquotes and the
    horizontal rule; hyperlinks, bookmark anchors, heading slugs, images and the media
    files they come from; bullet and numbered **lists**, nested by `w:ilvl`, with real
    computed numbers, the whole `w:num`/`w:abstractNum`/`w:numStyleLink` indirection behind them,
    `w:lvlOverride`/`w:startOverride`/`w:lvlRestart`, and numbering that arrives through a paragraph
    style; **tables**, as GFM pipe tables with a delimiter row sized from the grid,
    alignment from the first row's `w:jc`, `w:gridSpan` and `w:vMerge` padded into it, a cell's blocks
    flattened and joined by `<br>`, and a raw `<table>` where a nested table or `--tables=html-on-merge`
    asks for one; and — new at M10 — **fields**, complex and simple, with `HYPERLINK` and `REF \h`
    becoming links, a `TOC` vanishing whole whether it is a field or a content control, and every other
    field showing its cached result; **footnotes and endnotes**, as `[^n]` references and definitions
    after the body in one sequence numbered by the order they are read, with a note's own relationships
    resolved against its own part and a note holding any block the body can; and the last of the
    **tracked changes** accept-all needs, a deleted paragraph mark joining two paragraphs and a deleted
    cell dropping out of its row. `w:sym`, `m:oMath` and comments are still skipped whole. Underline,
    highlight, colour and size are dropped by policy and always will be (mapping rows 8 and 9 for
    underline and highlight, and `docs/CONVERSION_REFERENCE.md` 2.3 for colour and size).

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
  (`SetConsoleOutputCP`) sit outside the `WIN32_LEAN_AND_MEAN` guard in `windows.h`. Nineteen
  `<ClCompile>`s, all `src\…`, and twenty-five `<ClInclude>`s: the six `include\…` headers and nineteen
  `src\…` ones.
- `DOCXtoMD.vcxproj.filters` — lists the nineteen `src\*.cpp` files under Source Files and all twenty-five
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
  compiles every `src\*.cpp` except `main.cpp`, which owns `wmain`, plus the fifteen files in
  `tests\unit\`.
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
- `tests/` — the container and package test scaffolding, the golden runner and the unit suite.
  `make_fixtures.py` builds every fixture; `run_container.py` runs the exe over them and checks the exit
  code and the message; `run_golden.py` converts every golden and byte-compares it. All three
  are CRLF like the rest of the tree and carry **no shebang**, because a CRLF shebang does not survive on
  a POSIX host — run them as `python tests/<name>.py`. There are **thirty-one** part trees under
  `fixtures/`: `minimal`, `relocated`, the five M5 golden cases `headings`, `toggles`, `textflow`,
  `nostyles` and `wrappers`, `dollars`, which D12 added, and M6's eight — `fragments` (mid-word run
  splits across rsids, a proofErr, a bookmark and an accepted insertion), `hoisting` (a trailing space
  inside bold, a leading one, a whitespace-only span, a tab, and the Zs characters beyond the ASCII
  space: U+00A0, U+2002, U+3000 and U+202F, with U+200B beside them because it is Cf and must *not*
  hoist), `inline` (every delimiter and combination, and the HTML fallbacks, including the one the
  closing half of the flanking test decides), `code` (both code detections, backtick collisions, two
  fences, the blank line that does *not* separate them, and the bold code run that must merge with the
  plain one beside it), `quotes`, `rules`, and the two monospace-baseline pins — `monodefault`, whose
  `w:docDefaults` names Courier, and `monostyle`, whose `w:docDefaults` names a *theme* slot and whose
  default `Normal` paragraph style names Courier, which is what Word actually writes. Neither may turn
  its document into one fence. M7 adds three more: `links` (every shape a hyperlink comes in — a dangling
  reference, an empty one, a nested pair, a target needing percent-encoding, a target inside the package,
  one in a heading and one broken by a hard break), `images` (both markup families, an external target, a
  chart with no bitmap, a picture inside a link, a part the archive does not hold, an `mc:AlternateContent`
  that must emit one picture and not two, a part named `.png` and typed `image/jpeg`, a shape whose
  `a:blipFill` must *not* become the paragraph's picture, and a picture inside a fenced block, which must
  become its alt text and write no file, a run whose text is split by the picture in the middle of it, and
  a reference that names no relationship at all) and `anchors`
  (a bookmark in a heading, one mid-paragraph, an unreferenced `_GoBack`, a link to a bookmark nothing
  defines, two headings that must be numbered apart, a bookmark between paragraphs, and one name declared
  twice, where the first carries the anchor and the second emits nothing). M8 adds four, and each pins
  something the others cannot. `lists` is the ordinary document — bullets, nesting, an ordered list that
  starts at three, a third level, a level the document skips over, formatting inside an item, a
  hard-break continuation, an empty item, a line whose own text would start a bullet, and a
  `List Paragraph` style carrying no numbering, which is Word's list *look* without the list.
  `listcounters` is the arithmetic: two `w:numId`s over one abstract definition continuing a single
  sequence across an interruption, a `w:startOverride` firing once and the `<!-- -->` it then needs,
  levels counting on their own, `w:lvlRestart` 0 refusing a restart, and a `numFmt none` continuation
  paragraph. `listbroken` is every way the graph can fail — a dangling `w:numId`, a `w:num` whose
  abstract definition is missing, a `w:numStyleLink` chased four hops to the definition it lands on, two
  definitions that delegate to each other, and a sound definition beside them all, unaffected.
  `liststyles` is numbering that arrives through the style chain — a style carrying the `w:numPr`, a
  style inheriting it through `w:basedOn` and naming only the `w:ilvl`, a `numId` of 0 cancelling it, a
  numbered heading staying a heading, a quotation that is also an item, and a monospace item that stays
  an item instead of becoming a fence. `w:lvlRestart` with a **value** is the one rule of M8 that is
  pinned at the unit level only, because a fixture for it would say nothing the counters fixture does not.
  M9 adds four, and each pins something the others cannot. `tables` is the ordinary document — a table
  as the very first thing in the body, a header row carrying `w:tblHeader` and three alignments, bold,
  a hard break, a two-paragraph cell, a literal pipe in text and another inside a code span, an empty
  cell, a short row, a cell whose text would otherwise start a bullet or an ordered item, two tables
  meeting with only a blank line between them, a one-row table, and a table of one empty cell.
  `tablemerges` is the padding policy: a `w:gridSpan` of two and one of three, a `w:vMerge` restart and
  its continuation, and a row whose cells reach past the grid, which widens the table rather than losing
  one. `tablenested` is the raw-HTML fallback a nested table forces, with the inline HTML spelling
  beside it — `<strong>`, `<em>`, `<del>`, `<code>`, a `<br>` and two entities. `tablecells` is block
  content a pipe table cannot carry: a bullet list, a heading, a quotation, two lines of code, a
  horizontal rule, a bookmark, two hyperlinks, and an ordered list inside a cell whose count continues
  in a paragraph after the table. `tablemerges` is converted a second time under
  `--tables=html-on-merge` by `run_golden.py`'s own options section, which is where a policy over one
  document is pinned rather than as a second fixture tree.
  M10 adds four, and each pins something the others cannot. `fields` is every field shape the walk
  reads -- a complex `HYPERLINK` with a tooltip, one whose instruction is split over three runs, one
  with only a `\l` location, a `REF` with `\h` and one without, `PAGE`, `NUMPAGES`, `DATE` and `SEQ`
  showing their results, a field with no separate showing nothing, an `IF` whose instruction holds a
  `MERGEFIELD` of its own, both `w:fldSimple` shapes, a link field across a hard break and across a
  paragraph, a field inside a `w:hyperlink` and a formatted result. `toc` is Word's two ways of writing a
  table of contents -- a content control whose gallery says so, and a bare `TOC` field spanning
  paragraphs, with a numbered entry, a bordered empty paragraph and hidden `PAGEREF` runs inside it --
  both of which must vanish while a `REF \h` to a heading after them still links. `footnotes` is the
  notes: four footnotes and an endnote cited from the body, one of them twice, a dangling reference, a
  reference before a parenthesis and one opening its line, notes holding a link, a second paragraph, a
  list, a picture and a table, an endnote cited only from a footnote, an empty note, and an unreferenced
  note whose picture must **not** be extracted -- and its `footnotes.xml.rels` gives `rId5` and `rId2` other
  meanings than `document.xml.rels` does, which is the test of per-part scoping M4 has been owed since
  it was written. `revisions` is accept-all: an insertion, a deletion, a move's two ends, formatting and
  style changes whose old values must not win, two deleted paragraph marks -- one joining into a
  heading, whose style wins -- a deleted row, deleted and inserted cells, and field instructions a
  tracked change edited or deleted. All four matched on their first run.
  `make_fixtures.py` also synthesises `media-binary.docx`, whose one media part holds every byte value,
  so that the byte path to disk is proved rather than assumed. Each case has an `expected.md` beside its
  `src/`, and every one was
  written by hand from the specification before the converter was run at it. The six M6 wrote up front
  all matched on the first run; the two monospace-baseline pins did not, and were not meant to — each
  was authored as the regression pin for a defect a review had just found, so each failed against the
  build as it stood and passed once its guard landed. Of M7's three, `links` and `images` matched first
  time and `anchors` did not: it is how `[nowhere]()` — what a link to a bookmark the document does not
  define was emitting — was found, which is the whole reason for writing one by hand. Of M8's four,
  `lists` and `liststyles` matched first time and the other two did not, for the same reason and to the
  same benefit: `listcounters` is how a nested run that *begins* at a deep `w:ilvl` was found to emit the
  shallower item further in than the deeper one above it, and `listbroken` is how a `<!-- -->` was found
  between two bullet lists that had nothing to separate.
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
  the exit code it should produce are declared in one place. Since M7 a second table beside it, `MEDIA`,
  declares which fixture must extract which files and what each must hold, and `run_golden.py` reads that
  the same way; the bytes come from the fixture's own tree, so a part edited under `tests/fixtures/`
  changes both sides of the comparison at once. Since M4 each row also carries a **`sound`**
  flag: whether the bytes are a well-formed ZIP an independent reader must read back. That used to be the
  same question as "does it exit 5" and no longer is, because a package can be a perfectly good archive
  and still not be a DOCX — without the flag every new package-level negative would silently drop out of
  the `zipfile` cross-check.
- `tests/unit/` — **exists** as of M4, doubled at M5, gained a ninth suite at M6, an eleventh at M7 and
  a twelfth at M8; M9 added no thirteenth, because a table is not a module — its cases went to the four
  suites that already own the stages it touches — and M10 added none either, for the same reason:
  `Check.h`/`Check.cpp` (one `CHECK` macro, a
  group heading and a pass/fail summary, over `typedefs.h` and `<stdio.h>` and nothing else — the header
  itself needs only `typedefs.h`, so a suite that includes it pulls in no I/O), `TestMain.cpp`, and one
  suite per module — `TestUtf.cpp`, `TestXmlPull.cpp`, `TestOpcPackage.cpp`, `TestStyleModel.cpp`,
  `TestNumberingModel.cpp`, `TestDocWalker.cpp`, `TestRunCoalescer.cpp`, `TestLinkResolver.cpp`,
  `TestMediaExtractor.cpp`, `TestMdEscape.cpp`, `TestMdEmitter.cpp`,
  `TestConvert.cpp`. Every case is driven from a string literal;
  nothing here opens a file, so the binary needs no working directory and no fixture path. `TestXmlPull`
  works by tokenizing a literal into a compact trace — `(name` opens, `)name` closes, `[text]` is
  character data, `$` is the end and `!n` is refusal *n* — so one string per case reads better than ten
  assertions. `src/` carries seven result-sentence tables, and five of them — `Utf`, `XmlPull`,
  `OpcPackage`, `StyleModel` and `NumberingModel` — are pinned against their enums by comparing
  specific rows against
  the exact sentence, because a sentence table and the enum indexing it drift apart silently; that
  check caught a real one-row misalignment during M4, and an M5 review caught the `OpcPackage` pair
  asserting only that the sentence was non-null, which `OpcResultText` can never return. `DocWalker`
  pins two rows -- the body's root and, since M10, the notes part's -- and `ZipReader`'s table is
  unpinned; both are still To Do. M5's suites reach the parser and the walker from string literals
  through `StyleLoadBytes` and `DocWalkBytes`, which are the halves of `StyleLoad` and `DocWalk` that
  work over bytes rather than over a package; `TestDocWalker` renders the whole intermediate
  representation into a compact trace — `H1{…}` a heading, `P{…}` a paragraph, `[text]` a span, `|` a
  break, and the letters before a bracket its formatting — so a case is one string comparison rather
  than ten assertions. M6 extended that notation rather than inventing one: `c` is a code span, and the
  block letters are `Q` for a blockquote, `C` for a line of a fenced block and `R` for a horizontal
  rule. `TestDocWalker` and `TestRunCoalescer` share it, which is what makes the pair readable: the
  first shows the fragmentation the walker preserves, the second shows the same document merged. M7
  extended it again rather than inventing one: `L(dest)` opens a link and `L)` closes it, `I(source)[alt]`
  is an image, `N(name)` is a bookmark anchor and `N-(name)` one that has been muted. M8 extended it once
  more: `[level#numId]` before a block letter is the list reference the walk read, and `[level=marker]`
  is what `NumAssignMarkers` settled — a `-` for a bullet, digits for a number, empty for a marker-less
  continuation — with a `!` for the first item of a list. M9 extended it a fourth time, and this time
  the notation had to grow a shape rather than a letter: a table is `T`, its column count, one character
  of alignment per column (`-`, `l`, `c`, `r`), then `m` for a table holding a merge and `n` for one
  holding a nested table; its rows are separated by `/` and a `w:tblHeader` row is prefixed `=`; and a
  cell is its blocks between parentheses, prefixed by its span where it covers more than one column, by
  `v` where it starts a vertical merge and `^` where it continues one. Because a cell's blocks are
  blocks, a renderer descends into a cell with the same function it renders the document with — and
  skips past a table's whole block range afterwards, or every cell would be rendered twice. The two
  renderers
  are independent copies with no shared header, so a kind or a field added to one and not the other makes
  the pair
  disagree about the same document — edit both. Each carries a `static_assert` on the block-kind count,
  so a seventh kind cannot be added without both traces being told about it, and since M10 one on the
  span-kind count too. M10 extended the notation a fifth time, and by letters again: `F(id)` is a
  footnote reference and `E(id)` an endnote one, `F-(id)` a reference `LinkResolveNotes` muted, and a
  block a note holds is prefixed `f2:` or `e7:` with its story and `w:id`. `NotedAs` walks a body and
  then its notes parts, optionally labelling them, which is how a case shows the walk and the numbering
  side by side.
  `TestMdEmitter`'s helper runs every pass `Convert.cpp` runs between the walk and the emitter but
  `MediaPlan`, in the same order, and no emitter case draws a picture, so what it measures is the shape
  the program really produces for the documents it is given; with no package a relationship resolves to
  nothing, so a `w:anchor` link is the half of M7 the emitter suite can reach and the rest is the
  goldens' to prove.
  Since M9 it also drives `--tables`, beside the `--hard-break` policy it already took; those two are
  the policies an emitter case can choose.
  Since M8 it runs `NumAssignMarkers` in that order too, over a numbering part the case supplies as a
  literal, which is what lets the emitter's list rules — the content-column indent, the `<!-- -->`, the
  three blank-line shapes and the setext hazard — be driven without a package. Since M10 a case may
  supply notes parts as literals as well, walked after the body exactly as `Convert.cpp` walks them, and
  `Cited` is the one-line form for the cases about what a single note may hold.
- **Not yet created** (GCS obligations, see Roadmap): `bench/` and CI. Do not reference them as if they
  exist. Everything else this section names does exist, `tests/run_golden.py` included.

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
DOCX exits 3 and is told which rule it broke and, except where the styles or numbering part is not
well-formed XML, which part broke it, an output that cannot be written exits 4, a sound package is
converted and exits 0, and a run that both converted and failed exits 6.
The fixtures and their expected verdicts are checked by

```bat
python tests\make_fixtures.py                                   :: writes tests\build\*.docx
python tests\run_container.py                                   :: runs x64\Release\DOCXtoMD.exe over them
python tests\run_golden.py                                      :: converts every golden and byte-compares it
python tests\run_container.py --exe x64\Debug\DOCXtoMD.exe      :: or any other build
tests\x64\Release\DOCXtoMD.Tests.exe                           :: the unit suite; prints a tally, returns 0 or 1
```

`run_container.py` and `run_golden.py` each build the fixtures themselves, so either alone is enough.
At M10 they return **157**, **118** and **1518** checks, over the **83** fixtures `make_fixtures.py`
builds. All four were confirmed on Windows on 2026-09-23. The three check counts are the interesting
ones: they are what the shim measures on Linux, and at every milestone since M3 they have been exactly
what the real MSVC binary then returned. The fixture count is not evidence of that -- `make_fixtures.py`
is the same Python on both platforms -- and is recorded only so a run that builds a different number is
noticed.
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
- **M4's coverage gap is closed by M10, at the golden level only.** The claim `OpcFindRelById` exists to
  support -- that relationship ids are scoped per part, so `rId3` in `document.xml` and `rId3` in
  `footnotes.xml` are unrelated -- is now tested: `tests/fixtures/footnotes` gives `rId5` and `rId2`
  different meanings in `document.xml.rels` and `footnotes.xml.rels`, and the body's link, the note's
  link and the note's picture each have to land on their own part's target. Resolving every block
  against the body's part instead fails three golden checks, which is how the mutation pass confirmed
  the test tests it. No unit case reaches it, because the unit suites build no package; `TestDocWalker`'s
  To Do says so.
- **Three M8 limits are declared and reachable by no test, and that is stated rather than carried
  quietly.** `NUM_MAX_ABSTRACT` and `NUM_MAX_NUMS` refuse a part declaring more than 4,096 definitions,
  and `NUM_MAX_DELEGATE` bounds a `w:numStyleLink` chase at sixteen links. None of the three thresholds
  is driven by a test. The delegation one is the subtler of them: a **cycle** is pinned at both the unit
  and the golden level, and since M8's review the cap is the whole guard that catches it -- the visited
  set that used to sit beside it could never change an outcome, because every exit but the one that
  finds a definition carrying levels leaves the delegation unresolved, and unresolved is a bullet at
  every level either way. So what is untested is the threshold rather than the behaviour: a cycle runs
  the cap out and lands where a visited set would have put it sixteen steps earlier. The two
  4,096 caps are unreachable from a suite whose whole point is that every case is one readable string: a
  literal declaring 4,097 definitions is about a megabyte of source. `NUM_ERROR_LIMIT`'s sentence is
  pinned against its enum row like every other, so the refusal path is wired even where the threshold is
  not driven. Generated fixtures would settle all three and are the obvious thing for **M11** to add,
  where hostile input is the milestone rather than a footnote.
- **One M9 limit is declared and reachable by no test, and one M6 promise is still not exercised.**
  `IR_MAX_COLUMNS` clamps a table at 256 columns and `IR_MAX_TABLE_DEPTH` drops a table nested past
  twelve; the depth cap **is** driven by a unit case, and the column cap is not. Unlike M8's 4,096 caps
  it is not expensive to reach -- 257 `w:gridCol` is about 3 KB of literal, and a single
  `<w:gridSpan w:val="257"/>` reaches the same clamp -- so what is missing is only the case, and M11
  will settle it with the others. What a cell past the cap loses is a column, which is the one place
  this build stops honouring "never drop a column" and says so in `IrBeginCell`'s own comment. And
  `DocWalker`'s save and restore of the paragraph classification, which M6 wrote and M9's roadmap entry
  expected to exercise, is **still exercised by nothing**: a `w:tbl` is a sibling of a paragraph and
  never a child of one, so a cell's paragraphs are walked with no outer paragraph open. The save is
  right and the case it guards is hypothetical. M10's note bodies did not make it real either, because a
  note is walked after the body rather than at its reference, so no paragraph is open when one is; row
  38's text boxes are now the only candidate.
- **`w:gridBefore` and `w:gridAfter` are not read, and two loops are dead because of it.** A row may
  declare that it starts part-way across the grid, which is what Word writes for an indented row or one
  whose leading cells were deleted; M9 reads neither element, so such a row's cells slide left into the
  wrong columns. That is a real gap rather than a policy -- `docs/CONVERSION_REFERENCE.md` does not name
  either element, which is why M9's scope did not cover it, and it is the obvious thing for **M11** to
  add -- M10's scope did not reach it either. What makes it worth recording here rather than only in a `To Do` is that both table
  forms already carry the loop that would serve it: a cell's column is derived from the one before it in
  `IrBeginCell` rather than read from the document, so a row's cells are contiguous and the gap-filling
  loop in `MdEmitPipeRow` and `MdEmitTableHtml` **cannot run**. Mutation testing found it -- deleting
  either changes no byte of any output -- and the comments above them said the opposite, that a producer
  writes a gap by omitting a cell, which is not something WordprocessingML can express. Both comments
  now say what is true, and the loops are kept as the code `w:gridBefore` will need rather than deleted
  as dead.
- **Nothing caps how many rows or cells a table may hold, and `<w:tc/>` is one of this build's largest
  IR amplifiers.** `IR_MAX_COLUMNS` caps the *emitted grid*, not the *stored records* -- `IrBeginCell`
  declines to clamp on purpose, so that two cells of one row can never claim the same column -- so a
  row may hold an unbounded number of `IR_CELL`s and a table an unbounded number of `IR_ROW`s. Seven
  input bytes retain a 24-byte record, and at the archive's own per-entry ceiling that is a file-to-peak
  memory ratio in the thousands. It is bounded by the ZIP caps rather than unbounded, and those caps
  were sized before M9 existed. **M11 owns it**, with the two 4,096 numbering caps and the 256-column
  one: the fix is the ceiling the columns already have, and capping cells per row at `IR_MAX_COLUMNS`
  would be the natural shape of it. `<w:tc/>` is not the only seven-byte element that retains a record:
  an interior `<w:br/>` keeps a 24-byte `IR_SPAN`, and `RunCoalesce` then reserves three span slots for
  every span, so a break costs more than a cell once the coalescer has run -- and a cap on cells per row
  would not touch it.
- **The raw-HTML fallback renders no cell decoration, and that is a limit rather than an oversight.**
  A `w:tcPr` may carry `w:tcBorders`, `w:shd` and `w:vAlign`, and the `<table>` form could carry all
  three where the pipe form can carry none. M9 reads none of them: the fallback exists to keep a merge
  and a nested table expressible, and adding style to it would make the two forms differ in more than
  structure. Three module headers -- `Ir.h`, `MdEmitter.h` and `MdEscape.h` -- carry it as a `To Do`,
  naming `w:shd` and `w:tcBorders`.
- **Five M10 limits are declared, and each is a shape Word's own interface does not produce or a
  renderer's rule this build cannot change.** A field **begin that neither separates nor ends** leaves
  everything after it in its story read as instruction, where `docs/CONVERSION_REFERENCE.md` 5.7 asks for
  the stream's end to count as an implicit end: a streaming walk cannot give back what it has already
  discarded, and only a pre-scan could, which is `DocWalker.h`'s To Do 2. A field that did separate or
  end is fine -- an end closes it, a separated field's result is read as it comes, and the end of the
  story ends it. A **note cited only from another note of its own story, or a footnote cited only from
  an endnote,** is not read, because its reference is not seen until the note holding it is, by which
  time its own story has been read, and the reference is muted; an endnote cited from a footnote *is*
  read, because the footnotes are read first. A **note reference inside a raw-HTML table** is written `<sup>n</sup>` and one inside
  a **fence** is not written at all, because GFM parses no Markdown in either; the note is still defined,
  and GitHub drops a definition no Markdown reference reaches, so that note reaches no reader. A
  **`w:customMarkFollows`** reference is written `[^n]` like any other, and the custom mark Word shows in
  its place is the content that follows it in its run, so it is emitted beside the reference as ordinary
  text; the reference calls this rare, and it is `DocWalker.h`'s To Do 4. And **list counters run across
  the stories in the order they are read** -- the body, then each footnote in the order its part holds
  them, then the endnotes -- so a list in a note over the same abstract definition as one in the body
  continues its numbers, which is what 2.9's counter keying says and may not be what Word shows.
- **Two M10 guards are defensive rather than live, and the mutation pass says so.** `DocFieldOpenLink`
  opens no link while the walk is quiet, and `DocFieldSeparate` ignores a second `separate`. Deleting
  either changes no byte of any output -- checked on the three suites, on six shapes built to reach them
  and on 1,500 generated documents -- because a link opened while quiet has nothing visible between its
  brackets and is muted, and a second separate re-reads the same instruction. Both stay, because the
  first keeps spans out of the IR that a later pass would have to know to discard, and the second keeps
  a malformed field from re-running its own analysis.
- **`w:lvlRestart` with a value is pinned at the unit level only.** `0` (never restart) and an absent one
  (restart under any shallower level) are both driven by `tests/fixtures/listcounters`; `N` is driven by
  `TestNumberingModel` alone, because a fixture for it would exercise nothing the counters fixture does
  not already show and would cost a reader a second document to hold in their head.
- **Two M7 rules are live and pinned by nothing, and both are stated rather than quietly carried.**
  `MdFormatAhead` reports `IR_FMT_NONE` for a markup span, which is the same reasoning as
  `MdEdgeAhead`'s and is right for the same reason -- a `[` between two emphasis spans separates their
  delimiter runs, so the rule of three does not apply across it. Every case that would distinguish it
  puts two adjacent asterisk runs on one line, where the *correct* output is itself the open question
  M6's abutted rule only half answers, so a test would pin an argument rather than a rule. And the
  fence emitter's "nothing but text reaches a fence" guard can no longer change a byte: the only span
  kind it dropped that carried any text was an image, and `MediaPlan` now degrades an image inside a
  fenced block to its alt text before the emitter sees one. The guard stays because the emitter must
  not depend on a pass that runs before it, but it is defensive code and not a live rule. Both were
  found by mutation testing, which is the only thing that finds this class -- and it found five more that
  are now pinned rather than merely stated: a picture ending the text span beside it, the first bookmark
  of a repeated name winning, an image whose reference resolved to nothing degrading to alt text, and both
  halves of the slug counter's reserved margin. A third mutation survives and
  is not the same kind of thing: deleting `LinkResolver`'s relationship index changes no byte of any
  output, because the index is a speed measure whose deliberate fallback is the scan it replaced. What
  pins *that* is a scale probe rather than a suite, and a scale probe is not something a definition of
  done can name until `bench/` exists (bd1/bd2).

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
| Bold / italic / strike | `**` / `*` (never `_`) / `~~`, except where CommonMark's flanking rules will not let the delimiter parse where it stands, or where its delimiter run cannot be paired by length — see the four rows below |
| Superscript / subscript | `<sup>` / `<sub>` |
| Underline, highlight, color, size | **Dropped** (no Markdown equivalent; hyperlink styling suppressed) |
| A strikethrough that wraps another delimiter | `<del>` — session-derived at M6. `word~~**x**~~` emits four literal tildes and no strikethrough: a `~~` in front of a `**` is followed by punctuation, so it may only open where the character before it is whitespace or punctuation too, and mid-sentence it is a letter. Two `~~` runs that meet fail as completely — `~~a~~~~b~~` is a run of four tildes, which GFM does not recognise at all. Raw HTML has no flanking rule |
| Emphasis or a strikethrough whose content touches punctuation at the edge, hard against a word character outside | `<strong>` / `<em>` / `<del>` — session-derived at M6, and the same rule as the row above generalised. `word**(a)**after` loses its emphasis entirely. Two delimiter runs that meet are one run to a parser, so the test steps back over an adjacent run before looking at what precedes it. "Punctuation" is CommonMark's own definition exactly — the Unicode P and S categories, as a generated range table `MdEmitter.cpp` binary-searches — so an Arabic full stop and a Devanagari danda are punctuation while a Roman numeral and a CJK ideograph are not |
| An emphasis span with an identical delimiter run hard against it on **both** sides | `<strong>` / `<em>` — session-derived at M6, and the one trigger that is not a flanking rule. CommonMark merges adjacent runs of one delimiter character into a single run and then pairs openers to closers by *length* — its rule of three — so three emphasis spans meeting with no text between them can leave a run no pairing resolves: `**bo*****th****ree*` renders as `<strong>bo</strong>***th***<em>ree</em>` -- six literal asterisks in the reader's text with the middle span lost outright, and there is no punctuation anywhere in it for a character class to catch. An element has neither a length nor a flanking rule, and it also keeps the two Markdown runs apart |
| A code span, wherever it stands | `` ` `` always. A code span has no flanking rule of its own, so it never needs the fallback |
| Inline code | `` ` `` — via code-named character styles or monospace `rFonts`. Code wins over bold and italic, and the bits are cleared in the **walker** so that two runs coming out as the same code span coalesce; left set, their backtick delimiters would meet and a renderer would read the pair as one span |
| Code block | Fenced ``` — consecutive all-monospace paragraphs merge into one fence, whose length is one more than the longest backtick run inside it and never fewer than three. No info string: the language is not recoverable. An empty code paragraph is a blank line of the fence, and is trimmed only where it falls at either end of one |
| Quote styles | `> ` blockquote — Quote, Intense Quote, Block Text and LibreOffice's Quotations, by name, never by indent. Two consecutive quote paragraphs are separated by a bare `>` rather than a blank line, so a quotation a producer broke in two stays one blockquote: session-derived at M6, and the one exception to the blank line between blocks |
| Bullet / numbered lists | `-` / real computed numbers (`3.` honors start); nested by `ilvl`. Counters are keyed on the resolved **abstract definition**, never on the `numId`, so two `numId`s over one definition continue one sequence (2.9); a `w:startOverride` is keyed by `numId` instead and fires the first time that `numId` is used. Those are the two commands Word's list UI offers |
| A `w:numFmt` this build does not recognise, and an absent one | **Ordered**, a decimal. Every ST_NumberFormat token but `bullet` and `none` counts, so an unknown one is far likelier to be a counting format than a bullet — and reading it as a bullet throws away ordering the counter already has, while reading it as a decimal loses only a glyph shape row 15 says the renderer discards. A level whose marker is a `w:lvlPicBulletId` picture is a bullet. Session-derived at M8 |
| A paragraph that is both a heading and a list item | The heading. 5.4 rules it outright, and it is the common case rather than an edge one: Word's Multilevel List linked to headings puts a `w:numPr` on every `Heading N` style, so without the rule every heading in such a document becomes an item and the structure inverts |
| A paragraph that is both a list item and a quotation, a fence, or all-monospace | An item, keeping its kind — `- > quoted` and a fence inside its item. But row 12's **font heuristic** is switched off for an item, on the same reasoning as its monospace-baseline guard: the font is a guess at what a paragraph is and a `w:numPr` is a statement, so a list of code lines set in Consolas stays a list rather than becoming fences that have each lost their marker. A code *style* is unaffected. Session-derived at M8 |
| A `w:numId` of 0, and a `w:numId` naming a `w:num` the part does not declare | Not a list at all — 0 is a specification of "no numbering" (2.4) and cancels whatever the style chain supplied, and a dangling reference degrades the way 5.4 asks every broken reference to. A `w:num` whose abstract definition is missing, and a `w:numStyleLink` delegation that loops, keep the item and take a **bullet**: the document has said the paragraph is an item and only the format is unknown |
| An empty list item | A bare marker on a line of its own, which CommonMark renders as an empty `<li>`. Word writes them, and the counter has already counted one, so dropping the block leaves a hole in the numbers. Trimmed off a list's two **edges**, where an empty item is the paragraph a user leaves behind on pressing Enter to get out of a list. Session-derived at M8 |
| A child list's indentation | The parent item's **content column** — the marker's own width plus one space, so 2 under `- `, 3 under `1. ` and 4 under `10. `. A fixed step per level flattens the whole list the moment it reaches item ten. A `w:ilvl` the document skips over is normalised to one Markdown level per step (5.4), because four columns past a parent's content column is an indented code block and the list stops being a list |
| An ordered marker past nine digits | Capped there, and the counter saturates at the same value. CommonMark stops reading an ordered marker at nine digits, so a tenth makes the paragraph stop being a list at all: a hostile `w:start` costs a wrong number rather than a lost list |
| Two adjacent lists that must not merge | `<!-- -->` at the level's own indentation, with no blank line either side (row 17) — but **only between two ordered lists**. What a merge costs is the second list's start number, which two bullet lists do not have, so a comment between those would be markup written for no one; a pair whose marker kinds differ separates itself. Session-derived at M8 |
| A marker-less continuation paragraph, a nested list starting at a number other than 1, and a nested list whose first item is empty | Each takes a blank line in front of it, because each is a block that cannot interrupt a paragraph. The last matters most and fails silently: a lone `-` under a line of text is a **setext underline**, so the line above becomes a heading rather than merely losing its structure |
| Tables | GFM pipe tables; header = first row (a later `tblHeader` row is not promoted — see the next row); cell breaks → `<br>`; merged → padded GFM cells (gridSpan: content in first cell + empty pads; vMerge continue: empty cell; HTML `<table>` under `--tables=html-on-merge`); nested → HTML `<table>` fallback |
| The header row, where several rows carry `w:tblHeader` or none does | **The first row, always.** GFM has exactly one header row and it is the one at the top, so a later `w:tblHeader` cannot be promoted without reordering the document — and the block array's order is what `LinkResolver`'s heading slugs and `MediaExtractor`'s picture numbering are both counted in. 2.5 offers an all-empty header row as a policy where the first row is clearly data; it is declined, because it costs a row of the reader's screen to say something no producer's markup actually asked for. Session-derived at M9 |
| How wide a pipe table is | The **wider** of what `w:tblGrid` declares and what the widest row's cells actually reach. The grid is authoritative (2.5) but it is not a ceiling: a row whose cells reach past it has columns the grid did not declare, and clamping to the grid is exactly the silent loss row 19 forbids. Every row is then padded to that width. For the header row that is required: GFM reads a pipe table only where the delimiter row holds as many cells as the header, so a short header row turns the whole table into a paragraph. A renderer pads a short body row itself, so padding those is this build's choice rather than GFM's requirement. Session-derived at M9 |
| Column alignment | `:---`, `:---:` or `---:` from the **first row's** own `w:jc`, because a delimiter row is the only place an alignment can be written and it stands under the header. `start` and `end` read as left and right, having no bidirectional layout here to reverse them against (2.5's `w:bidiVisual` is "note and ignore"); `both` and `distribute` are alignments GFM cannot spell and become none. A cell spanning several columns aligns all of them. Session-derived at M9 |
| Block content in a pipe table's cell | Flattened to one line, its blocks joined by `<br>`: a list item keeps its marker as literal text, because losing `3.` from a cell loses the document's own count; a code paragraph becomes a **code span**, which is the inline form of the fence it would otherwise have been; a heading, a quotation and a horizontal rule keep only what they say, because a `#` or a `> ` in a cell is literal text a reader has to ignore. Nesting inside a cell's list is lost, which is a known limit rather than a policy: GFM has no spelling for indentation inside a cell. Session-derived at M9 |
| A `w:vMerge` restart wider than the row continuing it | The `rowspan` is written only where **every** column the restart covers is continued, so a ragged merge becomes an ordinary cell of its own width and the row below it keeps its columns. HTML can only spell a rectangle; counted at the restart's first column alone, the cell claimed columns nothing continued and a browser pushed the next cell of that row past them, so the raw-HTML form rendered one column wider than the pipe form of the same document. Session-derived at M9, and found by the grid oracle once its generator was widened to put a restart on a spanning cell |
| A `w:vMerge` continuation nothing above it still covers | An ordinary empty cell. A producer writes one when an intervening row spans across the column the merge was opened in; dropped from the raw-HTML form it would leave that row a column short, which is the silently narrower table row 19 forbids. Session-derived at M9, and found by a grid oracle rather than by a fixture |
| Everything inside a raw-HTML `<table>` | Written as **HTML**, not Markdown: `<strong>`, `<em>`, `<del>`, `<code>`, `<a href>`, `<img>`, `<br>`, and text with only `&`, `<`, `>` and `"` turned into entities. A CommonMark HTML block runs to the next blank line and passes every byte of itself through unparsed, so `**bold**` in a `<td>` reaches the reader as two asterisks and `\*` as a backslash. No line of one is ever blank, for the same reason. Session-derived at M9 |
| A table nested past twelve deep, and a table with no rows | Skipped whole, and unwound whole. The depth cap is what keeps the walk's own stack off the document's content; an empty `w:tbl` costs no block and no blank line, because a table that came to nothing is not a blank line the reader asked for. Session-derived at M9 |
| Hyperlinks | `[text](url)` external, `[text](#anchor)` internal (GFM heading slugs). A slug is github-slugger's rule exactly: lower case, then everything outside Unicode L, M, **Nd** and connector punctuation removed, then each space to a hyphen — over the heading's content with the padding at its two ends stripped, as an ATX heading's own parsing strips it. `Nd` and not all of `N`: the renderer removes the superscripts, the vulgar fractions and the Roman numerals |
| A hyperlink whose destination resolves to nothing | The text, with no brackets — a dangling `r:id`, a target inside the package, a bookmark the document does not define. Session-derived at M7 and the same shape reference 5.4 gives a dangling numbering reference: degrade, never refuse. A hyperlink with no *content* goes the same way, which 5.6 asks for outright |
| A bookmark a link points at | The heading's own GFM slug where the bookmark sits in a heading, and `<a id="name"></a>` at the bookmark otherwise (row 22). A bookmark **nothing** points at emits nothing at all: session-derived at M7, and it is what keeps Word's `_GoBack` and `_Toc…` out of every converted document without the code knowing their names |
| Images | `![alt](media dir/imageN.ext)` — extracted, extension from content type, alt from `docPr/@descr` |
| A drawing container with no picture in it | Nothing — a chart, a SmartArt diagram, a drawn shape. Reference 2.6 leaves it to policy between a placeholder and a skip; skipping is session-derived at M7, because a placeholder invents content the document does not have and `docPr/@name` is "Chart 1" rather than a description |
| An image whose part the archive does not hold | Its alt text, as plain text, exactly as `--no-images` renders every picture. Session-derived at M7: a picture that cannot be found is a defect in the document, not in the conversion, so it is not a refusal |
| EMF and WMF | Extracted and linked like any other picture, which reference 1.2 leaves to policy between that and a warning. Session-derived at M7: no Markdown renderer will display one, but the file is what the document had and dropping it loses more than linking it does |
| Footnotes/endnotes | `[^n]` refs + definitions at end, renumbered 1..n — **one** sequence for both stories, in the order the references are read: the body first, then each note in the order it was numbered, so a reference inside a note to one not yet reached takes the next label. The definitions follow in label order, each `[^n]: ` with its later lines indented four columns, so a note may hold anything the body may. A note referenced twice keeps one label. Session-derived at M10: reference row 24 offers the endnotes after the footnotes, but GitHub renumbers footnotes by first reference whatever the labels say, and this is the one order in which the number in the `.md` is the number on the page |
| A note reference naming nothing — an unknown `w:id`, a separator's, a missing notes part — and a note cited only from another note of its own story, or a footnote cited only from an endnote | Muted: it writes nothing and the text either side meets, which is 5.4's degradation; `[^n]` with no definition is literal text to every renderer. The second shape is a limit, not a choice — see Known gaps. Session-derived at M10 |
| A note that came to nothing | `[^n]:` alone, which GFM reads as an empty definition. Leaving it out would turn every reference to it into the literal text `[^n]`. Session-derived at M10 |
| A note reference followed by `(`, and one opening its line followed by `:` | `[^n]\(` and `[^n]\:` — the first would make the pair a link, the second a definition of its own. Nothing else after a reference is escaped. Session-derived at M10 |
| A note reference inside a raw-HTML table, or inside a fence | `<sup>n</sup>` and nothing, respectively, because GFM parses no Markdown in either; the note is still defined, and GitHub drops it — see Known gaps. Session-derived at M10 |
| A field's cached result | Correctness rule 7 and reference 2.7 as written — a `HYPERLINK`, and a `REF` carrying `\h`, is a link around its result; a `TOC` field or a content control whose gallery says it is one vanishes whole; every other field is its result, and one with no separate is nothing — **except `INCLUDEPICTURE`**, which 2.7 would turn into `![](url)` from its instruction and which is its cached result here, a picture if it holds one. Session-derived at M10; `DocWalker.h`'s To Do 3 |
| A link a field's result opens across a paragraph break | Closed at the end of each paragraph and opened again at the start of the next: two links to one destination, as a hyperlink across a hard break already is. Session-derived at M10 |
| A paragraph whose mark a tracked change deleted | Runs on into the next paragraph (5.11), which gives the pair its classification; where the next block is a table, or the cell, note or body ends, it ends as written. Session-derived at M10 in the second half, because Word will not delete those marks |
| Horizontal rule (`pBdr` bottom on empty ¶) | `---` with blank lines around |
| `w:br` (textWrapping) / page break | Backslash hard break (`<br>` in cells, under either table form: a pipe table's row is one line by construction, and a line end inside an HTML cell renders only as a space) / nothing |
| Hidden text | Dropped, for `w:vanish` (a toggle) and `w:webHidden` (nearest-wins) alike |
| `w:caps` | The run's text is uppercased — ASCII and the Latin-1 supplement, which is where a 0x20 offset is exactly right; anything beyond needs Unicode's case tables and is a `To Do` |
| TOC (field or SDT), headers/footers, comments | Skipped |
| Soft hyphens | Removed; NBSP and smart punctuation kept verbatim |
| Output encoding | UTF-8, no BOM, LF line endings (tc2's CRLF governs source files, not program output) |
| Leading/trailing ASCII space or tab on an emitted line | Removed. Four leading spaces would be an indented code block; two trailing ones are Markdown's other hard break |
| A `$` on a line that holds two or more of them | `\$` — GitHub reads `$...$` as inline math and `$$...$$` as display math (D12). A line holding one `$` keeps it bare: a span needs two delimiters, and a price is the common case |
| Two hard breaks with nothing between them | Collapse to one, and a hard break with nothing after it is dropped. A Markdown line that is empty ends the paragraph, so neither `--hard-break` spelling can carry an empty continuation line |
| A hard break inside a heading | One space. An ATX heading is a single line by construction |
| A hard break inside a hyperlink | The link closes at the end of its line and opens again on the next — two clickable halves of one destination, because Markdown cannot spell a link that spans a line. A break at the very *edge* of one leaves a half with nothing between the brackets, and that bracket is unwound rather than closed: `[](url)` is a link a reader can neither see nor click. Session-derived at M7 |
| An exclamation mark immediately in front of an emitted link | `\!` — the pair `![` is an image marker, so "see this!" followed by a link renders as a broken picture with the link text gone (CONVERSION_REFERENCE 4.2's pitfall 7). `MdEscape` leaves the mark alone by design: it is only dangerous next to a bracket the emitter itself writes |
| A picture inside a fenced code block | Its alt text, as literal text of the fence, and no file extracted. A fence emits its text and nothing else, so an extracted picture would be one no line of the document refers to. Session-derived at M7 |
| `#`, `%` or `?` in a **generated** media path | Percent-encoded. The three bytes `MD_CONTEXT_LINK_DEST` leaves alone in a producer's own target, because that target arrives already encoded far more often than it arrives holding a literal one — which is not true of a name derived from `draft #2.docx` |

## Planned architecture (`docs/`, `include/`, `tests/` and twenty `src/` modules exist — build the rest by Roadmap)

M9 added no module, which is worth stating where a reader counts them: a table is a shape over blocks
that already exist rather than a stage of its own, so it landed in `Ir`, `DocWalker`, `MdEscape`,
`MdEmitter` and `CliOptions`, plus one line of `Convert` that hands `--tables` to the emitter, and the
count stayed at twenty. **M10 added none either**, for the same reason: a field is a state the walk
carries, a revision is a rule the walk applies, and a note is a run of ordinary blocks with a record
beside them -- so it landed in `Ir`, `DocWalker`, `LinkResolver`, `MdEmitter` and `Convert`, and the
count is still twenty.

**Written so far (M2 + M3 + M4 + M5 + M6 + M7 + M8 + M9 + M10)**: `src/main.cpp`, `src/BuildGuards.h`,
`src/CliOptions.h`/`.cpp`, `src/Diag.h`/`.cpp`, `src/Crc32.h`/`.cpp`, `src/Inflate.h`/`.cpp`,
`src/ZipReader.h`/`.cpp`, `src/Utf.h`/`.cpp`, `src/XmlPull.h`/`.cpp`, `src/OpcPackage.h`/`.cpp`,
`src/StyleModel.h`/`.cpp`, `src/NumberingModel.h`/`.cpp`, `src/Ir.h`/`.cpp`, `src/DocWalker.h`/`.cpp`,
`src/RunCoalescer.h`/`.cpp`,
`src/LinkResolver.h`/`.cpp`, `src/MediaExtractor.h`/`.cpp`,
`src/MdEscape.h`/`.cpp`, `src/MdEmitter.h`/`.cpp` and `src/Convert.h`/`.cpp`, plus everything already in
`docs/`, `include/` and `tests/`. Every other entry below is still to be written — do not reference one
as if it exists.

Three entries below are **not** in the list `docs/CONVERSION_REFERENCE.md` 6.3 maps the stages onto, and
all three are session-derived rather than ruled. `Ir.cpp` exists because the representation needs growable
arrays, and growable arrays need real functions rather than a header full of `inline` the p1 rule does
not license. `Convert` exists because the per-file pipeline is M13's worker body: it has to be callable
from something other than `wmain` before M13 arrives, and putting the output-path derivation there is
what lets the unit suite drive it — the test project compiles every `src\*.cpp` but `main.cpp`.
`LinkResolver` exists because a heading's GFM slug is numbered over the whole document, so the pass has
to see all of it at once — which neither the streaming walker nor the per-block emitter can do. 6.3 puts
that work in stage [9] and maps it onto `RunCoalescer`/`MdEmitter`; giving it a module of its own is a
divergence, recorded here and in the module's own header rather than left to be discovered.
`NumberingModel` **is** in 6.3's list, but half of what it does is not where 6.3 puts it: the counters
are a pass over the finished document rather than state the stage [6] walk carries, for the reason its
header gives — `IrRewind` unwinds a speculative `mc:Choice` and a counter table cannot be unwound with
it. That lands the numbers in stage [9] beside `LinkResolver`'s, which is the same shape and the same
reason, and it is recorded here and in the module's header for the same reason too.

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
                         [written at M5; a style's own w:numPr at M8]
   NumberingModel.h/.cpp numbering.xml → per-numId levels with overrides; the counter pass
                         [written at M8. The counters are a pass over the finished document, not walk
                         state — see the divergence note above]
   Ir.h/.cpp             intermediate representation (blocks/spans) — the walker never emits Markdown
                         [written at M5; the .cpp is a session addition, see above; the list fields on
                         a block at M8, which added no sixth block kind; M9's table records, the sixth
                         block kind and the row and cell chains -- a cell's blocks are blocks in the
                         same flat array, which is why no pass between the walk and the emitter changed
                         but IrDropEmptyBlocks, which moves a table whole; M10's note reference span
                         and note records, a note's blocks being blocks in the same array too]
   DocWalker.h/.cpp      document walk → IR (tracked changes, sdt, AlternateContent) [written at M5;
                         hyperlinks, pictures and bookmarks at M7; w:numPr read as a reference at M8;
                         w:tbl, w:tr and w:tc at M9, as two more dispatch levels so that every
                         transparent wrapper is handled once for all four; the field state machine,
                         the deleted paragraph mark and w:cellDel, and the notes walk at M10]
   RunCoalescer.h/.cpp   adjacent-run merging + whitespace hoisting  [written at M6; unchanged at M9,
                         which is the point of putting a cell's blocks in the same array, and at M10,
                         whose fields needed no barrier of their own]. The effective
                         format is resolved one stage earlier, in DocWalker, which is where the run
                         properties are — a divergence from CONVERSION_REFERENCE 6.2's [7]+[8], noted
                         there and in the module's own header
   LinkResolver.h/.cpp   relationship ids → destinations; bookmarks → GFM heading slugs or <a id>
                         anchors; the anchors nothing points at muted  [written at M7; a session
                         addition, see above; per-part resolution and the note labels at M10]
   MdEscape.h/.cpp       the context-aware escaping writer (pure, unit-testable)  [written at M5; the
                         pipes argument and MD_CONTEXT_HTML_BLOCK at M9]
   MdEmitter.h/.cpp      IR → Markdown text; blank-line discipline; delimiter sizing  [written at M5;
                         the delimiters, the block kinds and the flanking fallback at M6; the per-line
                         prefix stack and the list rules at M8; the two table forms at M9; the note
                         references and definitions, through a base every line start writes, at M10]
   Convert.h/.cpp        one file end to end: container → package → styles → numbering → walk → notes →
                         coalesce → resolve → plan → number → emit → write → extract, plus D7b's
                         output-path derivation and M7's
                         media-directory derivation. M13's Batch calls this per worker
                         [written at M5; a session addition, see above]
   MediaExtractor.h/.cpp referenced media parts → disk; content-type extensions; dedup; safe names
                         [written at M7, in two halves: MediaPlan names and rewrites without touching
                         the filesystem, MediaWrite writes after the document is safely out]
   Diag.h/.cpp           error codes/messages → stderr, and the --stdout document → stdout;
                         exit-code mapping. MT-safe from M13: every worker reports through this one
                         sink, so it locks then (D6). Reentrant at M2
tests/                   fixtures/<case>/src/ (unzipped part trees) + expected.md; make_fixtures.py and
                         run_container.py [make_fixtures.py written at M3 and extended at every
                         milestone since; run_container.py written at M3 and extended at M4]; run_golden.py
                         [written at M5, with the media table and the media options at M7];
                         unit/ holds the CHECK header and one suite per module, built by
                         tests/DOCXtoMD.Tests.vcxproj [written at M4, five more suites at M5, a
                         ninth at M6, an eleventh at M7, a twelfth at M8; M9 and M10 added no
                         thirteenth and put their cases in the suites that already own the stages they
                         touch]
bench/                   GCS p4 microbenches (create with the first performance claim)
docs/                    CONVERSION_REFERENCE.md (already here); module guides (d2/d3) still to come
include/                 the six owner-authored shared headers (already here); on the include path
                         via $(ProjectDir)include, so TUs include them by bare name
```

There is **no `third_party/`** and there will not be one (D1/D2): the shipped binary is first-party
code plus the CRT/Win32 and the six shared headers in `include/`, which every module may include.

Allocation-conscious modules (GCS p2 hot set): `Inflate`, `ZipReader`, `XmlPull` (zero-allocation
steady state), `DocWalker`, `RunCoalescer`, `MdEmitter` (one growable output buffer and one line
buffer, plus one note-order table per document), `Utf` — all allocating through `memory management.h`.
The parsed-once models (`StyleModel`, `NumberingModel`, `OpcPackage`, `CliOptions`) use the same
allocators but are not hot, and so do the three passes above the walk: `LinkResolver` allocates one
relationship index for each part whose relationship ids it resolves, and one note table, one name index
and one slug index per document, `MediaExtractor` one plan, and `NumAssignMarkers` one counter table
sized by the numbering part rather than by the document.

Under D6, **`Batch` and `Diag` are the only `MT-safe` modules**. Everything that converts a document
— `Utf`, `Inflate`, `Crc32`, `ZipReader`, `XmlPull`, `OpcPackage`, `StyleModel`, `NumberingModel`,
`Ir`, `DocWalker`, `RunCoalescer`, `LinkResolver`, `MdEscape`, `MdEmitter`, `MediaExtractor`,
`Convert` — is
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
  --tables=<gfm|html-on-merge>     (default gfm)
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
- Three runners, with different jobs. `tests/run_container.py` (M3, extended at M4) runs the exe
  over every fixture and asserts the exit code and a substring of the message, and reads every *sound*
  archive back with Python's `zipfile`; `tests/x64/Release/DOCXtoMD.Tests.exe` (M4, extended at M5 and M6) runs
  the unit suite, which drives every case from a string literal and touches no file;
  `tests/run_golden.py` (M5) converts every golden fixture twice — once to a file beside the input and
  once through `--stdout`, which are different code paths in `Convert.cpp` — and byte-compares both
  against the case's `expected.md`. None takes another's job.
- A golden fixture is a part tree under `tests/fixtures/<case>/src/` **plus** an `expected.md` beside it,
  and which built `.docx` compares against which case is declared in `make_fixtures.py`'s `GOLDENS`
  table, next to the exit-code table, so a fixture and what it must produce are named in one place. The
  mapping is many-to-one on purpose: fifteen fixtures compare against `minimal/expected.md` — the
  fourteen container fixtures, through which a byte comparison came to assert what M4 asserted with a
  message substring, and `unreferenced-bad-notes.docx`, whose unread notes part must cost the document
  nothing. That is also why one change to the emitter shows up in every one of them in a golden run, as
  M6's `**bold**` did.
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
- **M5 `[done]` Paragraphs & headings** — `StyleModel` (chains + toggle XOR), minimal
  `DocWalker`/`Ir`/`MdEmitter`, plus `tests/run_golden.py` (exe runner + byte-compare + exit-code
  assertions). DoD: first golden fixture converts byte-exact.
  **Status**: the code landed from Linux on 2026-08-25 as `[done-unverified]`, and the owner verified it
  on Windows the same day: every command under "Build & run" runs clean. Both x64 configurations of the
  solution and of `DOCXtoMD.vcxproj`, and a Release `/t:Rebuild`, build with **no errors and no
  warnings**; `python tests\run_container.py` passes all **99** checks against `x64\Release` and all
  **99** again against `x64\Debug`; `python tests\run_golden.py` passes all **47**; and
  `tests\x64\Release\DOCXtoMD.Tests.exe` passes all **839**. That discharges the milestone's own
  definition of done -- `run_golden.py` is what byte-compares the goldens -- and the global one, so the
  marker is `[done]` with nothing outstanding.
  **The three tallies above are M5's, not the tree's**: D12 landed on 2026-08-26, after this
  verification, and moved them to 863, 49 and 101 by adding a rule, its unit cases and the `dollars`
  fixture. They are left as the owner ran them, because a verification record is of what was run.
  - **The three suites return exactly what the shim returned on Linux**: 99, 47 and 839, the same
    numbers in the same order. That is the third milestone running where a Linux session's harness
    predicted the real MSVC binary rather than only itself, which is what makes the shim worth
    building -- but it proves nothing about `/W3`, `/sdl`, `/arch:AVX2` or the real `include/` headers,
    and those are exactly what the owner's run covers instead. The Debug run matters on its own: the
    Debug configuration is where `/RTCu` would catch an indeterminate read like the `DocFindStyle` one
    an M5 review found, and where `mzero`'s aligned 256-bit path over the three new `al32` structures
    would fault if any of them had lost its alignment.
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
  - **Verified on Linux, behaviourally, against the shim build**: the unit suite passes all **839**
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
    and M4 from expat. Everything in this bullet and the two after it was run from **scratch harnesses
    the commit does not carry**, as M3's and M4's equivalents were: 4,000 generated documents — nested
    transparent wrappers, tracked changes, `mc:AlternateContent` with and without a fallback, hidden
    runs, breaks, tabs, hyphens, and text
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
  - **Fuzzed**, again from harnesses the commit does not carry: 15,000 mutated archives and 7,500
    generated well-formed documents through the exe, on top of those 4,000 and 3,150.
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
  - **Two of the six review dimensions died of context exhaustion and were re-run split five ways**, each
    with a bounded reading list, and each finding again put to a skeptic. That round found the two
    defects that lose data. `--stdout` **reported success after a failed write**: `DiagWriteOutBytes`
    returned `void`, so a bad handle or a short `WriteFile` left the process exiting 0 having emitted
    nothing or half a document, while the file path has always deleted a half-written `.md` on exactly
    that reasoning. And **two inputs with the same leaf name silently overwrote each other**: `-o dst/`
    over `p/report.docx q/report.docx` converted both, kept the second and exited 0. It also found a
    **spec-legal 45 KB `.docx` that spins for seconds** with no output and no refusal, because
    `StyleFind` was a linear scan behind a one-entry cache: 13.53 s against a 0.08 s control of
    identical parse volume, now 0.09 s through an index and emitting the same bytes -- measured on the
    shim at `-O2`, from a harness the commit does not carry, so `bench/` is still owed at the first
    claim that one implementation simply beats another. Besides those:
    two unit checks that could not fail, a `w:styleId` over 255 bytes stored in a form the document
    could never match, a raw `unsigned long long` in an alias-only TU, five accessors missing the
    `@return` d1 requires, and eleven false statements in this file and `CHANGELOG.md` -- among them a
    fixture count, a writer count, a `mc:ProcessContent` transparency the code does not have, and
    three verification bullets missing the "harnesses the commit does not carry" disclaimer M3 and M4
    both carry.
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
  - **What a Linux session could not reach, and the owner's Windows run did**: `/W3` and its
    zero-warnings requirement, `/sdl`, `/arch:AVX2`, the real `include/` headers, and whether `mzero`
    on the three new `al32` structures behaves — the shim traps a misaligned 256-bit store rather than
    reproducing MSVC's fault, which is stricter, but it is not the same code. Two of the four defects
    M4's review fixed were reachable only on that path. All of it is now covered: both configurations
    build warning-free and every suite passes against the real binary.
- **M6 `[done]` Inline formatting** — `RunCoalescer` (merge + hoist), the remaining
  `MdEscape` callers, bold/italic/strike/code/sup/sub. DoD: fragmented-run and trailing-space-in-bold
  fixtures pass.
  **Status**: the code landed from Linux on 2026-08-26 as `[done-unverified]` over four commits, and the
  owner verified it on Windows on 2026-08-27. Both x64 configurations build with **no errors and no
  warnings**, `python tests\run_container.py` passes all **117** checks against `x64\Release` and all
  **117** again against `x64\Debug`, `python tests\run_golden.py` passes all **65**, and
  `tests\x64\Release\DOCXtoMD.Tests.exe` passes all **1058**. That discharges the milestone's own
  definition of done -- `run_golden.py` is what byte-compares the fragmented-run and
  trailing-space-in-bold fixtures -- **and the global one**, so the marker is `[done]` with nothing
  outstanding.
  The zero-warnings half was confirmed separately, the way M4's was: the first report said both
  configurations compiled, which is not the same claim, and a rebuild the same day produced no warnings
  in either. That matters past bookkeeping. M2 established that the six owner-authored `include/`
  headers come through `/W3` clean, so a warning appearing from here on belongs to the commit that
  introduces it rather than being a latent header problem -- and M6 puts `RunCoalescer`, its test suite
  and eleven changed files under that inheritance.
  - **The three tallies are the shim's, exactly.** 117, 65 and 1058, the same three numbers in the same
    order a Linux session measured before any of this reached a Windows machine. That is the fourth
    milestone running where the shim predicted the real MSVC binary rather than only itself -- and it is
    worth what it costs precisely because it proves nothing about `/W3`, `/sdl`, `/arch:AVX2` or the real
    `include/` headers, which is what the owner's run covers instead. The Debug run matters on its own:
    Debug is where `/RTCu` catches an indeterminate read, and where `mzero`'s aligned 256-bit path over
    this milestone's structures would fault if any had lost its alignment. `RunCoalescer` adds no `al32`
    structure of its own, which was the one thing that would have made that risk new at M6.
  - **The definition of done, and what discharges it**: `tests/fixtures/fragments` is the fragmented-run
    case — mid-word splits across differing rsids, with a `w:proofErr`, a `bookmarkStart`/`End` pair and
    an accepted `w:ins` between the halves, since 5.1 says merging must ignore all of them — and
    `tests/fixtures/hoisting` is the trailing-space-in-bold case, with the leading-space, whitespace-only,
    U+00A0 and tab variants beside it. Both convert byte-exact, and **every one of the seven new
    `expected.md` files was written by hand from the specification before the converter was run at it**.
    Six matched on the first run; `monodefault` was authored later, as the regression pin for a defect a
    review had just found, so it failed against the build as it stood and passed once the guard landed.
  - **What the milestone took beyond its own line, and why each.** The three escaping contexts M5 left
    caller-less are named in its own roadmap entry, and giving `MD_CONTEXT_CODE_BLOCK` a caller means a
    fenced block, which means mapping row 12; row 13's blockquote is `MdEmitter`'s To Do 2 and row 25's
    horizontal rule is `DocWalker`'s To Do 3, which `DocWalker.h` says "the To Do names it so the
    milestone cannot close without it". What is **not** taken early: no link, image, list or table, and
    `MD_CONTEXT_TABLE_CELL` and the two link contexts still have no caller, because M7 and M9 own them.
  - **Verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only,
    CRLF and ≤150 columns on all thirty-two `src/` files and all twelve `tests/unit/` ones;
    `clang-format --style=file` a verified no-op on every one of them; both `.vcxproj`/`.filters` pairs
    well-formed XML, mutually byte-identical in their `Include=` paths and every listed file on disk.
  - **Verified on Linux, behaviourally, against the shim build**: the unit suite passes all **1058**
    checks, `tests/run_golden.py` all **65** and `tests/run_container.py` all **117**, every one of them
    under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection on and no diagnostic.
  - **Cross-checked against an independent implementation**, which is what M3 got from Python's `zlib`,
    M4 from expat and M5 from `markdown-it-py`'s plain text. M6's claim is about *markup* rather than
    text, so the oracle compares the rendered inline structure: 4,000 generated documents over eight
    seeds were converted, re-parsed with `markdown-it-py` in CommonMark mode with the strikethrough
    extension, and the sequence of (text, formatting) spans it produces compared against an independent
    Python model of merge, hoist and delimiter emission. All 4,000 agree. Separately, all **960**
    ordered pairs of formatting combinations, with and without a word character on either side, round
    trip exactly, as do all **1,458** ordered triples of the same combinations and 1,500 random
    emphasis lines. All three harnesses are scratch and **the commit does not carry them**; what they
    leave behind is the fixtures they motivated.
  - **Fuzzed**, from a fourth scratch harness: 600 mutated archives and 600 generated documents through
    the sanitizer build, every archive returning a documented exit code and every document satisfying
    the emitter's own invariants — UTF-8 out, LF endings, no trailing whitespace, no blank line inside a
    block, exactly one trailing newline. No AddressSanitizer or UndefinedBehaviorSanitizer diagnostic
    anywhere, here or in the oracle, the triples or the three committed suites.
  - **What that oracle found, none of it reachable from the fixtures or the unit suite as they stood.**
    A **strikethrough that wraps another delimiter vanishes**: `word~~**x**~~` emits four literal tildes,
    because a `~~` in front of a `**` is followed by punctuation and so may only open where the
    character before it is whitespace or punctuation, and mid-sentence it is a letter; two `~~` runs
    that meet fail as completely, since `~~a~~~~b~~` is a run of four tildes GFM does not recognise.
    **Emphasis vanishes against punctuation** for the same reason — `word**(a)**after` — and so does
    `***T*****=eq=**`, where two delimiter runs meet and a parser reads the five asterisks as one run
    whose neighbour is the letter before them. And **two adjacent runs that render as the same code
    span did not merge**, because code drops bold and italic only at emission, so their two backtick
    delimiters met and a renderer read the pair as one span with backticks in it. The first two are
    fixed by falling back to `<del>`, `<strong>` and `<em>` exactly where a Markdown delimiter cannot
    parse where it stands; the third by clearing the bits in the walker, where the complex-script twins
    are already folded for the same reason. The first two are pinned by `tests/fixtures/inline`; the
    third is pinned by `tests/fixtures/code` and `TestRunCoalescer`, because it is invisible at emission
    -- clearing the bits changes nothing a reader sees, only whether the two spans merge.
  - **The fallback is session-derived, not ruled**, and it is the one place M6 writes markup the mapping
    table does not name. It is recorded as four rows in that table rather than as a decision because
    the alternative is not a policy but a defect: the delimiter it replaces does not render at all.
    An owner who wants it spelled differently — always HTML for strike, say, or never — should say so.
  - **What a second review found after that, all seven of it verified on the code before it was
    fixed.** The worst was catastrophic and silent: a **`w:docDefaults` naming a monospace family turned
    the whole document into one fenced code block**, because row 12's font heuristic asks whether every
    run is monospace and a document default makes every run monospace — a legal filing set in Courier
    converted to a single fence with every heading, emphasis and link delimiter dead inside it. The
    heuristic is now switched off by such a default rather than turned on everywhere. Beside it, a
    **whitespace-only run between two monospace runs broke the fence**, because Word splits a run at
    every rsid boundary and the space between two code runs routinely lands in the body font — the vote
    is now taken at the end of a run and only by a run that produced something that is not a space or a
    tab. **Three adjacent asterisk spans lost all three**, which is CommonMark's rule of three and is
    arithmetic no character class can express; a span abutted on both sides now takes the element form.
    A **fence measured its backtick run per span**, so a bold ` `` ` beside a plain `` ` `` sized the
    fence at three and the content's own three closed it early; the run now carries across spans and
    breaks at a line end. A **fence's edges were trimmed by byte count**, so a code paragraph of nothing
    but padding opened it; the test is now the same has-content test `IrEndBlock` uses. `IrEndBlock`'s
    own **break trim lost a blank line inside a fence**, where a break is a real newline rather than a
    marker, so `IR_BLOCK_CODE` is exempt from it. And a **vendor element inside a `w:pBdr` suppressed a
    horizontal rule**, because the other borders were found by exclusion; both halves of `CT_PBdr` are
    now matched by name, which is the OOXML compatibility model — what is not understood gets no vote.
    Each is pinned: `monodefault` and the extended `code`, `inline` and `rules` fixtures, plus unit
    cases in `TestStyleModel`, `TestDocWalker` and `TestMdEmitter`.
  - **The punctuation the test rests on is CommonMark's own definition, not an approximation of it**:
    the Unicode P and S categories, as a 338-range table generated from the character database and
    binary-searched. A first cut carried a hand-written subset, and auditing it against the database
    found it wrong in *both* directions — 326 code points it called punctuation are letters or numbers,
    and 854 below U+3100 alone that are punctuation it called letters, among them the Arabic full stop,
    the Hebrew maqaf and the Greek question mark. The generated table was then checked exhaustively:
    all **1,112,064** code points agree with Python's `unicodedata`, so an Arabic full stop and a
    Devanagari danda take the fallback while a Roman numeral, a letterlike symbol, an accented letter
    and a CJK ideograph correctly keep the Markdown spelling. Regenerating and diffing the table is how
    a reader who doubts a row checks it.
  - **What a third review found, all of it verified on the code before it was fixed, and none of it
    reachable from the suites as they stood.** Two were serious. The **monospace-baseline guard covered
    only half of what declares a baseline**: it read `w:docDefaults`, and the other half is the
    `w:default="1"` paragraph style — which is what Word's *Modify Style ▸ Normal* writes, and the
    likelier half, because Word's `w:docDefaults` normally names a *theme* slot that specifies no family
    at all. A Word-shaped `styles.xml` still converted the whole document to one fence, its headings
    becoming `` # `Chapter One` ``. The baseline is now folded from both halves, nearest-wins, and a
    *proportional* default style correctly puts the heuristic back on over a monospace `w:docDefaults`.
    And **hoisting covered three whitespace characters out of the seventeen CommonMark counts**: every
    Zs flanks alike, so a bold span beginning with U+2002 emitted `a** bold** c`, four literal asterisks
    with the formatting lost. Neither character is exotic — U+2002 is one Insert ▸ Symbol away in Word,
    U+3000 is what a CJK keyboard's space bar produces. Both the hoist set and the emitter's flanking
    classifier are the Zs category now, U+200B deliberately excluded because it is Cf.
    Beside those: a **discarded `mc:Choice` voted on the paragraph it was rewound out of**, because the
    row 12 verdict is walker state that `IR_MARK` does not carry, so a plain Choice beside an
    all-monospace Fallback demoted the surviving fence to an inline code span; and **a run contributing
    no visible character still voted**, because the abstention rule counted bytes as they arrived rather
    than as they would be emitted — a CR or LF inside a `w:t` folds to one space and a soft hyphen is
    dropped, and Word gives a hyphenation point from a later session its own `w:r`. Two GCS breaches went
    with them: `cchptr const *` written at two use sites, which is r2/t2 and breaks t3 in both TUs, now
    `cchptrcptr`; and a comment block orphaned from the function it describes.
  - **The same review found three rules that were implemented and pinned by nothing**, each confirmed by
    deleting the rule and watching the whole suite stay green: row 11's "code drops bold and italic",
    whose only observable effect is whether two spans merge, so no golden could ever see it; the
    **closing** half of the flanking test, because every fallback case in the suite was decided by the
    opening half; and the guard suppressing hoisting inside a fence, whose case drove an *unformatted*
    span and so returned before the guard was read. All three now have cases that fail without the rule.
  - **What a fourth review found, and the one methodological lesson worth keeping.** It confirmed four
    more defects and none of them was a regression: a body-font U+2002 or U+3000 between two monospace
    runs still broke the fence, because `DocIsSolid` was widened in a *different* direction from the
    other two whitespace sites and never reached the Zs class — one fence became three blocks and the
    demoted line lost its indentation; and a `w:basedOn` **across two style types** leaked the monospace
    baseline past the new guard, so a character style based on the default paragraph style turned italic
    prose into a fenced code block, with the role leaking the same way in the other direction and
    needing no monospace font at all. ISO/IEC 29500-1 17.7.4.3 requires the link to be ignored, and the
    test has to ask whether both styles **said** what they are: `w:type` is optional, an absent one
    reads as paragraph, and comparing the stored types alone drops a typeless style's link to a real
    character style — a shape producers write, and dropping it loses the code span. Beside those, the
    baseline's `w:basedOn` fold was pinned by nothing, and a comment miscounted its own class.
  - **The lesson: a claim that a rule is pinned is itself a claim that has to be run.** The previous
    round's assertion that all nine defects were pinned at both the unit and the golden level was
    **false for five of them**, and the review caught it by doing what the sentence described. Mutating
    each rule and running all three suites also found six rules that were live and covered by nothing at
    all — among them three members of the emitter's own whitespace class, and the fold above. Every rule
    either round introduced now fails under mutation; the table of which suite catches which is in the
    commit message rather than here, because it is a fact about a moment and this file is not.
  - **What a Linux session could not reach, and what the owner's Windows run then covered**: `/W3` and
    its zero-warnings requirement, `/sdl`, `/arch:AVX2`, the real `include/` headers, and whether `mzero`
    on the structures this milestone touches behaves — the shim asserts the alignment `mzero`'s 256-bit
    path needs rather than faulting the way MSVC would, which is stricter but is not the same code. All
    of it is now covered: both configurations build warning-free and every suite passes against the real
    binary.
- **M7 `[done]` Hyperlinks & images** — rels resolution, `MediaExtractor`, anchors/slugs.
  M6 left three things waiting here by name and all three are settled: `MD_CONTEXT_LINK_TEXT`,
  `MD_CONTEXT_LINK_DEST` and `MD_CONTEXT_ALT_TEXT` have callers and were re-cut against real hyperlinks
  (which changed none of them, and `MdEscape.h` records why); `RunCoalescer` no longer merges across a
  `w:hyperlink` boundary, because a link start blocks a merge while an anchor is transparent to one; and
  `MdEdgeAhead` reads the markup rather than the text beyond it, so a `[` standing between two spans is
  punctuation to the flanking test.
  **Status**: the code landed from Linux on 2026-08-27 as `[done-unverified]`, and the owner verified it
  on Windows on 2026-09-09. Both x64 configurations build with **no errors and no warnings**;
  `python tests\make_fixtures.py` builds all **67** fixtures; `python tests\run_container.py` passes all
  **125** checks against `x64\Release` and all **125** again against `x64\Debug`;
  `python tests\run_golden.py` passes all **86**; and `tests\x64\Release\DOCXtoMD.Tests.exe` passes all
  **1195**. That discharges the milestone's own definition of done -- `run_golden.py` is what
  byte-compares the `links`, `images` and `anchors` fixtures, and `check_media` is what compares the
  extracted files byte for byte -- **and the global one**, so the marker is `[done]` with nothing
  outstanding.
  **One fix landed after that verification**, the way D12 landed after M5's: an audit of this file's own
  claims found that a *muted* link still separated the two runs it stood between, so a bold run either
  side of one emitted `**A****B**` and an entity split across it went unescaped. The three tallies are
  unchanged -- the fix adds two paragraphs to an existing fixture and no new check -- so what the owner
  ran still describes the tree. The marker stays `[done]` on M5's precedent: a verification record is of
  what was run, and a later bug fix does not un-verify a milestone. The gap that left -- the changed
  `RunCoalescer`, `Convert` and `MdEmitter` never having been through `/W3` -- was closed by M8's
  Windows run on 2026-09-22, which built all three clean along with the rest of the solution.
  - **The three tallies are the shim's, exactly.** 125, 86 and 1195, the same three numbers in the same
    order a Linux session measured before any of this reached a Windows machine. That is the fifth
    milestone running where the shim predicted the real MSVC binary rather than only itself -- and it is
    worth what it costs precisely because it proves nothing about `/W3`, `/sdl`, `/arch:AVX2` or the real
    `include/` headers, which is what the owner's run covers instead. Two things this milestone made the
    Debug run matter more than usual for. `/RTCu` is what catches an indeterminate read, and M7 shipped
    one -- `MediaPlan` measuring a `--no-images` prefix `ConvertMediaDir` had short-circuited past --
    which the Linux sanitizers do not report and which a review found by reading rather than by running.
    And `/sdl` puts `/GS` on both configurations, which is what would have turned the slug counter's
    stack-buffer overflow into a `__report_gsfailure` on a heading 511 characters long. Both are fixed;
    the clean Debug run is the evidence that neither left anything behind.
  - **What was verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII
    only, CRLF and ≤150 columns on all thirty-six `src/` files and all fourteen `tests/unit/` ones;
    `clang-format --style=file` a verified no-op on every one of them; both `.vcxproj`/`.filters` pairs
    well-formed XML, mutually byte-identical in their `Include=` paths, and every listed file on disk.
  - **What was verified on Linux, behaviourally, against the shim build**: the unit suite passes all
    **1195** checks, `tests/run_golden.py` all **86** and `tests/run_container.py` all **125**, every one
    of them under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection on and no
    diagnostic. Those numbers are the shim's, and the sanitizers behind them are what it is for -- see
    the closing bullet for what each half of the pair covers that the other cannot.
  - **Cross-checked against an independent implementation**, which is what M3 got from Python's `zlib`,
    M4 from expat and M5 and M6 from `markdown-it-py`. M7's claim is about *references*, so the oracle
    compares the link and image structure `markdown-it-py` parses back out of the emitted Markdown
    against an independent Python model of the walk, the resolution and the slugger. The slugger is
    checked the harder way the punctuation table was at M6: all **1,112,064** code points agree with
    Python's `unicodedata` under the keep rule, and the keep rule itself is pinned against
    github-slugger's own removal class rather than against a guess -- see the Nd entry below. Every
    harness is scratch and **the commit does not carry one**; what they leave behind is the fixtures and
    the unit cases they motivated.
  - **What that oracle and the reviews found, none of it reachable from the fixtures as they stood.**
    One is a **stack-buffer overflow**, and it is the only defect in the milestone that is not about
    output: `LinkHeadingSlug` wrote a duplicate slug's counter into a 512-byte stack array with no bounds
    check, so a heading whose slug filled that array put the digits past the end of it -- and then handed
    the over-long length to `IrStoreDest`, which published the adjacent stack in the document. On MSVC,
    where both configurations build with `/sdl`, that is a `__report_gsfailure` rather than a silent
    corruption. Three more are about scale, and all three are M5's `StyleModel` lesson arriving again: a
    paragraph of N bookmarks between N word fragments was quadratic in the coalescer; N hyperlinks
    against N relationships was quadratic in `OpcFindRelById` -- **2.34 seconds** at 32,000 links, now
    0.10, through an index `LinkResolver` builds once; and one part lookup per picture was quadratic in
    `OpcFindPart`, which is a scan over every archive entry -- **5.10 seconds** for 100,000 pictures in a
    9,000-entry package, now 0.38, through a part-name index `OpcPackage` builds at `OpcOpen`. That last
    one fixes `OpcOpen`'s own two quadratic passes as well, and M10's footnote parts inherit it. Of the
    rest, the ones that lost a document's meaning: an `a:blip` was matched wherever it stood, so a drawn shape's *fill* was emitted as the
    figure the paragraph shows; an image inside a fenced block was dropped from the output and extracted
    to disk anyway; an exclamation mark in front of a link turned it into a broken picture (pitfall 7);
    a hard break at the edge of a link emitted `[](url)`; a generated media path did not encode `#`, `%`
    or `?`, so a document called `draft #2.docx` linked its pictures to a fragment of itself; a
    `--media-dir` ending in a separator doubled it; a heading's slug carried the padding a renderer
    strips, so `# Intro ` reached `-intro-`; and the slug keep set was the whole of category N where the
    renderer's is `Nd`, so a vulgar fraction or a Roman numeral in a heading resolved to nothing.
    `CHANGELOG.md` carries the whole list; each is pinned by a unit case, a fixture, or both.
  - **The Nd question is settled against the renderer's own class, not against a reading of it**:
    github-slugger's removal regex takes out U+00B2, U+00B3, U+00B9 and U+00BC..U+00BE -- the
    superscripts and the vulgar fractions, every one of them No -- while leaving U+00AA, U+00B5 and
    U+00BA, every one of them a letter, standing in the gaps between those ranges. Bengali says it twice
    over: the digits U+09E6..U+09EF are kept and the currency numerators U+09F4..U+09F9 beside them are
    not. Comparing that Latin-1 class against both candidate rules leaves `L | M | Nd | Pc` matching
    exactly and `L | M | N | Pc` wrong in six places.
  - **Every rule this milestone introduced was mutation-tested**, the way M6's third review established:
    each rule is deleted or inverted in turn and all three suites are run over it, and a rule no suite
    notices is a rule covered by nothing. Two rounds of that found six rules covered by nothing, and
    every one of the six now fails under mutation. What is *not* pinned is stated rather than hidden --
    see the entries under Known gaps.
  - **A second review round, run as a workflow of 105 agents over six dimensions with every finding put to
    three skeptics, raised 33 findings of which 11 survived.** Two of the eleven were already fixed by the
    round above; the rest are the stack overflow, the part-lookup quadratic, an r12 breach in a table this
    session had itself added, four prolog `To Do` items and one `@param` still describing work M7 had
    delivered or ruled the other way, and three rules live and covered by nothing. Every one is fixed or
    pinned. That a second, larger review found a memory-safety defect the first missed is the argument for
    running one: the first round's six dimensions were the same six, and the defect needs a heading 511
    characters long to reach, which no fixture and no generated document had.
  - **The mutation harness's own verdicts were then re-checked by hand, and two of them were wrong.**
    Running the set twice gave different answers for four rules: the padding trim and `--no-images`
    reported as unpinned in the second round are both caught by a suite when the mutation is applied and
    run by hand, and the fence guard reported as caught in the second round is not. The harness is a
    scratch tool and the commit does not carry it; what it is for is finding candidates, and a candidate
    it names is not a finding until the mutation has been applied and the suites run over it directly.
    That is M6's own lesson about pinning claims turned on the tool that checks them.
  - **What a Linux session could not reach, and what the owner's Windows run then covered**: `/W3` and
    its zero-warnings requirement, `/sdl` and the `/GS` cookie that turns the slug overflow into a
    fast-fail, `/RTCu` and the indeterminate read it catches, `/arch:AVX2`, the real `include/` headers,
    and whether `mzero`'s aligned 256-bit path behaves over the one `al32` structure this milestone added
    and the two it grew -- `MEDIA_SET`, `IR_DOCUMENT`'s destination arena and `OPC_PACKAGE`'s part-name
    index, each pinned by its own `static_assert`. All of it is now covered: both
    configurations build warning-free and all four commands return what the shim returned. What stays
    Linux-only is the other half of the pair, and it is not a gap in the verification but the reason for
    keeping the shim. MSVC v143 does ship `/fsanitize=address`, so the heap-use-after-free this
    milestone's `IrStore` carried is in principle reachable there; it has no UndefinedBehaviorSanitizer
    at all, so the out-of-bounds index the slug counter produced is not. Neither is switched on in
    `DOCXtoMD.vcxproj`, which is the honest statement of it: the suites run under both sanitizers on
    Linux and under neither on Windows, and turning `/fsanitize=address` on for a Debug build would be
    worth a decision of its own rather than a quiet edit.
- **M8 `[done]` Lists** — `NumberingModel` (indirection, overrides, restarts, style-borne
  numPr). DoD: the milestone names no commands of its own, so the global five apply; `tests/fixtures/lists`,
  `listcounters`, `listbroken` and `liststyles` are the fixture pairs bullet 4 asks for.
  **Status**: the code landed from Linux on 2026-09-10 as `[done-unverified]`, and the owner verified it
  on Windows on 2026-09-22. Both x64 configurations build with **zero errors and zero warnings**;
  `python tests\make_fixtures.py` builds all **71** fixtures; `python tests\run_container.py` passes all
  **133** checks against `x64\Release` and all **133** again against `x64\Debug`;
  `python tests\run_golden.py` passes all **94**; and `tests\x64\Release\DOCXtoMD.Tests.exe` passes all
  **1334**. Those six runs discharge the two global bullets no Linux session can reach: bullet 1, zero
  warnings at `/W3`, and bullet 4, where `run_golden.py` byte-compares the `lists`, `listcounters`,
  `listbroken` and `liststyles` pairs against an `expected.md` written by hand from the specification
  before the converter was run at it. Bullets 2, 3 and 5 are mechanical and were checked on Linux, so
  the marker is `[done]` with nothing outstanding.
  **Two fixes landed after that verification**, the way one did after M7's, and they are one defect in
  two places. Three readers turn a `w:val` into a number -- `StyleReadDecimal`, `DocReadDecimal` and
  `NumParseValue` -- and each capped the value's *length* rather than its magnitude, so a value padded
  with the leading zeros an `xsd:integer` allows was discarded, silently, at every call site that seeds
  its destination with -1 and ignores the result. M8 had already dropped the styles cap; the other two
  kept theirs, so direct formatting went on refusing what a style accepted. A paragraph carrying
  `<w:outlineLvl w:val="007"/>` stopped being a heading and a padded `w:ilvl` lost the depth it named;
  a padded `w:abstractNumId` left a `w:num` with no definition behind it, which 5.4 degrades to a
  bullet at every level. All three readers now agree -- the overflow test is the only bound, and
  `NumParseValue` keeps the sign branch the other two have no need of. Every one of the 71 fixtures
  converts to the same bytes either way, so the container and golden tallies stand at **133** and
  **94**; the unit suite gains ten checks and returns **1344**, measured on the shim and not on
  Windows. The marker stays `[done]` on M5's precedent: a verification record is of what was run, and
  a later bug fix does not un-verify a milestone. The gap that left -- the changed `DocWalker` and
  `NumberingModel` and their ten checks never having been through `/W3` or run on Windows -- was closed
  by M9's Windows run on 2026-09-23, which built both clean along with the rest of the solution and
  whose 1419 unit checks include all ten.
  - **The three tallies are the shim's, exactly.** 133, 94 and 1334, the same three numbers in the same
    order a Linux session measured before any of this reached a Windows machine, and the fixture count
    with them. That is the **sixth** milestone running where the shim predicted the real MSVC binary
    rather than only itself -- and it is worth what it costs precisely because it proves nothing about
    `/W3`, `/sdl`, `/arch:AVX2` or the real `include/` headers, which is what the owner's run covers
    instead. The Debug run carries its own half of that: `/RTCu` is where an indeterminate read
    surfaces, which is how M7's was caught, and Debug is where `mzero`'s aligned 256-bit path over the
    one `al32` structure M8 adds -- `NUM_MODEL`, pinned by its own `static_assert` -- would fault had
    the alignment been lost.
  - **What the milestone is, in one line**: a paragraph's `w:numPr` becomes a Markdown list item, with
    real computed numbers, the whole `w:num`/`w:abstractNum`/`w:numStyleLink` indirection behind it,
    `w:lvlOverride`/`w:startOverride`/`w:lvlRestart`, and numbering that arrives through a style chain.
    `src/NumberingModel.h`/`.cpp` is the new module and `tests/unit/TestNumberingModel.cpp` the twelfth
    suite; `Ir`, `StyleModel`, `DocWalker`, `MdEmitter` and `Convert` each gained the part of it that
    belongs to them, and no sixth block kind was added — see the `Ir` bullet for why.
  - **Verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only,
    CRLF and ≤150 columns on all thirty-eight `src/` files and all fifteen `tests/unit/` ones;
    `clang-format --style=file` a verified no-op on every one of the fifty-three; both
    `.vcxproj`/`.filters` pairs well-formed XML, mutually byte-identical in their `Include=` paths and
    order, and every listed file present on disk.
  - **Verified on Linux, behaviourally, against the shim build**: the unit suite passes all **1334**
    checks, `tests/run_golden.py` all **94** and `tests/run_container.py` all **133**, every one of them
    **twice** — plain, and under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection on,
    with no diagnostic from either.
  - **Cross-checked against an independent implementation**, which is what M3 got from Python's `zlib`,
    M4 from expat and M5 through M7 from `markdown-it-py`. M8's claim is about *numbers*, so the oracle
    generates documents with random numbering parts, converts them, re-parses the emitted Markdown with
    `markdown-it-py` and compares the list structure a reader would actually see — every item's rendered
    number, its marker kind, the order of the text and whether the nesting the document asked for was
    ever inverted — against an independent Python model of 2.9 and 5.4 written from the specification.
    Roughly **14,000** documents agree, 250 of them under both sanitizers. The harness is scratch and
    **the commit does not carry it**; what it leaves behind is the fixtures and unit cases it motivated.
  - **The emitter's own rules were settled empirically rather than reasoned about.** Every claim about
    what CommonMark does with a list — that a child indents to its parent's *content column* and not by
    a fixed step, that an ordered marker stops being one at ten digits, that `<!-- -->` splits two lists
    with no blank line around it, that a nested ordered list whose first number is not 1 cannot
    interrupt a paragraph, and that a lone `-` under a line of text is a **setext underline** — was run
    through `markdown-it-py` before a line of the emitter was written.
  - **What the oracle found, none of it reachable from the fixtures as they stood.** Two defects in the
    same shape, each losing the document's own numbers: a list that **restarts after a nested item** got
    no `<!-- -->`, because the separator asked whether the block *immediately above* was an ordered
    sibling and that block sits one level deeper — so the two lists merged and a renderer renumbered the
    second from the first one's start. And a `w:startOverride` **spent at a level other than the item's
    own** restarted that level's counter without marking the item first, so the same merge happened with
    no override at the item to point at. The fix for the second is the general fact rather than the
    special case: what makes an item the head of a list is that its counter had to be *seeded*.
  - **What a 105-agent adversarial review found on top of that**, run over seven dimensions with every
    finding verified on the code before it was fixed. Its skeptic stages died on a session limit, so
    every finding below was confirmed or refuted by hand instead, which is the only reason any of it is
    reported as settled. Three more defects lose output. A **dangling `w:numId` cancelled row 25's
    horizontal rule and row 12's font detection**, because the walker decided on the raw reference
    rather than on a resolvable one — a broken numbering graph deleted a `---` outright and demoted a
    fence to an inline code span, which is a defect in a reference losing output that has nothing to do
    with it. An **empty item that has deeper items after it was trimmed** as the artefact a user leaves
    behind on pressing Enter, but it is the *parent* those items hang from: its children were promoted
    to the outer list and the next shallower item became their sibling, so the document's `7.` reached
    the page as `8.`. And a **content-free continuation suppressed the blank line the block before it
    had earned**, because the guard read the previous record rather than the last block that emitted a
    line — two paragraphs of one item merged into one. Beside those: two consecutive marker-less code
    paragraphs in one item emitted two fences where row 12 merges them; an empty marker-less
    continuation emitted a second blank line; a refused `NumLoadBytes` left half-read definitions in the
    model against its own header, and a second load stranded the first one's index; and M8 had narrowed
    `w:outlineLvl` parsing to two digits, so a zero-padded `007` — legal in an xsd:integer — silently
    demoted a heading to body text. Every one is fixed, and every one is pinned.
  - **Every rule this milestone introduced was mutation-tested**, the way M6 established and M7 repeated:
    the rule is deleted or inverted and all three suites are run over it, and a rule no suite notices is
    a rule covered by nothing. **Every one of the nine defects above was pinned by nothing when it was
    found** — the suites were green with the bug in place, which is exactly what the technique is for —
    and each now fails at least one suite, most of them two. The mutations were applied and the suites
    run by hand rather than by a harness, which is M7's own lesson about trusting the tool that checks.
  - **What a Linux session could not reach, and what the owner's Windows run then covered**: `/W3`
    and its zero-warnings requirement, `/sdl`, `/RTCu`, `/arch:AVX2`, the real `include/` headers, and
    whether `mzero`'s aligned 256-bit path behaves over the one `al32` structure M8 adds — `NUM_MODEL`,
    pinned by its own `static_assert` like every other. All of it is now covered: both configurations
    build warning-free and all six commands return what the shim returned. The shim is stricter than
    Windows where it cannot be identical, and it was never a substitute for any of that -- what stays
    Linux-only is the other half of the pair, AddressSanitizer and UndefinedBehaviorSanitizer, neither
    of which is switched on in `DOCXtoMD.vcxproj`.
- **M9 `[done]` Tables** — grid normalization, gridSpan/vMerge policy, HTML fallback.
  DoD: the milestone names no commands of its own, so the global five apply; `tests/fixtures/tables`,
  `tablemerges`, `tablenested` and `tablecells` are the fixture pairs bullet 4 asks for.
  **Status**: the code landed from Linux on 2026-09-22 as `[done-unverified]`, and the owner verified it
  on Windows on 2026-09-23. Both x64 configurations build with **zero errors and zero warnings**;
  `python tests\make_fixtures.py` builds all **75** fixtures; `python tests\run_container.py` passes all
  **141** checks against `x64\Release` and all **141** again against `x64\Debug`;
  `python tests\run_golden.py` passes all **106**; and `tests\x64\Release\DOCXtoMD.Tests.exe` passes all
  **1419**. Those runs discharge the two global bullets no Linux session can reach: bullet 1, zero
  warnings at `/W3`, and bullet 4, where `run_golden.py` byte-compares the `tables`, `tablemerges`,
  `tablenested` and `tablecells` pairs against an `expected.md` written by hand from the specification
  before the converter was run at it. Bullets 2, 3 and 5 are mechanical and were checked on Linux, so
  the marker is `[done]` with nothing outstanding.
  - **The three tallies are the shim's, exactly.** 141, 106 and 1419, the same three numbers in the
    same order a Linux session measured before any of this reached a Windows machine, and the fixture
    count with them. That is the **seventh** milestone running where the shim predicted the real MSVC
    binary rather than only itself -- and it is worth what it costs precisely because it proves nothing
    about `/W3`, `/sdl`, `/arch:AVX2` or the real `include/` headers, which is what the owner's run
    covers instead. The Debug run carries its own half of that: `/RTCu` is where an indeterminate read
    surfaces, and Debug is where `mzero`'s aligned 256-bit path over the two `al32` structures M9 grew
    -- `IR_DOCUMENT`, which gained the table records and the align arena, and `MD_EMITTER`, which gained
    the table mode, each pinned by its own `static_assert` -- would fault had the alignment been lost.
  - **What the milestone is, in one line**: a `w:tbl` becomes a GFM pipe table, or a raw `<table>` where
    a nested table or `--tables=html-on-merge` asks for one. `src/` gained **no new module**, which is
    the first milestone since M2 that did not: a table is a shape over the blocks that already exist,
    so `Ir` grew three record arrays and a sixth block kind, `DocWalker` two dispatch levels, `MdEscape`
    a `pipes` argument and a context, `MdEmitter` the two forms, and `CliOptions` the flag.
  - **The design decision everything else follows from**: a cell's blocks are ordinary blocks in the one
    flat array, in document order. That is what leaves `RunCoalesce`, `LinkResolveRefs`,
    `LinkResolveAnchors`, `MediaPlan` and `NumAssignMarkers` **unchanged** — each reads one array in
    reading order, and a paragraph in a cell is a paragraph. The cost is two rules the header of `Ir.h`
    states and this file repeats: a table's rows and a row's cells are chains rather than ranges, because
    a nested table interleaves them; and `IrDropEmptyBlocks` never looks inside a table, moving one whole
    and shifting its records by a single delta.
  - **What the roadmap asked for and what came of it.** `MD_CONTEXT_TABLE_CELL` got its caller and
    re-cutting it against real tables changed **nothing in it** — the same result M7 had with link text
    and alt text. What re-cutting *did* find is that being in a cell is not a context at all but a fact
    that composes with every context, which is why the pipe is now an argument. `RunCoalescer` needed no
    change, for the reason above. And `DocWalker`'s saved-and-restored paragraph classification is
    **still exercised by nothing**, which is worth saying plainly rather than claiming M9 closed it: a
    `w:tbl` is a sibling of a paragraph and never a child of one, so a cell's paragraphs are walked with
    no outer paragraph open. The save is right; the case it guards remains hypothetical.
  - **Verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only,
    CRLF and ≤150 columns on all thirty-eight `src/` files and all fifteen `tests/unit/` ones;
    `clang-format --style=file` a verified no-op on every one of the fifty-three; both
    `.vcxproj`/`.filters` pairs well-formed XML, mutually byte-identical and every listed file on disk
    — and neither needed a line changed, because M9 added no file. `USAGE_TEXT` was diffed byte for byte
    against the Target CLI block above, which gained the `--tables` line.
  - **Verified on Linux, behaviourally, against the shim build**: the unit suite passes all **1419**
    checks, `tests/run_golden.py` all **106** and `tests/run_container.py` all **141**, every one of them
    **twice** — plain, and under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection on,
    with no diagnostic from either.
  - **The emitter's own rules were settled empirically**, the way M8's list rules were. Every claim about
    what GFM does with a pipe table — that the delimiter row must hold exactly as many cells as the
    header or the whole thing is a paragraph, that a one-row table is legal, that a row with too few
    cells is padded and one with too many is truncated, that `\|` is the one escape that works inside a
    code span, that a table absorbs the paragraph after it unless a blank line intervenes, and that a
    raw-HTML block passes every byte through unparsed so `**bold**` in a `<td>` stays two asterisks —
    was run through `markdown-it-py` before a line of the emitter was written.
  - **Cross-checked against an independent implementation**, which is what M3 got from Python's `zlib`,
    M4 from expat and M5 through M8 from `markdown-it-py`. M9's claim is about a *grid*, so there are two
    oracles. The first generates random tables with random spans, converts them, re-parses the emitted
    Markdown with `markdown-it-py` and compares the table a reader would see — one row per `w:tr`, every
    row exactly as wide as the delimiter row, each cell's text where its column starts — against an
    independent Python model: **2,500** documents agree. The second is for the raw-HTML form, where the
    property is stronger and simpler: every square of the R×C grid claimed by exactly one `<td>` or
    `<th>` once its `colspan` and `rowspan` are honoured. **3,000** documents agree. Both harnesses are
    scratch and **the commit does not carry them**; what they leave behind is the defect below and the
    unit case that pins it.
  - **What the second oracle found, which no fixture would have.** A `w:vMerge` continuation whose merge
    nothing above it still covers — a producer writes one when an intervening row spans across the
    column the merge was opened in — was **dropped** from the raw-HTML form, leaving that row a column
    short. A silently narrower row is the one failure mapping row 19 names by saying a row must never
    lose a column. It is an ordinary empty cell now, and `TestMdEmitter` pins it.
  - **A 105-agent adversarial review over seven dimensions raised 25 findings; two survived three
    skeptics each, and both are fixed.** The first is the one that matters, and it is worth recording
    that *this session refuted it first and was wrong*: the raw-HTML form emitted a table one column
    wider than the pipe form of the same document. The shape is a `w:vMerge` restart **wider than the
    continuation below it** — `MdRowSpanOf` counted the run at the restart's first column alone while
    `held[]` was stamped across its whole span, so the restart claimed columns nothing continued, and
    the browser's own grid algorithm then pushed the next ordinary cell of that row past them. A
    rowspan can only promise a rectangle, so it is now written only where every column the restart
    covers is continued. The refutation failed because it reasoned from an invariant — "a rowspan is
    exactly the run of continuation cells below it" — that holds only while the restart and its
    continuation are the same width; two skeptics reproduced it by building the document. The second
    survivor is the `MdRowSpanOf` quadratic below, which was already fixed by then.
  - **The grid oracle's generator was the reason it missed that**, and this is the lesson of the round:
    the invariant was right and the documents were too narrow. It put a `w:vMerge` restart only on a
    cell of one column, so a restart wider than its continuation could not arise. Widened to put one on
    a spanning cell, it reproduces the defect in **4 of 400** documents and finds **0 mismatches in
    3,000** against the fix — while all three committed suites stay green over the bug, which is why
    `TestMdEmitter` now carries the ragged case and its matching-span twin.
  - **A hostile-input review found a denial of service, measured and fixed.** `MdRowSpanOf` rescanned
    every row's whole cell chain for every `w:vMerge` restart in the row above, and nothing caps how
    many cells a row may hold, so the cost was quadratic in the document's own cell count: 64,000
    restarts over 64,000 continuations is a **21 KB `.docx` that took 15.76 seconds**, and the
    archive's caps leave room for a file that would take hours. One nested table is the whole entry
    fee, since it forces the raw-HTML form unconditionally. Two bounds fix it — the inner walk stops at
    the first column no continuation claims and never looks past the restart's own end, and the caller
    does not ask at all for a cell outside the grid — and the same file now takes **0.12 seconds**,
    scaling linearly.
  - **Three more defects came out of that review, each reproduced before it was fixed.**
    `IR_TABLE_NESTED` **survived a rewind**: a nested table marked its parent as it closed, and
    `IrRewind` restores counters but not flags, so a table whose only nested table an `mc:Fallback`
    discarded was emitted as raw HTML it did not need. It is derived in `IrEndTable` from the blocks
    the surviving cells hold, which is what the header always said the design intended — and
    `IrMarkTable` had no caller left, so it is gone. `context->justify` was **not restored on a
    rewind** either: a cell's alignment latches on the first `w:jc` that names an alignment, so one
    inside a discarded `mc:Choice` settled the column and the surviving branch's own `w:jc` was ignored,
    which reaches the delimiter row and aligns the whole column by a branch that was thrown away.
    `context->pendingCount` goes back with it, and that one has been wrong since M7: a
    `w:bookmarkStart` in a discarded Choice was flushed into the Fallback's first block.
  - **A trailing `<br>` in a cell was trimmed in one table form and not the other**, found by testing
    the two against each other rather than by reading. A break at the end of a cell has no next line to
    start, so one with nothing after it is dropped — but the pipe form assembles a cell in the line
    buffer and the raw-HTML form writes it straight to the output, and only the first trimmed.
    `<th>a<br>   </th>` is a blank line inside a cell the pipe form of the same document does not have.
    One function now does it for both, over whichever buffer holds the cell.
  - **96 hostile table documents** — nesting past the cap and far past the tokenizer's, `w:gridSpan` at
    and past the edges of both `si32` and `ui32`, a `w:tblGrid` of 200,000 columns, 20,000 cells in one
    row, orphaned and runaway `w:vMerge`, empty tables, rows and cells, tables inside discarded
    `mc:Choice`s, 5,000-row and 3,000-table documents, and every one of them under both table forms —
    run under AddressSanitizer and UndefinedBehaviorSanitizer with **no diagnostic, no hang and no
    undocumented exit code**. The depth cap is exact: a table nested twelve deep keeps its content and
    one nested thirteen deep is dropped.
  - **Every rule M9 introduced was mutation-tested**, the way M6 established and M7 and M8 repeated,
    and the two the battery found unpinned are now pinned: the trailing-`<br>` trim, in both forms, and
    the ragged merge above. Two mutations are *deliberately* left surviving and are recorded under Known
    gaps rather than papered over — the inter-cell gap loop in each table form, `MdEmitPipeRow`'s and
    `MdEmitTableHtml`'s, which no input this build reads can reach.
- **M10 `[done]` Fields, notes, tracked changes** — field state machine, footnotes/endnotes,
  sdt, accept-all revisions. Two things M9 left for it by name: a second part's relationships, which is
  what tests that relationship ids are scoped per part (M4's coverage gap), and a multi-block note body,
  which M9 expected to be the first content to exercise `DocWalker`'s saved paragraph classification --
  a table did not, because a table is never inside a paragraph.
  DoD: the milestone names no commands of its own, so the global five apply; `tests/fixtures/fields`,
  `toc`, `footnotes` and `revisions` are the fixture pairs bullet 4 asks for.
  **Status**: the code landed from Linux on 2026-09-23 as `[done-unverified]`, and the owner verified it
  on Windows the same day. Both x64 configurations build with **zero errors and zero warnings**;
  `python tests\make_fixtures.py` builds all **83** fixtures; `python tests\run_container.py` passes all
  **157** checks against `x64\Release` and all **157** again against `x64\Debug`;
  `python tests\run_golden.py` passes all **118**; and `tests\x64\Release\DOCXtoMD.Tests.exe` passes all
  **1518**. Those runs discharge the two global bullets no Linux session can reach: bullet 1, zero
  warnings at `/W3`, and bullet 4, where `run_golden.py` byte-compares the `fields`, `toc`, `footnotes`
  and `revisions` pairs against an `expected.md` written by hand from the specification before the
  converter was run at it. Bullets 2, 3 and 5 are mechanical and were checked on Linux, so the marker is
  `[done]` with nothing outstanding.
  - **The three tallies are the shim's, exactly.** 157, 118 and 1518, the same three numbers in the
    same order a Linux session measured before any of this reached a Windows machine, and the fixture
    count with them. That is the **eighth** milestone running where the shim predicted the real MSVC
    binary rather than only itself -- and it is worth what it costs precisely because it proves nothing
    about `/W3`, `/sdl`, `/arch:AVX2` or the real `include/` headers, which is what the owner's run
    covers instead. The Debug run carries its own half of that: `/RTCu` is where an indeterminate read
    surfaces, and Debug is where `mzero`'s aligned 256-bit path over the two `al32` structures M10 grew
    -- `IR_DOCUMENT`, which gained the note records, and `MD_EMITTER`, which gained the base and the
    marker, each pinned by its own `static_assert` -- would fault had the alignment been lost.
  - **What the milestone is, in one line**: fields run through a state machine and show what they
    showed, a TOC vanishes, footnotes and endnotes become `[^n]` references and definitions numbered as
    GitHub numbers them, and accept-all reaches the last two revisions that are not wrappers -- a deleted
    paragraph mark and a deleted cell, beside the deleted row M9 already dropped. `src/` gained **no new
    module**, for the second milestone running: `Ir` grew a span kind and a record array, `DocWalker`
    the field machine, the join and the notes walk, `LinkResolver` per-part resolution and the note
    labels, `MdEmitter` a base every line start writes, and `Convert` the notes stage.
  - **The two things M9 left for it by name.** The second part's relationships are loaded, and
    `tests/fixtures/footnotes` gives `rId5` and `rId2` different meanings in `document.xml.rels` and
    `footnotes.xml.rels`, so **M4's coverage gap is closed** -- at the golden level; resolving every
    block against the body's part fails three golden checks. The multi-block note body is there too, but
    it does **not** exercise the saved paragraph classification, and the Known gaps entry on the saved
    classification says why: a note is walked after the body rather than at its reference, so no
    paragraph is open when one is.
  - **Verified on Linux, mechanically**: the r17 prolog regexes, 3-space indent, no tabs, ASCII only,
    CRLF and ≤150 columns on all thirty-eight `src/` files and all fifteen `tests/unit/` ones;
    `clang-format --style=file` a verified no-op on every one of the fifty-three; both
    `.vcxproj`/`.filters` pairs untouched, because M10 added no source or header file; every changed
    file's `Last Modified` bumped. The four new fixture trees are LF and ASCII like every other.
  - **Verified on Linux, behaviourally, against the shim build**: the unit suite passes all **1518**
    checks, `tests/run_golden.py` all **118** and `tests/run_container.py` all **157**, every one of them
    **twice** -- plain, and under AddressSanitizer and UndefinedBehaviorSanitizer with leak detection on,
    with no diagnostic from either. The four goldens' `expected.md` files were written by hand from the
    specification before the converter was run at them, and all four matched on their first run.
  - **Cross-checked against an independent implementation**, which is what M3 got from Python's `zlib`,
    M4 from expat and M5 through M9 from `markdown-it-py`. M10's claim is about what a reader sees, so the
    oracle generates documents with random complex and simple fields -- links, TOCs and plain ones,
    nested, split across runs and across paragraphs -- tracked insertions, deletions and deleted marks,
    hidden runs, hyperlinks, and footnotes and endnotes referencing each other, converts them, renders
    the Markdown with **cmark-gfm**, GitHub's own renderer, with footnotes on, and compares every
    paragraph's text, every link's destination, every reference's label *and the number cmark-gfm
    displays for it*, and every definition's content, against an independent Python model of rule 7,
    5.11 and row 24. **5,500** documents agree, 500 of them under both sanitizers. The harness is scratch
    and **the commit does not carry it**.
  - **What a hostile-input pass found**, which no fixture and no generated document had: a **nested
    raw-HTML table inside a note** broke the note. The line a nested table's `</table>` leaves the rest
    of its cell on was written without the table's prefix, and inside a definition the prefix is the four
    columns that keep a line in it -- so the definition ended there, the rest of the table landed in the
    body and the note's next paragraph became an indented code block. Every line of a raw-HTML table
    takes the prefix now, and two `TestMdEmitter` cases pin both shapes. The whole hostile set -- 25
    shapes, that one among them --
    20,000 unclosed begins, 5,000 nested links, 20,000 stray ends and separates, 200 KB instructions,
    10,000 references to one note and 20,000 to distinct ones, self-citing and chained notes, notes in
    nested tables, 20,000 deleted marks in a row, 500 deleted cells, a missing note relationships part --
    and 800 mutated archives of the four fixtures ran under both sanitizers with **no diagnostic, no
    undocumented exit code and none slower than 0.4 seconds**.
  - **Every rule M10 introduced was mutation-tested**, the way M6 established and every milestone since
    has repeated. **Sixty-one** mutations over the field machine, the join, the notes walk, the numbering,
    the IR, the emitter and the pipeline, and **fifty-nine** now fail at least one suite -- thirty-three the unit
    suite alone, one the golden runner alone (per-part resolution, which no unit suite can build a package
    for) and the rest two or three of them. They did not all fail at first: eleven survived, and nine of
    those were rules covered by nothing, each now pinned -- a switch's argument written in front of the
    target, in both groups of switches that take one; a note's pending join ending with the note; the
    adopted began-inside-a-TOC moment; a childless paragraph taking the join; a separator's `w:id`
    referenced; a bookmark after a note's last paragraph; an unreferenced notes part that must not even be
    validated; and a link an `mc:Choice` left open, undone with it. The two left are the defensive guards
    recorded under Known gaps. The mutations were applied, and the suites run over them, by a
    scratch driver, and the two survivors were then checked by hand against six shapes built to reach
    them and 1,500 generated documents, which is M7's lesson about trusting the tool that checks.
  - **What a Linux session could not reach, and what the owner's Windows run then covered**: `/W3` and
    its zero-warnings requirement, `/sdl`, `/RTCu`, `/arch:AVX2`, the real `include/` headers, and
    `mzero`'s aligned 256-bit path over the two `al32` structures M10 grew. All of it is now covered:
    both configurations build warning-free and every suite returns what the shim returned. What stays
    Linux-only is the other half of the pair, AddressSanitizer and UndefinedBehaviorSanitizer, neither
    of which is switched on in `DOCXtoMD.vcxproj`.
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
raised them: the owner accepted all four session recommendations as written. **D12 was raised by M5 on 2026-08-25
and ruled on 2026-08-26**, the owner again accepting the recommendation as written. **D13 was raised by M7 on
2026-08-27 and is `Open — owner call`**: the code implements the recommendation meanwhile, because a milestone cannot
ship without doing *something*, and the row says exactly what would change if the owner rules the other way. Keep the
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
| D12 | GitHub renders `$...$` and `$$...$$` as LaTeX math, and has since 2022. `docs/CONVERSION_REFERENCE.md` 4.1 predates that and does not list `$` among the characters to escape, so today a paragraph reading `costs $5 and $10` is emitted verbatim and github.com renders `5 and ` in math font, losing both dollar signs. Should `$` join the unconditional inline escape set, join it conditionally (only where a closing `$` could pair with it), or stay unescaped? Note the cost of each: unconditional puts a backslash in front of every price in every document, conditional needs a lookahead the line-assembly pass can do but the reference does not describe, and leaving it corrupts a real and common shape on the one renderer this converter names in its own mapping table. The same question reaches `docs/CONVERSION_REFERENCE.md`, which would gain the row either way. | **Escape `$` conditionally**, adopting the session recommendation in full; ruled 2026-08-26. Unconditional escaping is the safe direction but it is visible on every ordinary document, and math is not a CommonMark feature -- it is one renderer's extension, so paying for it everywhere is out of proportion. *(Consequence, session-derived: "conditionally" is implemented as **at most one unescaped `$` per assembled line** -- a line holding two or more has every one of them escaped, a line holding one keeps it bare. A span needs two delimiters under every renderer's reading, so a count is safe without reproducing GitHub's exact opener and closer conditions, which this project cannot verify. All-or-none was preferred over leaving one bare per line because it also narrows the one residual: a line that pairs internally contributes no live dollar to the next line.)* | **done** (the rule, the reference row and `tests/fixtures/dollars` landed 2026-08-26, after M5's verification) |
| D13 | `--stdout` and the media files. `--stdout` is single-input only (D7d) and writes the document to a pipe; M7 gives a document pictures, which are files and cannot go down a pipe. Three readings are available. **Extract anyway**, into the media directory beside where the `.md` *would* have gone, so the piped document and a written one are the same bytes and the pictures are on disk for whatever consumes the pipe. **Extract nothing**, on the reading that `--stdout` means "write no files", which makes the piped document name pictures that do not exist unless the reader also passes `--no-images`. **Refuse the combination**, which is the strictest and costs the shell pipeline that wants both. Note what the second and third cost beyond the obvious: `tests/run_golden.py` converts every fixture twice, once to a file and once through `--stdout`, and byte-compares both against one `expected.md` -- that is the check that has caught a `--stdout`-only defect before, and either of them ends it. | **Recommendation (not yet ruled): extract anyway.** `--stdout` is about where the *document* goes, and the media directory is derived from `-o` or from the input either way, so nothing about it is ambiguous. It is also the only reading under which the two output paths produce the same document, which is the property the golden runner exists to prove. The strict direction stays open: extract-anyway to refuse is a change a user notices, but so is every other pair, and no producer or consumer has a stake in this one yet. | **Implemented as recommended at M7**, and `tests/run_golden.py` compares the two paths byte for byte. If the owner rules otherwise, the change is in `ConvertFile` alone -- the pipeline below it does not know which path it is on. |

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
