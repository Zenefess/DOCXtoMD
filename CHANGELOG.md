# Changelog

All notable changes to DOCXtoMD are recorded here, per GCS c2. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) with the c3 grouping —
Added / Changed / Fixed / Removed / Perf. Nothing has been released yet, so every entry
sits under `[Unreleased]`. File prologs carry no history (GCS c1); this file is the history.

## [Unreleased]

### Added
- **M7, hyperlinks and images.** The converter now resolves references. `src/LinkResolver.h`/`.cpp` and
  `src/MediaExtractor.h`/`.cpp` are two new modules; `LinkResolver` turns a relationship id into a
  destination and a bookmark into a GFM anchor, and `MediaExtractor` writes the image parts the document
  draws from beside the `.md`. `LinkResolver` is not in the architecture list and is a session addition,
  for the same reason `Ir.cpp` and `Convert` are: heading slugs are numbered over the whole document, so
  the pass needs to see all of it at once, which neither the streaming walker nor the per-block emitter
  can do. `MediaExtractor` is the list's stage [11] as written.
- External hyperlinks (mapping row 21): `w:hyperlink r:id` becomes `[text](url)` with the formatting
  inside the brackets, which is CONVERSION_REFERENCE 5.6. A `w:hyperlink` carrying both an `r:id` and a
  `w:anchor` is a link into another document and the anchor becomes the fragment of what the
  relationship resolves to. A link to a part inside the package keeps its text and loses its brackets,
  because Markdown can address a file and not a part; so does a link whose relationship is dangling, and
  one whose content is empty, which is 5.6's skipped hyperlink.
- Internal hyperlinks and bookmark anchors (row 22). A bookmark that sits in a heading resolves to that
  heading's own GFM slug, so the link needs no markup at all; one that sits anywhere else resolves to
  its own sanitised name and is emitted as an `<a id>` element where it stands. An anchor nothing points
  at is muted rather than emitted, which is what keeps Word's generated bookmarks -- `_GoBack` in every
  saved document, a `_Toc` target in every heading -- out of the output without the code having to know
  their names; a paragraph that held nothing else goes with it.
- **The slugger is generated from the Unicode character database, not guessed**, for the reason M6
  recorded about its punctuation table one milestone earlier. The anchor a link points at is the
  *renderer's* to generate: we write `#dont-panic` and GitHub writes the heading's own id, so a slug
  that differs by one apostrophe is a link that scrolls nowhere. github-slugger's rule is lower case,
  then everything that is not a letter, a mark, a decimal digit or connector punctuation removed, then
  each space to a hyphen -- so both halves need the database, the lower-casing as much as the keep set,
  or a Cyrillic or a Greek heading reaches a fragment that does not exist. A decimal digit and not every
  number: github-slugger's own removal class settles that inside Latin-1, where it removes the
  superscripts and the vulgar fractions -- every one of them category No -- while leaving the feminine
  ordinal, the micro sign and the masculine ordinal standing in the gaps between those ranges. Two
  tables carry it: 753 ranges of code points a slug keeps, and 181 runs of simple lower-case mappings.
  All **1,112,064** code points agree with Python's `unicodedata`. A heading's leading and trailing
  padding is dropped before any of it, because an ATX heading's content is its line stripped of the
  whitespace at both ends and a renderer never sees the padding a producer left there. Duplicate slugs
  are numbered by github-slugger's own loop rather than by a counter, because a heading may literally be
  called "Introduction 1".
- Images (row 23), in all four shapes one arrives in: `w:drawing`, `w:pict`, `w:object`, and an
  `mc:AlternateContent` standing in for one. One scan serves all four, because a picture is identified
  by the markers inside it rather than by the element it arrived in, and both markup families are looked
  for at once -- which is what makes `mc:AlternateContent` right here with no branch selector: its
  `mc:Choice` and its `mc:Fallback` describe the *same* picture in two vocabularies, so reading both and
  keeping the first reference emits it exactly once. That closes the double-emit 5.8 warns about by
  arithmetic rather than by understanding the branches. A container holding no picture reference at all
  -- a chart, a SmartArt diagram, a drawn shape -- comes to nothing, because none of them has a bitmap
  the document could show. Alt text is `wp:docPr`'s `descr`, then `title`, then `name` (2.6), with every
  line end folded to one space.
- Media extraction. Each distinct part becomes `imageN.ext` in the order the document first draws it,
  and a part drawn twice keeps one file (5.8). The extension comes from `[Content_Types].xml` and never
  from the entry name, which producers get wrong -- the fixture pins a part named `mystery.png` typed
  `image/jpeg`, which is the Google Docs trap 1.2 names. No archive entry name ever reaches disk, which
  is correctness rule 10's other half. The files are written *after* the document, so a conversion that
  fails before that point leaves nothing behind, and a half-written picture is deleted the way a
  half-written `.md` is. The other order is not promised and says so in the code: a media directory that
  cannot be created leaves the document beside a picture it names and does not have, which is the right
  way round -- the text is what the conversion was for -- and the run still reports a failure.
  `--media-dir` and `--no-images` are consumed for the first time: M2 parsed both and nothing read them.
- A second arena on the intermediate representation, for destinations and anchor names. That is
  load-bearing rather than tidy. Every text span of a block lies end to end in the text arena, which is
  the invariant `RunCoalescer` merges on; a destination written between two runs would put a gap in the
  middle of it, and a bookmark -- which 5.1 says a merge must see straight through, because Word writes
  `_GoBack` in the middle of a paragraph -- would then stop the two halves of a word ever coming back
  together. `IR_SPAN` grows from sixteen bytes to twenty-four.
- `XML_NS_O` for the Office drawing extensions, which is where VML carries its `o:title` alt text.
- Three golden fixture cases -- `links`, `images` and `anchors` -- plus `media-binary`, whose one media
  part holds every byte value so that the byte path is proved rather than assumed. Every `expected.md`
  was written by hand from the specification before the converter was run at it; `links` and `images`
  matched on the first run and `anchors` did not, which is how `[nowhere]()` -- what a link to a
  bookmark the document does not define emitted -- was found. Two new unit suites, `TestLinkResolver`
  and `TestMediaExtractor`, and new cases in `TestDocWalker`, `TestRunCoalescer`, `TestMdEmitter` and
  `TestMdEscape`. `tests/run_golden.py` gains a media table and the `--no-images` and `--media-dir`
  checks; the three tallies move to **125**, **85** and **1164**.
- **M6, inline formatting.** The converter now emits delimiters. `src/RunCoalescer.h`/`.cpp` is the new
  module the architecture list has been holding a place for: it merges adjacent spans carrying equal
  formatting (CONVERSION_REFERENCE 5.1) and then hoists leading and trailing whitespace out of every
  formatted span (5.3), in that order and not the other -- merged first, `**one ** **two**` is one bold
  span reading `**one two**`, and hoisted first it would come apart. A span left holding nothing but
  whitespace loses its formatting rather than its bytes, which makes 5.5's "never emit delimiters around
  empty content" structural rather than a test the emitter has to remember. The merge is a length
  extension over the arena and never moves a byte, and it checks that the two ranges really do meet
  rather than trusting that they always will.
- Bold, italic, strikethrough, superscript, subscript and inline code, nested in one fixed order:
  `<sup>`/`<sub>`, then the strikethrough, then the emphasis, then a code span. Bold and italic together
  are `***` (mapping row 5), and code drops bold and italic (row 11's own ruling on that collision)
  while keeping the strikethrough and the vertical alignment, which wrap a code span perfectly well.
  A code span's backtick run is longer than the longest run inside it and pads with one space when the
  content begins or ends with a backtick, which is the only way a literal backtick can survive.
- **The three escaping contexts M6 owed a caller.** `MD_CONTEXT_HTML` is what a superscript, a subscript
  and every HTML fallback below write through; `MD_CONTEXT_CODE_SPAN` is what a code span writes
  through; and `MD_CONTEXT_CODE_BLOCK` is what a fenced block writes through, so correctness rule 6's
  "walker code never concatenates raw strings into output" holds for the literal contexts too.
- Blockquotes (mapping row 13): a paragraph whose style chain is named Quote, Intense Quote, Block Text
  or LibreOffice's Quotations becomes a block whose every line carries a `> ` prefix, the continuation
  line after a hard break included. Two consecutive quote paragraphs are separated by a bare `>` rather
  than a blank line, because a blank line closes the blockquote and opens a second one -- the single
  exception to the one-blank-line-between-blocks rule, and it is still exactly one line. The row's
  `w:ind >= 720` heuristic is deliberately not implemented; the reference has it off by default.
- Fenced code blocks (row 12), by either detection: a paragraph style named HTML Preformatted,
  Preformatted Text, Source Code or Code, or a paragraph whose every text-bearing run is set in one of
  row 11's monospace families. Consecutive code paragraphs merge into one fence, whose length is longer
  than the longest backtick run anywhere inside it; there is no info string, because the language is
  never recoverable. An empty code paragraph is a blank line of the fence and is trimmed only where it
  falls at either end of one, which is the one place `IrEndBlock`'s emptiness test is now suspended --
  and its break trim with it, because inside a fence a break is a real newline rather than a marker.
- The horizontal rule of row 25: a lone `w:pBdr` bottom or between border on a paragraph that came to
  nothing becomes `---`. "Lone" is enforced at both ends -- a paragraph wearing a box is not a rule, and
  neither is a bordered paragraph that has text in it -- and "came to nothing" covers a paragraph of
  whitespace as well as one with no runs at all.
- `StyleModel` gains the quote and code roles and the monospace verdict. A role now depends on what kind
  of style declares the name, which is load-bearing rather than tidy: "Source Text" is LibreOffice's
  *character* style for inline code (row 11) and an ordinary paragraph style name otherwise, and
  `tests/fixtures/headings` has carried a paragraph style called exactly that since M5. `w:rFonts` is
  read for its `w:ascii` alone and reduced to a tri-state at parse time, so the model stores no font
  names; a `w:rFonts` naming only a theme slot specifies nothing rather than specifying false.
- Seven golden fixture cases, each pinning a decision that would otherwise be unpinned: `fragments`
  (mid-word run splits across rsids, a proofErr, a bookmark and an accepted insertion), `hoisting`
  (a trailing space inside bold, a leading one, a whitespace-only span, U+00A0 and a tab), `inline`
  (every delimiter and combination, and the fallbacks below), `code` (both code detections, backtick
  collisions, two fences and the blank line that does *not* separate them), `quotes`, `rules`, and
  `monodefault`, whose `w:docDefaults` names Courier and which must not become one fence. The two the
  milestone names by name are the first two. Every `expected.md` was written by hand from the
  specification before the converter was run at it; six matched on the first run, and `monodefault`
  was authored afterwards as a regression pin, so it failed until the guard below landed.
- `tests/unit/TestRunCoalescer.cpp`, and the trace notation the other suites already use extended with
  `c` for a code span and `Q`, `C` and `R` for the three new block kinds.
- Rules that were implemented but pinned by nothing now have tests, each verified by deleting the rule
  and watching the new case fail. Row 11's "code drops bold and italic", whose only observable effect is
  whether two spans merge, so no golden could see it; the **closing** half of the flanking test, which
  every existing fallback case reached the opening half of instead; and the guard that suppresses
  hoisting inside a fence, whose case drove an *unformatted* span and so returned before the guard was
  read. Then a second sweep, which mutated every rule the first round had touched and ran all three
  suites against each: it found six more that were live and covered by nothing -- `DocIsSolid`'s CR and
  tab, four of the six members of the emitter's flanking whitespace class, and `StyleReadBaseline`'s
  `w:basedOn` fold, whose absence restores the whole-document fence verbatim because every
  `w:default="1"` style in the repository named its font directly rather than inheriting it.
  `tests/fixtures/monostyle` now inherits it through a parent, so the fold is pinned at golden level for
  nothing.

### Changed
- **`RunCoalescer` no longer merges across a link's brackets, and no rule in it says so.** 5.1 asks that
  runs be coalesced *within* a hyperlink; M7's link markers are spans, so the two text spans on either
  side of one are simply not adjacent, and the only thing the pass ever merges is a text span with the
  text span immediately before it. An image and an anchor separate two runs for the same reason -- and
  an anchor must not, which is why it is the one kind a merge reads straight through. The merge target
  is carried rather than searched for: anchors pile up behind the span every later run merges into, so
  stepping back over them would be quadratic in a paragraph's own length, which a paragraph of 32,000
  bookmarks between 32,000 fragments of one word showed at one second against nine hundredths.
- `MdEdgeAhead` and `MdFormatAhead` were re-cut, which M7's roadmap entry asks for by name. Both used to
  read past a non-text span to the text behind it, which was right while every neighbour *was* text;
  now a link start writes `[`, a link end `]`, an image `!` and an anchor `<`, and every one of those is
  punctuation, so reading past one would report a letter where a bracket stands. A delimiter run cannot
  merge with one on the far side of a bracket either, so anything but text ahead is reported as no
  formatting at all.
- `MD_CONTEXT_LINK_TEXT`, `MD_CONTEXT_LINK_DEST` and `MD_CONTEXT_ALT_TEXT` have callers, which is the
  other thing M7's entry asks for by name, and re-cutting them against real hyperlinks changed none of
  them: what 4.1 asks of link text and alt text beyond the inline set is that a closing bracket may not
  appear unescaped, and the inline set escapes both brackets unconditionally already. One thing about
  the destination rule is now recorded rather than left to be discovered: a byte above ASCII is left as
  it stands, because CommonMark takes a raw UTF-8 destination and every renderer encodes it itself.
  `MD_CONTEXT_TABLE_CELL` is the one context still waiting, and M9 is expected to re-cut it the same way.
- `ConvertPackage` runs four passes between the coalescer and the emitter, in the one order that works:
  references resolve against the part they were read in, anchors resolve once every reference is a
  destination (a heading's slug is numbered over the whole document), the media plan turns a part name
  into a path and can turn a picture back into its alt text, and dropping the emptied blocks last is
  what restores the invariant the emitter rests on -- that every block it is handed produces a byte.
- One existing golden changes: `wrappers`, whose hyperlink was plain text at M6 and is now a link. Every
  other case is byte-identical, which is what says the second arena, the re-cut lookahead and the four
  new passes cost nothing they should not.
- `MdEscapeMeasure` and `MdEscapeWrite` take the D12 dollar verdict as an argument rather than counting
  it themselves, and `MdEscapeCountDollars` is what a caller counts with. This is the obligation D12
  left for M6 in writing: a line is escaped span by span now, because there is markup between the spans,
  and a per-span count would read `costs $5` and `and $10` as two runs of one dollar each and escape
  neither -- restoring exactly the corruption the ruling was made to fix. The emitter counts over the
  whole assembled line and passes one verdict down to every span of it. A dollar inside a code span does
  not count towards it: it cannot be escaped there, and 4.1 records that it is inert.
- A line is assembled in its output form -- delimiters and escaped text together -- rather than raw and
  escaped in one piece. The ampersand lookahead is safe within a span because the coalescer has already
  merged every adjacent pair with equal formatting, so a split entity can only be separated by markup
  that stops it being one; `tests/fixtures/textflow` pins that its bytes did not change.
- Three existing goldens change, all for the same reason: `minimal` (and the fourteen container fixtures
  that compare against it) and `relocated` gain `**bold**` and `**fails**`, and `nostyles` gains
  `***...***`. Every other case is byte-identical, which is what says the escaping restructure and the
  type-qualified style roles cost nothing they should not.

### Fixed
- **A document with tens of thousands of hyperlinks no longer spends minutes resolving them.**
  `OpcFindRelById` is a scan, which is the right shape for the handful of lookups every milestone before
  this one made; M7 makes one per hyperlink and one per picture against a part that declares one
  relationship for each, so the pair is quadratic. A generated document of 32,000 external links took
  **2.34 seconds** where 8,000 took 0.17 -- on a 280 KB file producing under a megabyte of Markdown --
  and it is M5's `StyleModel` lesson arriving one milestone on. `LinkResolver` now builds an
  open-addressed index over the one part's relationship ids and looks up in constant time: the same
  document takes **0.10 seconds**, and the curve is linear from 4,000 to 32,000. The scan stays where it
  is for the callers that make three lookups, and a failed index allocation falls back to it rather than
  failing the conversion.
- **A picture *fill* is no longer emitted as the document's picture.** An `a:blip` was matched wherever
  it stood, and the same element under an `a:blipFill` is the bitmap a drawn shape, a chart wall or a
  table cell is painted with. A shape's wallpaper came out as the figure the paragraph shows -- and it
  contradicted the walk's own documented rule that a drawn shape comes to nothing. A blip now counts
  only as the direct child of a `pic:blipFill`, which is the DrawingML *picture* vocabulary.
- **An image inside a fenced code block no longer writes a file nothing refers to.** A fence emits its
  text and nothing else, so the picture was dropped from the output and extracted to disk anyway. It
  degrades to its alt text before anything is planned, which also stops the alt text being lost.
- **An exclamation mark in front of a link no longer turns it into an image.** "see this!" followed by a
  hyperlink renders as a broken picture with the link text gone, which is CONVERSION_REFERENCE 4.2's
  pitfall 7. `MdEscape` leaves the mark alone on purpose -- it is only dangerous next to a bracket the
  emitter itself writes -- so the emitter escapes it, where it can see one.
- **A hard break at the edge of a hyperlink no longer emits `[](url)`.** A link that runs over a break
  is closed at the end of its line and opened again on the next, and a break at the very first or last
  position of one leaves a half with nothing between its brackets. The bracket is unwound instead.
- **A generated media path now percent-encodes `#`, `%` and `?`.** `MD_CONTEXT_LINK_DEST` leaves those
  three alone deliberately, because a *target* arrives already encoded far more often than it arrives
  holding a literal one -- but none of that holds for a path this converter generates, where all three
  are ordinary bytes of a file name. A document called `draft #2.docx` linked its pictures to a fragment
  of itself.
- **A `--media-dir` ending in a separator no longer doubles it.** A trailing separator is how a person
  spells "a directory", and every emitted path joins with a separator of its own. A drive letter is the
  one place the separator is part of the name and is left alone.
- **A heading's slug no longer carries the padding a renderer strips.** `# Intro ` was slugged
  `-intro-`, so the link the document wrote reached an anchor the page does not have.
- **A slug no longer keeps the numbers github-slugger removes.** The keep set was the whole of category
  N; the renderer's is `Nd` alone, so a heading holding a vulgar fraction or a Roman numeral resolved to
  a fragment that does not exist. The regex settles it in Latin-1 by itself, and Bengali says it twice
  over: the digits U+09E6..U+09EF are kept and the currency numerators U+09F4..U+09F9 beside them are
  not.
- **A link to a bookmark the document does not define no longer emits `[nowhere]()`.** Muting an empty
  link ran before the anchors were resolved, and resolution is itself a way for a link to lose its
  destination; it runs again afterwards, so a dangling internal reference degrades to plain text exactly
  as a dangling relationship does. `tests/fixtures/anchors` was written by hand before the converter was
  run at it and failed on this, which is the whole reason for writing one that way.
- **A pass that appended arena bytes onto the same arena read freed memory.** `LinkResolver` copies a
  heading's slug onto the destination of the anchor that reaches it, and both live in the destination
  arena; growing it frees the block the source pointer is in. AddressSanitizer found it on the `anchors`
  fixture. `IrStore` now remembers an interior source as an offset across the growth, so no later caller
  can hit the same thing.
- **A strikethrough that wraps another delimiter no longer disappears.** `word~~**x**~~` emits four
  literal tildes and no strikethrough at all: CommonMark refuses a delimiter run that is both preceded
  by a letter and followed by punctuation, and a `~~` in front of a `**` is always followed by
  punctuation. Two `~~` runs that meet fail differently and as completely -- `~~a~~~~b~~` is a run of
  four tildes, which GFM does not recognise. A strikethrough that wraps anything is written `<del>`
  instead, which has no flanking rule at all; one that wraps only text keeps the `~~` the mapping table
  rules.
- **Emphasis no longer disappears against punctuation.** The same flanking rules break `word**(a)**after`
  and `***T*****=eq=**`, and the general answer is the same one: where the outermost Markdown delimiter
  cannot open or close where it stands, an HTML element takes its place. Two delimiter runs that meet
  are one run to a parser, so the test steps back over an adjacent run before looking at what precedes
  it.
- **Two adjacent runs that render as the same code span now merge.** Code drops bold and italic, so a
  bold monospace run and a plain one beside it come out identical -- and left unmerged their two
  backtick delimiters met and a renderer read the pair as one span with backticks in it. The bits are
  cleared in the walker for the same reason the complex-script twins share one: the intermediate
  representation is the output model, and what renders identically has to coalesce.
  All three were found by a differential test against an independent CommonMark implementation, not by
  a fixture, and none of them was reachable from the suites as they stood. The punctuation the flanking
  test rests on is CommonMark's own definition -- the Unicode P and S categories, as a 338-range table
  generated from the character database and binary-searched; a first cut carried a hand-picked subset
  and an audit found it wrong both ways, calling 326 letters and numbers punctuation and missing 854
  punctuation characters below U+3100 alone, the Arabic full stop and the Hebrew maqaf among them. The
  generated table agrees with Python's `unicodedata` on all 1,112,064 code points.
  `docs/CONVERSION_REFERENCE.md`
  gains an eleventh pitfall in 4.2 for the flanking rules, and mapping rows 6 and 11 gain the two
  caveats it implies, so the two documents do not disagree about what a delimiter may do.
- **A document whose default font is monospace is no longer one enormous code block.** Row 12's second
  detection asks whether every text-bearing run in a paragraph is monospace, and a `w:docDefaults`
  naming Courier -- which legal filings routinely do -- makes that true of every paragraph in the file,
  so the whole document converted to a single fence with every heading, emphasis and link delimiter
  dead inside it. Such a default now switches the font heuristic off rather than on: it is a signal only
  where it distinguishes one run from its neighbours. A code *character* style is unaffected, because
  that is a statement about a run and not about the document. `tests/fixtures/monodefault` pins it.
- **A space between two code runs no longer breaks the fence.** Word splits a logical run at every rsid
  boundary and the space between two monospace runs routinely lands in the body font, so a run that
  produced nothing but whitespace was voting against row 12 on exactly the fragmentation correctness
  rule 4 exists to absorb. The vote is now taken once a whole run is read, and only by a run that
  produced a byte which is neither a space nor a tab -- a space renders identically in every face.
- **Three adjacent asterisk spans no longer lose all three.** CommonMark reads adjacent runs of one
  delimiter character as a single run and pairs openers to closers by length -- its rule of three -- so
  `**bo*****th****ree*` came out as six literal asterisks. That is arithmetic no character class can
  express, so the flanking test could not see it; a span abutted by an identical run on both sides now
  takes the element form, which has neither a length nor a flanking rule and also keeps the two
  Markdown runs apart.
- **A fenced block no longer closes on its own content.** The backtick run was measured within each
  span, but a code block's spans need not carry equal formatting -- a bold ` `` ` beside a plain
  `` ` `` stays two spans -- so a fence sized at three met content holding three. The run now carries
  from span to span and resets at a line end.
- **A fence no longer opens on a line of invisible padding.** Its outermost blank blocks were trimmed by
  byte count, so a code paragraph of nothing but spaces counted as a line of code; the test is now the
  same has-content test `IrEndBlock` applies to every other block kind. Inside the fence a blank line
  still stays verbatim, because there it is content.
- **A blank line inside a fence survives.** `IrEndBlock` trimmed a block's leading and trailing break
  spans on the reasoning that a break with nothing beside it renders as a stray marker -- which is true
  everywhere except inside a fence, where a break is a real newline and no marker is written for it, so
  the trim simply lost a line. `IR_BLOCK_CODE` is exempt.
- **A vendor element inside a `w:pBdr` no longer suppresses a horizontal rule.** The borders that make an
  empty paragraph a rule were matched by name and every *other* element counted against it, so an
  extension element or an `mc:AlternateContent` inside the `w:pBdr` read as a fourth border. Both halves
  of `CT_PBdr` are now matched by name from their own tables, which is the OOXML compatibility model:
  what this build has never heard of gets no vote.
  All seven were raised by a review reading the code against the specification rather than by a test,
  and every one was reproduced on the build before it was fixed. Each is pinned by a fixture, a unit
  case, or both.
- **The monospace-baseline guard now covers the half that matters more.** It read `w:docDefaults` alone,
  and the other place a document declares a font baseline is the `w:default="1"` paragraph style -- which
  is what Word's *Modify Style ▸ Normal* writes, and what LibreOffice and Pandoc reference documents
  carry. It is also the likelier of the two, because Word's `w:docDefaults` normally names a *theme*
  slot, which specifies no family at all. A file with a theme `w:docDefaults` and Courier on `Normal`
  still converted to one fence, its headings becoming `` # `Chapter One` `` and its quotes
  `` > `A quotation.` ``. The baseline is now computed once after the chains are folded, nearest-wins
  over both halves, and both lower layers are suppressed where it is monospace -- a run's own `w:rFonts`
  and its character style still speak, because each is a statement about a run and not about the
  document. A *proportional* default style beats a monospace `w:docDefaults` and puts the heuristic back
  on, which is the same nearest-wins rule and the right answer: an unstyled paragraph is proportional
  there, so a monospace run really does stand out. `tests/fixtures/monostyle` is the golden pair.
- **Whitespace hoisting now covers the whole Zs category**, not the ASCII space, the tab and U+00A0
  alone: U+1680, U+2000-U+200A, U+202F, U+205F and U+3000 flank exactly the way an ordinary space does,
  so a delimiter written hard against one does not parse and the formatting was silently lost --
  `a<EN SPACE>bold` in bold emitted `a** bold** c`, which renders as four literal asterisks. Neither
  character is exotic: U+2002 is one Insert ▸ Symbol away in Word and U+3000 is what a CJK keyboard's
  space bar produces. The emitter's flanking classifier gained the same set, which also *relaxes* it --
  a Zs in front of a span is whitespace rather than a word character, so a delimiter that parses
  perfectly well no longer spends an HTML element. U+200B stays out of both: it is Cf, not Zs, and
  CommonMark does not count it.
- **A discarded `mc:Choice` no longer votes on the paragraph it was rewound out of.** The row 12 verdict
  is walker state rather than intermediate representation, so `IrRewind` does not carry it and the walk
  has to restore it by hand, exactly as `DocWalkParagraph` already did around a paragraph. Without it a
  plain `mc:Choice` beside an all-monospace `mc:Fallback` demoted the fence that survived to an inline
  code span, and a monospace Choice beside a plain Fallback did the reverse.
- **A run that contributes no visible character no longer breaks a fence.** The abstention rule counted
  bytes as they arrived rather than as they would be emitted, but a CR or an LF inside a `w:t` folds to
  one space and a soft hyphen is dropped outright -- so a run made only of those voted, and Word gives a
  hyphenation point from a later editing session its own `w:r`. The set is now the tab and the whole Zs
  category as well, which is where the first cut of this fix stopped short: it was widened in a
  different direction from the other two whitespace sites and never reached the class they had just
  agreed on, so a body-font U+2002 or U+3000 between two monospace runs still broke the fence. On a
  four-line listing with one such character, one fence became three blocks **and the demoted line's
  leading indentation was silently dropped** by the emitter's padding trim -- bytes that survive under an
  ASCII space. U+00A0 stays out of the set by mapping row 35, which makes it content; that is the one
  place this question and `RunCoalescer`'s answer differ, and it is deliberate.
- **A `w:basedOn` across two style types is now ignored**, which ISO/IEC 29500-1 17.7.4.3 requires: a
  character style's parent shall be a character style. Beyond conformance it was a hole straight through
  the monospace guard above -- a character style based on a monospace default *paragraph* style inherits
  its family, and `StyleResolveRun` takes the character layer *before* the guard is read, so a document
  whose only sin was `<w:basedOn w:val="Normal"/>` on an italic character style converted its prose to a
  fenced code block. The role leaks the same way in the other direction, and that manifestation needs no
  monospace font at all: a paragraph style based on a character style named "HTML Code" made ordinary
  prose a fence. The test asks whether **both** styles said what they are, because `w:type` is optional
  and this reader defaults an absent one to paragraph -- comparing the stored types alone would drop a
  typeless style's link to a real character style, which is a shape producers write, and would silently
  lose the code span it carries.

- Decision **D12**, ruled 2026-08-26: a `$` is escaped as `\$`, but only where the assembled line holds
  two or more of them. GitHub has read `$...$` as inline math and `$$...$$` as display math since 2022,
  so `costs $5 and $10` rendered `5 and ` in math font and lost both signs; `docs/CONVERSION_REFERENCE.md`
  4.1 predates the feature and now carries the row, with a tenth pitfall in 4.2. A math span needs two
  delimiters, so the rule is a count and not a grammar -- GitHub, Pandoc and the KaTeX-based previews
  disagree about whether a space may follow the opener or a digit the closer, and a count is safe under
  every one of those readings without reproducing any. A line holding one `$` keeps it bare, which is
  what the ruling chose conditional escaping for: a price is the common case and pays nothing. It applies
  in the five contexts whose text a renderer parses as inline content and in none of the other three.
  `tests/fixtures/dollars` is the golden pair, its `expected.md` written by hand from the specification
  before the converter was run at it, and it matched on the first run. Two of its cases exist to pin
  the scope the rule depends on: a line built from two runs holding one dollar each, which is escaped
  only while the count is taken over the whole line, and a heading whose break folds to a space, so
  its two dollars share one scope and both escape -- the opposite answer to the paragraph beside it,
  whose break splits them into two lines that each keep their dollar bare. An adversarial review
  raised the first: `textflow` already pinned per-line escaping in general through the ampersand
  rule, but nothing pinned the dollar count's dependence on it, and nothing pinned the heading scope
  at all. Both were checked against a scratch build that escapes at span boundaries, which is what
  M6 will do: it fails these two cases and no others.
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
  descriptor, an archive comment, ISO 29500 Strict URIs, a byte-order mark, a UTF-16 part, a main part
  reached through the content-type table, a content type that disagrees with its relationship, a
  decoy relationship, mixed-case part names and a duplicated entry name all have to produce the same
  bytes, which is a stronger statement than the exit code and the message substring they asserted
  before.
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
  Markdown and exits 0, and `tests/make_fixtures.py`'s expectation table says so for twenty-four
  fixtures -- the nineteen sound packages M4 left expecting 5, and the five golden cases M5 adds.
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
- A spec-legal 45 KB `.docx` could spin for seconds with no output and no refusal. `StyleFind` was a
  linear scan of full byte comparisons and the walker caches one style identifier, so a document
  whose paragraphs alternate between two of many long-prefixed identifiers cost paragraphs x styles
  x identifier length. Measured on the shim at `-O2`, with a control of identical parse volume whose
  paragraphs all name one style: 13.53 s against 0.08 s. `StyleFind` now answers from an
  open-addressed index built once at load, which puts the same file at 0.09 s and emits the same
  99,999 bytes. The index goes in before `StyleLinkChains`, which is a lookup per style and was the
  load-time half of the same shape. A model whose index cannot be allocated keeps the scan.
- A `w:styleId` longer than 255 bytes was stored in full while the walker truncated its lookup key to
  255, so the two differed at that byte and the paragraph silently took the default style -- a
  heading lost with no diagnostic. Both the identifier and `w:basedOn` are now stored capped at the
  ceiling the lookup key uses. ISO/IEC 29500 caps `ST_String` at 255, so nothing in spec changes.
- Two inputs whose derived output paths collide silently overwrote each other: `-o dst/`
  `p/report.docx q/report.docx` converted both, kept the second and exited 0. A pre-flight now runs
  before anything is written -- the first input to name a path keeps it, later ones are refused into
  D7c's failure list, and an input whose derived output is another input of the same run is refused
  rather than destroying it. One input named twice is not a collision: it writes the same bytes over
  its own output. The architecture note recommending this pre-flight gives it to `Batch` at M13; the
  loop in `main.cpp` is what `Batch` replaces, so it lives there until then.
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
