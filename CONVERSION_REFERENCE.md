# DOCX to Markdown Conversion: Domain Reference (ECMA-376 / ISO/IEC 29500)

The authoritative domain specification for DOCXtoMD — companion to the repo-root `CLAUDE.md`, which
holds the decided default policies, planned module layout, and roadmap. Read the sections relevant to
a conversion milestone before implementing it.

Target: native C++ console converter, DOCX (WordprocessingML) in, GitHub-Flavored Markdown (GFM) out, no external converter tools. This reference covers the container format, the element inventory, the feature mapping, escaping rules, correctness traps ranked by real-world frequency, and a recommended pipeline architecture.

---

## 1. The OPC container (ISO/IEC 29500-2)

A `.docx` file is an **Open Packaging Conventions (OPC)** package: a plain ZIP archive containing XML "parts" plus binary media, wired together by *relationship* files. Nothing about part locations is guaranteed by name — everything must be resolved through relationships, though in practice the conventional layout below is what Word, Google Docs, LibreOffice, and Pandoc all emit.

### 1.1 Typical package layout

```
[Content_Types].xml
_rels/.rels
docProps/core.xml            (Dublin Core metadata: title, author, dates)
docProps/app.xml             (application metadata)
word/document.xml            (the main document part - the body content)
word/_rels/document.xml.rels (relationships FROM document.xml)
word/styles.xml              (style definitions + docDefaults)
word/numbering.xml           (list numbering definitions)
word/settings.xml            (document-wide settings)
word/fontTable.xml           (font declarations - rarely needed)
word/footnotes.xml           (footnote content)
word/endnotes.xml            (endnote content)
word/comments.xml            (comment content)
word/theme/theme1.xml        (theme fonts/colors - needed only to resolve
                              rFonts w:asciiTheme etc., usually skippable)
word/header1.xml ...         (headers/footers, referenced from sectPr)
word/media/image1.png ...    (embedded images and other media)
```

### 1.2 `[Content_Types].xml`

Maps every part to a MIME content type via two mechanisms:

- `<Default Extension="png" ContentType="image/png"/>` — applies by file extension.
- `<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>` — applies to one exact part, wins over Default.

Converter uses:
1. **Locating the main document part robustly**: the part whose content type is `...wordprocessingml.document.main+xml` (or `...document.macroEnabled.main+xml` for `.docm`, or `...template.main+xml` for `.dotx`). This is the fallback/cross-check for the relationship-based lookup in 1.3.
2. **Choosing output file extensions for extracted media**: an image part's true type comes from Content_Types, not from its ZIP entry name. Google Docs in particular has emitted media entries whose extension does not match the content (and content types like `image/x-emf`). Map content type -> extension: `image/png`->`.png`, `image/jpeg`->`.jpg`, `image/gif`->`.gif`, `image/bmp`->`.bmp`, `image/tiff`->`.tif`, `image/svg+xml`->`.svg`, `image/x-emf`/`image/emf`->`.emf`, `image/x-wmf`->`.wmf`. EMF/WMF cannot be displayed by Markdown renderers — extract them anyway and link them, or (policy) warn.

### 1.3 Relationships: `_rels/.rels` and `word/_rels/document.xml.rels`

Relationship files live in a `_rels` subfolder next to the source part, named `<partname>.rels`. Each `<Relationship>` has:

| Attribute | Meaning |
|---|---|
| `Id` | e.g. `rId5` — the key that `r:id`/`r:embed` attributes in content reference |
| `Type` | a URI identifying the relationship kind |
| `Target` | part path (relative to the source part's folder) or external URL |
| `TargetMode` | absent/`Internal` = a package part; `External` = a URI outside the package (hyperlinks, linked images) |

**Package-level** `_rels/.rels` contains the entry of type `.../officeDocument/2006/relationships/officeDocument` whose Target is the main document part (conventionally `word/document.xml`, but resolve it — do not hardcode).

**Part-level** `word/_rels/document.xml.rels` is where a converter spends its time. Relationship types you will resolve (all prefixed `http://schemas.openxmlformats.org/officeDocument/2006/relationships/`):

| Type suffix | Target | Referenced by |
|---|---|---|
| `styles` | word/styles.xml | (loaded eagerly) |
| `numbering` | word/numbering.xml | (loaded eagerly) |
| `settings` | word/settings.xml | (loaded eagerly) |
| `footnotes` | word/footnotes.xml | `w:footnoteReference w:id` |
| `endnotes` | word/endnotes.xml | `w:endnoteReference w:id` |
| `comments` | word/comments.xml | `w:commentReference w:id` |
| `image` | word/media/imageN.ext or External URL | `a:blip r:embed` / `r:link`, `v:imagedata r:id` |
| `hyperlink` | External URL (TargetMode="External") | `w:hyperlink r:id` |
| `header` / `footer` | word/headerN.xml | `w:headerReference r:id` in sectPr |
| `theme` | word/theme/theme1.xml | theme font resolution |

**Resolution rule**: an `r:id` (attribute namespace `http://schemas.openxmlformats.org/officeDocument/2006/relationships`, conventionally prefix `r`) appearing in part P is looked up in P's own `.rels` file — **relationship IDs are scoped per part**. `rId3` in document.xml and `rId3` in footnotes.xml are unrelated. Footnote/header parts have their own `word/_rels/footnotes.xml.rels` etc.; an image inside a footnote resolves through the footnote part's rels, so the rels loader must be generic, keyed by part.

Relative `Target` values resolve against the source part's directory (`word/` for document.xml): `media/image1.png` -> `word/media/image1.png`. Targets may begin with `/` (package-absolute) and may contain `../` — normalize segments *within the package namespace only* (never onto the real filesystem; see §5 security).

**Strict vs Transitional namespaces**: almost every real file uses ECMA-376 Transitional (`http://schemas.openxmlformats.org/wordprocessingml/2006/main` for `w:`, `.../officeDocument/2006/relationships` for `r:`, `.../drawingml/2006/main` for `a:`, `.../drawingml/2006/wordprocessingDrawing` for `wp:`, `.../drawingml/2006/picture` for `pic:`, math `.../officeDocument/2006/math` for `m:`, VML `urn:schemas-microsoft-com:vml` for `v:`). ISO 29500 Strict files (rare; Word can save them) use `http://purl.oclc.org/ooxml/wordprocessingml/main` etc. Match elements by **namespace URI + local name**, with both URI families accepted as aliases; never match on the literal `w:` prefix, which is not guaranteed.

**Markup Compatibility (`mc:`)**: `mc:AlternateContent` wraps content in one or more `mc:Choice Requires="..."` branches plus one `mc:Fallback`. Word uses this around `w:drawing` when newer DrawingML features are present; the Fallback is typically legacy `w:pict` VML. Rule: take the first `mc:Choice` whose `Requires` namespaces you understand (e.g. `wps`, `wpg` you likely do not); otherwise take `mc:Fallback`. Ignore `mc:Ignorable` attributes and elements in namespaces listed there (`w14`, `w15`, `w16se`, `cx`, ...).

---

## 2. WordprocessingML element inventory (ISO/IEC 29500-1 §17)

### 2.1 Block-level structure

| Element | Role | Converter handling |
|---|---|---|
| `w:document` / `w:body` | Root; body's children are block items | Iterate children: `w:p`, `w:tbl`, `w:sdt`, `w:sectPr` (trailing), `w:bookmarkStart/End`, `w:ins`/`w:del` wrappers, `mc:AlternateContent` |
| `w:p` | Paragraph | The core block. First child may be `w:pPr` |
| `w:pPr` | Paragraph properties | `w:pStyle`, `w:numPr`, `w:ind`, `w:jc`, `w:outlineLvl`, `w:pBdr`, `w:spacing`, `w:rPr` (mark-run props — apply to paragraph *mark* only, not to text; ignore for content, but it participates in numbering-symbol formatting), `w:pPrChange` (tracked — ignore, use current props) |
| `w:tbl` | Table | See §2.5 |
| `w:sectPr` | Section props (page size/margins, header/footer refs, columns) | Ignore for layout; note that a mid-body sectPr lives inside a `w:pPr` (that paragraph is the last of the section). Headers/footers: policy = skip |
| `w:sdt` | Structured document tag (content control) | **Recurse into `w:sdtContent` transparently** at every level (block, run, cell, row). `w:sdtPr` may identify TOC (`w:docPartObj`/`w:docPartGallery` val "Table of Contents") — policy: skip TOC SDT content and optionally regenerate |
| `w:bookmarkStart` / `w:bookmarkEnd` | Anchor with `w:name`, `w:id` | Record position -> anchor. Skip machine bookmarks: `_GoBack`, `_Toc*`, `_Ref*`, `_Hlk*` unless targeted by an internal hyperlink |
| `w:ins` / `w:del` | Tracked insert/delete (wrap runs; also appear inside pPr/rPr as `w:rPrChange` etc.) | Accept-all policy: descend into `w:ins` keeping content; drop `w:del` entirely (its text is `w:delText`). Also: `w:moveTo` keep, `w:moveFrom` drop; `w:cellIns` keep, `w:cellDel` drop row/cell |
| `w:smartTag` / `w:customXml` | Semantic wrappers | Transparent — recurse into children |
| `w:proofErr`, `w:permStart/End` | Spellcheck / editing-permission markers | Skip (childless markers) |

### 2.2 Run-level structure

| Element | Role | Converter handling |
|---|---|---|
| `w:r` | Run — a span of uniformly formatted content | First child may be `w:rPr`. Content children below |
| `w:rPr` | Run properties | See §2.3 |
| `w:t` | Text. **`xml:space="preserve"` means keep leading/trailing spaces**; without it, the XML spec mandates no normalization of element content, but the consumer may treat leading/trailing whitespace as insignificant (Word writes `preserve` on any `w:t` with significant edge whitespace) — parse literally either way. Never trim interior whitespace | Append text |
| `w:delText` | Text inside deleted (`w:del`) runs | Dropped with the deletion |
| `w:instrText` / `w:delInstrText` | Field instruction text | Feed field state machine (§2.7), never emit directly |
| `w:tab` | Tab character | Emit a tab or spaces (policy; inside tables/code use spaces). Do not confuse with `w:tabs` inside pPr (tab stops — ignore) |
| `w:br` | Break. `w:type`: absent/`textWrapping` = line break; `page`; `column`. `w:clear` irrelevant | textWrapping -> hard line break; page/column -> paragraph break or nothing (policy; optionally `---`) |
| `w:cr` | Carriage return | Same as textWrapping break |
| `w:noBreakHyphen` | Non-breaking hyphen | Emit `-` (or U+2011, policy) |
| `w:softHyphen` | Optional hyphenation point (U+00AD) | **Drop** (invisible; corrupts words if kept) |
| `w:sym` | Symbol: `w:font` + `w:char` (hex). Values `F000`-`F0FF` are the Private Use Area convention: subtract 0xF000 to get the font's 8-bit code point (Wingdings/Symbol) | Map common Symbol/Wingdings codes via table, else emit replacement char or the raw char; never emit the PUA code point blindly |
| `w:drawing` | DrawingML image (see §2.6) | Image |
| `w:pict` / `w:object` | Legacy VML image / embedded OLE object | `v:shape` > `v:imagedata r:id`; OLE objects: extract preview image if present, else note placeholder |
| `w:footnoteReference` / `w:endnoteReference` | `w:id` into footnotes.xml/endnotes.xml | Emit `[^n]`; skip if `w:customMarkFollows` weirdness (rare) |
| `w:footnoteRef` / `w:endnoteRef` | The marker *inside* the note body | Skip (the `[^n]:` label replaces it) |
| `w:commentReference` | `w:id` into comments.xml | Policy: drop, or emit as HTML comment/footnote |
| `w:fldChar` | Complex-field boundary: `w:fldCharType` = `begin`/`separate`/`end` | Field state machine (§2.7) |
| `w:lastRenderedPageBreak` | Cache hint | Skip |
| `w:ptab` | Absolute-position tab | Treat as tab/space |
| `w:ruby` | East-Asian ruby text | Emit base text (`w:rubyBase`), optionally parenthesized ruby |

`w:hyperlink` is a **run-container** inside a paragraph (sibling of runs): attributes `r:id` (external, resolve via rels, TargetMode=External) or `w:anchor` (internal — bookmark name), optional `w:tooltip`; children are runs (which may be individually formatted). `w:fldSimple` is likewise a run-container with a `w:instr` attribute; its child runs are the cached field result.

`m:oMath` / `m:oMathPara` — Office Math (OMML). Elements: `m:r`/`m:t` (runs), `m:f` (fraction: `m:num`/`m:den`), `m:sSup`/`m:sSub`/`m:sSubSup` (scripts), `m:rad` (radical), `m:nary` (sum/integral), `m:d` (delimiters), `m:m` (matrix). Full OMML->LaTeX is a project in itself; recommended v1 policy: linearize (concatenate `m:t` text, `^{}`/`_{}` for scripts, `a/b` for fractions) or emit `$...$` LaTeX for the easy subset — GitHub renders `$...$`/`$$...$$` math.

### 2.3 Run properties (`w:rPr`) that affect output

| Element | Semantics | Notes |
|---|---|---|
| `w:b` / `w:bCs` | Bold (`bCs` = complex-script bold) | **Toggle property** (§5.2). `w:val` is OnOff: absent = true; `0`/`false`/`off` = false; `1`/`true`/`on` = true (Strict files use only `true`/`false`/`1`/`0`; treat any unrecognized value as false — defensive fallback, not spec) |
| `w:i` / `w:iCs` | Italic | Toggle property |
| `w:strike` / `w:dstrike` | Strikethrough / double-strike | `strike` is a toggle (§5.2); `dstrike` is plain on/off — nearest definition wins, no XOR. Treat dstrike as strike for output (`~~`) |
| `w:u` | Underline. `w:val`: `single`, `double`, `wavy`, `dotted`, ..., `none` | **Not** a toggle; `val="none"` cancels inherited underline |
| `w:vertAlign` | `superscript` / `subscript` / `baseline` | baseline cancels inherited |
| `w:highlight` | Highlight color name (`yellow`, `green`, ... `none`) | No MD equivalent — policy (§3) |
| `w:shd` | Character shading | Sometimes used instead of highlight; can also mark code (gray shading) |
| `w:rStyle` | Character style reference | Resolve through style chain; key for inline-code detection |
| `w:rFonts` | `w:ascii`, `w:hAnsi`, `w:cs`, `w:eastAsia` + theme variants (`w:asciiTheme` -> theme1.xml font scheme) | Monospace detection for code |
| `w:vanish` / `w:webHidden` | Hidden text | `vanish` is a toggle (§17.7.3); `webHidden` is plain on/off — nearest specification wins. Either way drop hidden runs (Word hides TOC field codes this way) |
| `w:caps` / `w:smallCaps` | Displayed capitalization | Toggle; policy: uppercase the text for `caps`, leave as-is for smallCaps |
| `w:color`, `w:sz` (half-points), `w:spacing`, `w:kern` | Visual only | Ignore (sz can support heading-guess heuristics only as last resort) |
| `w:rPrChange` | Tracked formatting change (old props) | Ignore — current rPr is the accepted state |
| `w:noProof`, `w:lang` | Proofing | Ignore |

The full toggle-property set per ISO 29500-1 §17.7.3 (twelve properties): `w:b`, `w:bCs`, `w:i`, `w:iCs`, `w:caps`, `w:smallCaps`, `w:strike`, `w:outline`, `w:shadow`, `w:emboss`, `w:imprint`, `w:vanish`. Note `w:dstrike` is **not** in the list — it is a plain on/off property (a well-known spec asymmetry with `w:strike`).

### 2.4 Paragraph properties that affect output

- `w:pStyle w:val="styleId"` — resolve through styles.xml.
- `w:numPr` > `w:ilvl w:val` (0-8) + `w:numId w:val`. **`numId` 0 means "no numbering"** (used to cancel style-inherited numbering). `numPr` may also come from the paragraph style chain (a style's `w:pPr` can contain `w:numPr`) — a common pattern for the built-in `ListParagraph` + heading-numbering styles.
- `w:outlineLvl w:val` (0-8) — heading level independent of style name; 9 = body text.
- `w:ind` — `w:left`/`w:start`, `w:hanging`, `w:firstLine` in twips (1/20 pt; 720 twips = 0.5"). Used for blockquote/indent heuristics and manual list-indentation sanity checks.
- `w:pBdr` — paragraph borders; a lone `w:bottom` (or `w:between`) on an otherwise empty paragraph is Word's autoformatted horizontal rule.
- `w:jc` — justification; ignore (or map `center` via HTML if configured).
- `w:keepNext`, `w:spacing`, `w:shd` (paragraph shading — code-block hint), `w:framePr` (text frames — treat content normally).

### 2.5 Tables

```
w:tbl
  w:tblPr        (w:tblStyle, w:tblW, w:tblBorders, w:tblLook)
  w:tblGrid      (w:gridCol w:w="twips" per column - the authoritative column count)
  w:tr
    w:trPr       (w:tblHeader = repeat-as-header row; w:trHeight)
    w:tc
      w:tcPr     (w:gridSpan w:val="N" = horizontal span;
                  w:vMerge w:val="restart" = starts vertical merge,
                  w:vMerge with no val or val="continue" = continues it;
                  w:tcW, w:vAlign, w:tcBorders, w:shd)
      block content: w:p+, nested w:tbl, w:sdt ...
```

Facts a converter must respect:
- Cell content is **block content** — one or more paragraphs, possibly nested tables. A `w:tc` always contains at least one `w:p` (even "empty" cells).
- **Column count** = number of `w:gridCol` in `w:tblGrid` (rows can differ in `w:tc` count because of `gridSpan`). Pad short rows.
- `gridSpan` consumes N grid columns with one `w:tc`. `vMerge` cells still appear in every spanned row (the continuation cells carry `w:vMerge` and an empty paragraph).
- Header row determination: (a) any row with `w:trPr/w:tblHeader`; else (b) `w:tblLook` `firstRow="1"` (or bit 0x020 in the legacy `w:val` bitmask) combined with the table style defining first-row formatting; else (c) default to first row as header (GFM *requires* a header row). Provide a policy to emit an all-empty header row instead when the first row is clearly data.
- Rows/cells can be wrapped in `w:sdt` (`w:sdtContent` holding `w:tr` or `w:tc`) and in `w:customXml` — recurse.
- Right-to-left tables: `w:bidiVisual` — column order reversal; rare, note and ignore initially.

### 2.6 Images

Modern (DrawingML):
```
w:r > w:drawing > (wp:inline | wp:anchor)
  wp:extent cx= cy=              (size in EMU; 914400 EMU = 1 inch)
  wp:docPr id= name= descr=      (descr = alt text; name = fallback alt)
  a:graphic > a:graphicData[uri=".../picture"] > pic:pic
    pic:nvPicPr
    pic:blipFill > a:blip r:embed="rIdN"   (or r:link="rIdN" for external)
    pic:spPr
```
- `wp:inline` flows with text; `wp:anchor` is floating (has position offsets, `wp:wrapNone` etc.). Markdown has no floats: **treat anchored images as inline at their anchor point**, in document order.
- `r:embed` -> relationship of type `image` -> `word/media/...`; `r:link` -> External target (emit the URL directly). A blip can carry both (embed = cached copy) — prefer embed.
- `a:graphicData` with other URIs (charts `.../chart`, SmartArt `.../diagram`, shapes `wps:`): no bitmap available — emit placeholder text or skip (policy). Charts/SmartArt have no rendered fallback in the file.
- Legacy (VML): `w:pict > v:shape > v:imagedata r:id="rIdN"` (attribute `o:title` = alt). Also appears as the `mc:Fallback` of AlternateContent. `v:rect` / `v:line` with `o:hr="t"` is a legacy horizontal rule.
- Alt text: prefer `wp:docPr/@descr`, then `@title`, then `@name`; sanitize `]` and newlines.

### 2.7 Fields

Two syntaxes, one semantic:

1. **Simple**: `<w:fldSimple w:instr=" HYPERLINK \"https://x\" ">` with child runs = cached result.
2. **Complex**: a run containing `w:fldChar w:fldCharType="begin"`, then runs whose `w:instrText` accumulate the instruction, optional `w:fldChar type="separate"`, then result runs, then `w:fldChar type="end"`. **Fields nest** (a TOC field contains PAGEREF fields) — maintain a stack. Fields can span paragraphs (TOC always does). If `separate` is absent there is no cached result — you must interpret the instruction or emit nothing.

State machine: on `begin` push field frame (mode=instruction); `w:instrText` appends to top frame's instruction; on `separate` switch top frame to result mode; on `end` pop and dispatch. Runs encountered while any frame is in instruction mode are suppressed; in result mode they are captured as the field result (or passed through, per field kind). The `w:fldChar` may carry `w:fldLock`/`w:dirty` — ignore.

Instruction grammar: keyword, then arguments (quoted with `"`), then switches (`\l "anchor"`, `\o`, `\h`, `\* MERGEFORMAT`). Common fields:

| Field | Handling |
|---|---|
| `HYPERLINK "url"` / `HYPERLINK \l "bookmark"` | Convert to `[result](url)` / `[result](#anchor)`. Some producers (older Word, some exporters) emit hyperlinks as fields instead of `w:hyperlink` |
| `TOC \o "1-3" \h ...` | Skip the entire field (result included) — the TOC is stale layout; MD readers get navigation from headings. (Configurable: keep cached result as plain list) |
| `PAGE`, `NUMPAGES`, `DATE`, `TIME`, `AUTHOR`, `FILENAME` | Emit cached result text, or nothing if no result (PAGE has no meaning in MD) |
| `REF bookmark \h` / `PAGEREF` | `REF`: link `[result](#bookmark)` if `\h`, else result text. `PAGEREF`: result text (page number — meaningless; usually only inside skipped TOCs) |
| `SEQ Figure \* ARABIC` | Emit cached result (the number). No live renumbering in v1 |
| `STYLEREF`, `NOTEREF` | Cached result |
| `INCLUDEPICTURE "url"` | `![](url)` if instruction parseable, else result |
| `SYMBOL`, `ADVANCE`, `MACROBUTTON`, form fields (`FORMTEXT` + `w:ffData`) | Result text / skip |

### 2.8 styles.xml

```
w:styles
  w:docDefaults > w:rPrDefault/w:rPr, w:pPrDefault/w:pPr   (root of inheritance)
  w:latentStyles                                            (ignore)
  w:style w:type="paragraph|character|table|numbering" w:styleId="..."
          [w:default="1"]
    w:name w:val="heading 1"      (UI name - the stable, localization-portable key
                                   for built-ins is actually the *English* w:name;
                                   styleId is arbitrary and localized Word builds
                                   use e.g. "berschrift1")
    w:basedOn w:val="parentId"
    w:link  (paragraph<->character style pairing)
    w:pPr / w:rPr                 (this style's contributions)
    w:qFormat, w:uiPriority       (ignore)
```
Heading detection order: (1) resolved style chain contains a style whose `w:name` matches `heading N` (case-insensitive) or styleId matches `Heading[1-9]`/`berschrift[1-9]` etc.; (2) effective `w:outlineLvl` 0-8 from pPr or style chain. `Title` style -> `#` (policy) with real `heading 1` demoted or both mapped to `#`. Levels 7-9 clamp to `######`.

### 2.9 numbering.xml

Two-level indirection:

```
w:numbering
  w:abstractNum w:abstractNumId="A"
    w:multiLevelType (ignore)
    w:numStyleLink w:val="styleId"   (this abstract def DELEGATES to a numbering
                                      style - follow: style -> its pPr numPr numId
                                      -> num -> abstractNum with w:styleLink)
    w:lvl w:ilvl="0".."8"
      w:start w:val="1"
      w:numFmt w:val="bullet|decimal|lowerLetter|upperLetter|lowerRoman|
                       upperRoman|decimalZero|ordinal|none|..."
      w:lvlText w:val="%1."         (bullets: the glyph, e.g. Symbol U+F0B7)
      w:lvlRestart w:val="N"        (restart when a higher level < N increments;
                                     default: restart when ANY higher level used)
      w:pStyle                      (style association)
      w:pPr/w:ind                   (visual indent - ignore, use ilvl)
      w:rPr                         (number formatting - ignore)
  w:num w:numId="7"
    w:abstractNumId w:val="A"
    w:lvlOverride w:ilvl="0"
      w:startOverride w:val="1"     (RESTART: this is how Word restarts a list -
                                     new numId, same abstractNum, startOverride)
      [w:lvl ...]                   (full level replacement)
```

Converter model:
- Effective numbering of a paragraph = direct `w:numPr` if present (numId 0 = none), else the style chain's `numPr`.
- `numId` -> `w:num` -> apply `lvlOverride`s over the `abstractNum` levels -> per-`ilvl` definition. Missing `numId` in numbering.xml (dangling reference — happens in the wild): treat as not numbered.
- `numFmt` = `bullet` -> `-` bullet at any level; anything numeric -> ordered list. `none` -> unnumbered continuation paragraph within the list (indent, no marker). Markdown renumbers for you, but to honor `w:start`/`startOverride` emit the actual start number (`3.`) — GFM takes the first item's number as the start.
- **Counter state** lives per (numId is wrong — per **abstractNumId**, per ilvl): two different numIds sharing an abstractNum continue the same sequence unless a `startOverride` resets it. Incrementing level L resets deeper levels' counters (subject to `lvlRestart`).

### 2.10 settings.xml, footnotes/endnotes, comments

- `word/settings.xml`: mostly ignorable. Relevant: `w:footnotePr`/`w:endnotePr` (number format `w:numFmt`, restart rules), `w:evenAndOddHeaders`, `w:updateFields` (hint TOCs are stale), `w:hyphenationZone`/`w:autoHyphenation` (irrelevant — soft hyphens are explicit in text), `w:noPunctuationKerning`. Absence of the whole part is legal.
- `word/footnotes.xml`: `w:footnote w:id="N" [w:type]` elements containing block content. **`w:type="separator"` and `"continuationSeparator"` (ids -1/0 by convention, but check the type attribute) are machinery — skip them.** Same for endnotes (`w:endnote`). Note bodies start with a run containing `w:footnoteRef` (the marker) usually styled `FootnoteReference` — drop that run. Footnotes can contain anything: multiple paragraphs, lists, tables, images, hyperlinks — the note-definition emitter must reuse the full block pipeline with 4-space continuation indentation.
- `word/comments.xml`: `w:comment w:id w:author w:date w:initials` + block content. Body markers: `w:commentRangeStart w:id` / `w:commentRangeEnd` bracket the commented text; `w:commentReference` sits in a run (usually right after the range end). Default policy: drop all three; optional: emit as `[^comment-n]` footnotes or `<!-- author: text -->`.

---

## 3. DOCX feature -> GFM mapping table

| # | DOCX feature | Detection | GFM output | Caveats / policy |
|---|---|---|---|---|
| 1 | Headings 1-9 | Style name `heading N` / styleId `HeadingN` / effective `outlineLvl` N-1 | `#`..`######` + space + inline content | Levels 7-9 clamp to `######`. Strip numbering from numbered headings or keep literal number (policy; keeping is safer). A heading paragraph's bold/font-size formatting is style-borne — do **not** additionally wrap in `**` |
| 2 | Title / Subtitle styles | name `Title`/`Subtitle` | `#` / `##` or bold line | Policy; document it |
| 3 | Bold | Effective toggle `w:b` | `**text**` | Delimiters only around trimmed content (§5.3) |
| 4 | Italic | Effective toggle `w:i` | `*text*` | Use `*` not `_` (intraword `_` does not parse) |
| 5 | Bold+italic | both | `***text***` | Order fixed; avoid `**_.._**` mixing |
| 6 | Strikethrough | `w:strike`/`w:dstrike` | `~~text~~` | GFM extension |
| 7 | Superscript / subscript | `w:vertAlign` | `<sup>..</sup>` / `<sub>..</sub>` | HTML fallback; GFM renders raw HTML. Optional `^`/`~` pandoc-style flag |
| 8 | Underline | `w:u` != none | Policy: drop (default), or `<u>..</u>`, or `<ins>` | Underline usually means hyperlink styling or emphasis-by-habit; never map to `*` (collides with italic) |
| 9 | Highlight | `w:highlight` != none / char `w:shd` | Policy: drop (default) or `<mark>..</mark>` | GitHub renders `<mark>` |
| 10 | Hidden text (`w:vanish`) | toggle | Omit | Word hides field codes this way |
| 11 | Inline code | (a) `w:rStyle` resolving to a style named `Code`/`HTML Code`/`Verbatim Char` (Pandoc)/`Source Text` (LibreOffice)/`Macro Text`; (b) effective `rFonts/@ascii` in monospace set: Consolas, Courier, Courier New, Cascadia Code/Mono, Lucida Console, Menlo, Monaco, DejaVu Sans Mono, Liberation Mono, Fira Code/Mono, JetBrains Mono, Source Code Pro, Roboto Mono, Ubuntu Mono, IBM Plex Mono | `` `code` `` | Inside code spans nothing is escaped: if content contains backticks, lengthen the fence (`` ``a`b`` ``) and pad with spaces when content starts/ends with a backtick. Code detection wins over bold/italic (drop them) or nests HTML — pick: drop |
| 12 | Code block | Paragraph style named `HTML Preformatted`, `Preformatted Text` (LO), `Source Code` (Pandoc), `Code`, `Plain Text`? no — plus heuristic: paragraph whose every run is monospace; merge consecutive such paragraphs into one fence | ```` ``` ```` fenced block | Inner text raw, no escaping; tabs kept; choose fence longer than any internal backtick run. Language never recoverable — bare fence |
| 13 | Blockquote | Style named `Quote`, `Intense Quote`, `Block Text`; optional heuristic: `w:ind/@left` >= 720 twips, not in a list/table (off by default — indentation is ambiguous) | `> ` prefix on every emitted line | Nested quotes only from nested-quote styles (rare) — single level in practice |
| 14 | Bullet list | numbering chain, `numFmt=bullet` | `- item`, nested: 2 spaces per ilvl (GFM: child must indent to parent marker+content offset; 2 for `-`, use 3-4 under ordered parents — safest: indent = sum of parent marker widths, or uniformly 4 spaces per level, which GFM accepts under wide markers... use marker-width-aware indent) | Ignore the actual bullet glyph in `lvlText` (Symbol-font `` etc.) — always `-` |
| 15 | Numbered list | `numFmt` decimal/roman/letter | `1.` `2.` ... with real computed numbers; nested per ilvl | GFM ignores non-decimal formats — letters/romans become decimal (accept, or policy: emit literal text). Start honored via first item's number |
| 16 | List continuation paragraph | Same list interrupted by numPr-less paragraph with matching indent, or numFmt `none` | Indented paragraph inside the item (blank line + item indentation) | Prevents list restart in MD |
| 17 | List restart | New `numId` with `startOverride`, or fresh abstractNum | Intervening block or `1.` restart (GFM: a paragraph between lists separates them) | Two adjacent lists with no separator merge in MD — insert HTML comment `<!-- -->` separator if needed |
| 18 | Table | `w:tbl` | GFM pipe table: header row, `| --- |` delimiter, one line per row | Cells: inline-only. Multi-paragraph / `w:br` in cell -> `<br>`. `\|` for literal pipes (or `&#124;`). Column alignment from first row's `w:jc`/`w:tcPr` -> `:---`, `:---:`, `---:` |
| 19 | Merged cells | `gridSpan` / `vMerge` | No GFM equivalent. Policy A (default): gridSpan -> content in first cell + empty padding cells; vMerge continue -> empty cell. Policy B (flag): fall back to raw HTML `<table>` with rowspan/colspan for any table containing merges | Never silently drop columns — row cell count must equal `w:tblGrid` size |
| 20 | Nested table | `w:tbl` inside `w:tc` | No GFM equivalent — HTML fallback for the whole outer table, or flatten inner table to `<br>`-joined text (default: HTML fallback) | |
| 21 | External hyperlink | `w:hyperlink r:id` (TargetMode External); HYPERLINK field | `[text](url)` | Percent-encode spaces or wrap dest in `<...>`; escape `(` `)` in URL as `%28`/`%29` or wrap; bare-URL text equal to target may emit autolink `<url>` (policy). Preserve inner formatting: `[**bold**](url)` |
| 22 | Internal hyperlink | `w:hyperlink w:anchor="bkmk"` | `[text](#anchor)` | If the bookmark sits at a heading, target the GFM auto-slug of that heading (lowercase; remove punctuation except `-` and `_`; spaces->`-`; dedupe duplicate slugs with `-1`, `-2`… suffixes); else emit `<a id="bkmk"></a>` at the bookmark site and link `#bkmk` |
| 23 | Image | §2.6 | `![alt](media/image1.png)` with files extracted to `<output>/media/` (or `<basename>_media/`) | Alt from `docPr/@descr`; escape `]` in alt, spaces in path -> `%20`. Extension from content type. Optional size via HTML `<img width=...>` when `wp:extent` matters (EMU/9525 = px at 96dpi). Anchored images: emit inline at anchor point |
| 24 | Footnote / endnote | references + parts | `[^1]` ... and `[^1]: text` definitions at end of document | Single global renumber 1..n in reference order (merge endnotes after footnotes, or prefix `[^en1]`). Multi-block notes: continuation lines indented 4 spaces |
| 25 | Horizontal rule | Empty-ish paragraph with `w:pBdr/w:bottom`; `v:rect o:hr="t"`; paragraph of only `---`/`***` text | `---` on its own line, blank lines around | A `---` line directly under text would be a setext H2 — always blank-line-separate |
| 26 | Line break in paragraph | `w:br` (textWrapping), `w:cr` | Backslash `\` at end of line (or two-space, policy; backslash survives editors that trim trailing whitespace) | Inside table cells: `<br>` instead |
| 27 | Page/column break | `w:br type=page/column`, `w:lastRenderedPageBreak` | Nothing (default); optional `---` or `<div style="page-break-after">` under a flag | lastRenderedPageBreak always ignored |
| 28 | Tab | `w:tab` in run | Single space (default) or literal tab under flag | Leading tabs are not code indication by themselves |
| 29 | Tracked changes | `w:ins`/`w:del`/`w:moveTo`/`w:moveFrom` | Accept-all: keep ins/moveTo content, drop del/moveFrom | Optional flag to render `~~del~~` / ins as `<ins>` |
| 30 | Comments | ranges + comments.xml | Drop (default); flag for footnote-style emission | |
| 31 | Content controls | `w:sdt` | Transparent recursion into sdtContent | TOC-gallery SDT skipped like TOC field |
| 32 | TOC | field / SDT | Skip (default); flag to keep cached text as plain lines | |
| 33 | Math | `m:oMath` | Linearized text or `$...$` (flagged) | |
| 34 | Soft hyphen | `w:softHyphen`, literal U+00AD in `w:t` | Remove | Also strip U+200B..U+200D zero-width chars (policy: keep ZWJ inside emoji sequences) |
| 35 | Non-breaking space | U+00A0 in text | Keep as U+00A0 (default, UTF-8 out) or `&nbsp;` under an ASCII flag | Treat NBSP as whitespace for delimiter placement: hoist it out of emphasis spans like an ordinary space (CommonMark ≥0.30 counts Zs characters, U+00A0 included, as Unicode whitespace for emphasis flanking — `**bold **` may not parse). Hoisting is safe in every renderer |
| 36 | Smart quotes / dashes / ellipsis | literal U+2018/19/1C/1D, U+2013/14, U+2026 | Keep verbatim (default); `--ascii-punct` flag to fold to `'"`, `--`, `...` | |
| 37 | caps / smallCaps | toggles | caps: uppercase text (policy) ; smallCaps: leave text as typed | |
| 38 | Text boxes / shapes (`wps:txbx`, `v:textbox`) | inside drawing/pict | Extract inner `w:txbxContent` blocks in place (default) or skip | Word cover pages live in text boxes |
| 39 | Headers/footers | sectPr references | Skip | |
| 40 | Empty paragraph | `w:p` with no visible content | Nothing (paragraph separation is the blank line between blocks); runs of N empty paragraphs collapse | Never emit `&nbsp;` filler |

---

## 4. Markdown escaping rules (GFM / CommonMark)

Escaping is **context-dependent**. A single "escape everything" pass produces backslash-littered output; no escaping produces corrupted documents. Backslash-escape works only on ASCII punctuation (`\!"#$%&'()*+,-./:;<=>?@[\]^_`{|}~`); a backslash before anything else is literal.

### 4.1 By context

| Context | Characters to escape | Rule |
|---|---|---|
| **Start of output line** (after any list/quote prefix) | `#` (ATX heading), `>` (quote), `-` `+` `*` (bullet, when followed by space/EOL), `=` (setext underline: line of only `=`), digits then `.` or `)` then space (`1. ` ordered list; escape the dot: `1998\.`), `|` (table row if inside/near table context) | Escape the *first* such character only when the pattern actually matches (e.g. `#x` needs no escape — `#` must be followed by space/EOL to head; but escaping defensively is harmless for `#`). A line consisting solely of `-`/`_`/`*` (3+) would be a thematic break; of `-`/`=` a setext heading for the previous line — escape first char |
| **Anywhere inline** | `*`, `_` (emphasis), `` ` `` (code span), `[`, `]` (links; `]` matters when a `[` is open — escape both unconditionally), `\` (as `\\`), `<` (HTML tag / autolink open — escape as `\<` or `&lt;` when followed by letter, `/`, `!`, `?`), `~` when doubled (`~~` strike) | `_` intraword is safe in GFM but escape anyway for portability, or only when word-adjacent boundaries make it active |
| **`&`** | Escape as `&amp;` only when followed by an entity pattern (`[a-zA-Z0-9]+;` or `#\d+;` / `#x[0-9a-f]+;`) | Otherwise literal `&` is fine |
| **Table cell** | Everything inline-level above, plus `|` -> `\|` everywhere in cell content, **including inside code spans** (GFM strips `\|` to `|` before inline parsing, so it works in code spans too). `&#124;` is an alternative outside code spans only — inside a code span the entity renders literally | Newlines forbidden -> `<br>` |
| **Link text `[...]`** | `]`, plus normal inline set | |
| **Link destination `(...)`** | space -> `%20` or wrap `<dest>`; `(` `)` -> `%28` `%29` or balanced or wrapped; `<` `>` inside wrapped form -> `%3C` `%3E`; control chars percent-encoded | Never backslash-escape URLs — percent-encode |
| **Image alt `![...]`** | Same as link text | |
| **Code span/fence content** | **NOTHING is escaped** — backslash is literal inside code | Collision handling only: choose delimiter run longer than any backtick run in content; space-pad span if content begins/ends with backtick or is all backticks; for fences, use more backticks than the longest internal ```` ``` ```` run (or `~~~` fences) |
| **Raw-HTML fallback content** (`<sup>` etc.) | Inner text still needs `&`/`<` HTML-escaping | |

### 4.2 Pitfalls

1. **No escaping inside code spans** — a literal backtick in monospace text cannot be backslash-escaped; only delimiter-length games work (see above).
2. **Autolinks**: `<https://x>` is a link; any `<word` risks being parsed as an HTML tag open and, in GFM, raw HTML passes through — text like `<T>` in prose must become `\<T>` or `&lt;T&gt;` or it disappears/breaks rendering.
3. **Escapes are markdown-global**: escaping `*` as `\*` inside what later becomes a code span corrupts it — decide code-span boundaries **before** escaping; escape only non-code segments.
4. **Adjacent emitted syntax**: text run `**` next to converter-emitted `**bold**` merges into `****bold**` — escape literal `*`/`_`/`~`/`` ` `` in text *always*, not only when "it looks dangerous", except inside code.
5. **Setext ambush**: any paragraph line of only `-` or `=` promotes the previous line to a heading. Blank lines between all blocks (which you should emit anyway) prevent this across blocks, but a hard line break *within* a paragraph followed by `---` text still triggers it — escape leading `-`/`=` after hard breaks too.
6. **Ordered-list ambush after run merging**: escape `digits.` / `digits)` + space only when they begin a line — a mid-line `3. ` is safe. Do the check after line assembly, not per-run.
7. **`!` before `[`**: `![` is an image; escape the `!` (`\![`) when literal text `!` precedes a link you are emitting or a literal `[`.
8. **Reference-link false positives**: `[foo]` followed by `[bar]` or `(x)` — unconditional `[`/`]` escaping already covers this.
9. **Trailing backslash**: a line-final literal `\` becomes a hard break — emit `\\`.

Recommended implementation: the emitter exposes `writeText(text, Context)` where Context ∈ {inline, lineStart, tableCell, linkText, linkDest, altText, codeSpan, codeBlock, html} and owns all escaping; walker code never concatenates raw strings into output.

---

## 5. Edge cases and correctness traps (ranked by real-world frequency)

### 5.1 Run fragmentation (hits ~every Word-authored file)

Word splits logical text into many `w:r` at spellcheck boundaries, cursor positions, and revision saves (`w:rsidR`/`w:rsidRPr` attributes on runs and paragraphs), plus `w:proofErr` markers between them. `Hello` frequently arrives as `Hel` + `lo`, mid-word. If you emit formatting delimiters per run you get `**Hel****lo**` (broken: `****` is not valid emphasis) or `*ital*` `*ic*`.

**Rule: after resolving each run's *effective* formatting (§5.2), merge adjacent runs whose effective formatting sets are identical, ignoring rsid/proofErr/bookmark/lastRenderedPageBreak boundaries, before any Markdown is emitted.** Merge across `w:ins` boundaries too (post-accept they are plain content). Do not merge across `w:hyperlink` boundaries or field-result boundaries.

### 5.2 Formatting resolution: the style hierarchy and toggle XOR

Effective run properties are computed per ISO 29500-1 §17.7.2/§17.7.3 by layering:

1. `w:docDefaults/w:rPrDefault`
2. the default paragraph style (`w:style w:default="1" w:type="paragraph"`) if no pStyle
3. paragraph style chain: follow `w:basedOn` to the root, apply root-first (both its `pPr` and its `rPr`)
4. numbering-level `rPr` (affects the number glyph only — skip)
5. character style chain (`w:rStyle`), basedOn root-first
6. direct paragraph `pPr` (for paragraph-level props)
7. direct run `rPr` — always wins

Non-toggle properties: nearest specification wins (later layers override earlier). **Toggle properties (`w:b`, `w:i`, `w:strike`, `w:caps`, `w:smallCaps`, `w:vanish`, etc.) have XOR semantics**: the effective value from the *style* layers is the XOR of every explicit `true` specification along the (paragraph style + character style) chains — an even count of trues cancels out (this is how "bold style applied to bold text yields non-bold" works). Exceptions per spec: if **direct formatting** (run `rPr`) specifies the toggle, that value is final (no XOR); if docDefaults sets it true, the effective value is true regardless of any style specifications — only direct run formatting can override it; docDefaults never participates in the XOR. Practical algorithm:

```
effective(t) = directRPr.has(t) ? directRPr.val(t)          // direct formatting is final
             : docDefaults.val(t) == true ? true            // docDefaults-true beats all styles
             : XOR of explicit val=true specifications across the applicable
               style hierarchies (pStyle chain, rStyle chain)
   (docDefaults is NEVER in the XOR: true short-circuits above; false/absent
    is the base, so odd count of style trues -> on, even -> off)
```
Also honor explicit `w:val="0"` in a style: it removes that style's contribution (specifies false — contributes 0 to the XOR count but *is* a specification for non-toggle semantics; for toggles, false simply doesn't flip). Parse OnOff values permissively: absent val = true; `0/false/off` = false; `1/true/on` = true.

Cycle-guard `basedOn` chains (malformed files exist); cap depth (e.g. 16). Unknown styleId -> treat as no style. Precompute and cache the resolved property set per styleId at load time.

### 5.3 Whitespace at run boundaries vs emphasis delimiters (very common)

Word happily stores `<w:r><w:rPr><w:b/></w:rPr><w:t xml:space="preserve">bold </w:t></w:r>` — trailing space *inside* the bold run. `**bold **text` does not parse as emphasis (CommonMark left/right-flanking rules: closing `**` cannot be preceded by whitespace; opening `**` cannot be followed by whitespace). **Rule: after coalescing, hoist leading/trailing whitespace out of every formatted span before wrapping**: `**bold** text`. A span that becomes empty or whitespace-only after hoisting emits its whitespace with no delimiters. Same rule for `*`, `~~`, `` ` `` (for code spans keep one interior pad space only for the backtick-collision case), and link text (leading/trailing spaces in `[ text ]` are legal but ugly — trim into surroundings). Hoist U+00A0 (NBSP) too: CommonMark ≥0.30 counts Zs characters as Unicode whitespace for emphasis flanking, so a closing `**` preceded by NBSP may not parse; moving the NBSP outside the delimiters is safe in every renderer.

### 5.4 List/numbering correctness (very common)

- Interleaved paragraphs: list -> plain paragraph -> list continuation with the same numId must continue numbering. Because you compute numbers yourself, this works — but the *Markdown* must keep them one list: indent the interrupting paragraph as item continuation when it belongs to the item (heuristic: matching `w:ind`), else accept the visual restart and emit the true number (`4.`) so GFM continues correctly (GFM respects the first number of a list).
- Restart = new numId + `startOverride`. Two visually identical adjacent lists that are distinct lists need a separator (`<!-- -->`) or they merge.
- numId referencing a missing num, num referencing a missing abstractNum, `numStyleLink` loops — all occur; degrade to bullets or plain text, never crash.
- Level skip (ilvl jumps 0 -> 2): emit intermediate indentation anyway (one MD level per ilvl step is fine to normalize).
- Headings with numbering (multilevel heading numbering): heading wins; drop or inline the number text.
- `ListParagraph` style with no numPr anywhere: it is *not* a list — plain paragraph (LibreOffice/Word both produce these).

### 5.5 Empty runs, empty paragraphs, delimiter-only artifacts (very common)

Runs with rPr but no text, `w:t` empty elements, paragraphs containing only bookmarks/proofErr. All must contribute nothing — guard the emitter so `**` `**` around empty content is impossible (emit delimiters only when the coalesced span has non-whitespace content). Collapse consecutive empty paragraphs; ensure exactly one blank line between blocks.

### 5.6 Hyperlinks wrapping formatted runs (common)

`w:hyperlink` children carry their own formatting (typically rStyle `Hyperlink` — which resolves to underline+blue; **suppress that specific style's underline/color rather than emitting `<u>`**). Coalesce runs *within* the hyperlink; formatting goes inside the brackets: `[**bold link**](url)`. Empty hyperlinks (no text) are skipped. Hyperlink containing an image -> `[![alt](img)](url)`.

### 5.7 Fields (common: TOC in most business docs, HYPERLINK in older files)

Unclosed fields (begin without end — truncated/corrupt files): treat EOF/paragraph-stream end as implicit end, emit accumulated result. Nested fields: stack (§2.7). Fields spanning paragraphs (TOC): the suppression state must live in the walker, not per-paragraph.

### 5.8 Images: anchored vs inline, AlternateContent (common)

Floating `wp:anchor` images sometimes sit in their own empty paragraph, sometimes mid-sentence; emit at anchor point either way. Behind `mc:AlternateContent` you must pick Choice-you-understand-else-Fallback or you'll double-emit (both branches contain the image) or zero-emit. Deduplicate media by relationship target (same image referenced twice -> one extracted file). Dedup output filenames case-insensitively (Windows FS).

### 5.9 Tables: merges, nesting, header inference (common)

Covered in §2.5/§3 rows 18-20. Additional traps: a table as the *first* body element (no preceding paragraph); table immediately followed by table (blank line between); a `w:p` **must** follow the last `w:tbl` in body/cell per spec — but don't rely on it; cells whose only paragraph is empty still need `|  |`; text starting with `-`/`digits.` inside cells doesn't need lineStart escaping (cells aren't line starts) but does need `|` escaping.

### 5.10 Producer variance (common when inputs are heterogeneous)

| Producer | Signature quirks |
|---|---|
| **Word (desktop)** | Heavy run fragmentation, rsids, proofErr, AlternateContent around drawings, latentStyles, `w:tblLook` bitmask+attributes, TOC as SDT+field |
| **Word Online** | Similar; more `w14`/`w15` extension markup (ignore via mc:Ignorable) |
| **Google Docs export** | Minimal styles.xml (often direct formatting for everything, including headings that *do* use `HeadingN` styles but bold/size directly); no rsids; single-run paragraphs; numbering.xml with one abstractNum per list and bullet glyphs `●`/`○`/`■` in lvlText; images sometimes in `word/media` with generic names; hyperlinks always `w:hyperlink`; no docDefaults surprises; may omit settings.xml parts |
| **LibreOffice** | Styles named `Heading 1` (space) with styleId `Heading1`; `Preformatted Text`, `Source_20_Text` (`_20_` = escaped space in styleId), `Internet Link` character style for hyperlinks; `Text Body` default; VML sometimes for images in older versions; `ListParagraph` absent — own `ListNumber`/`ListBullet` styles carrying numPr |
| **Pandoc-generated** | Styles `Body Text`, `Compact`, `Source Code`, `Verbatim Char`, `First Paragraph`; clean single runs; footnotes standard |
| Generic exporters | Missing optional parts (no styles.xml/settings.xml — all defaults), `w:t` without `xml:space` but with meaningful edge spaces, numbering referencing absent parts |

Match styles by **normalized name** (lowercase, collapse spaces, decode `_20_`-style styleId escapes) *and* styleId; treat every optional part as optional.

### 5.11 Tracked changes and w:delText (occasional)

Files saved mid-review are everywhere in business settings. Accept-all (§2.1): `w:ins` transparent, `w:del` (and its `w:delText`, `w:delInstrText`) dropped, `w:pPrChange`/`w:rPrChange`/`w:sectPrChange`/`w:tblPrChange` ignored (current props are the accepted state), deleted paragraph marks (`w:rPr/w:del` on the *paragraph mark* run inside pPr) mean the paragraph merges with the next — honor it or accept a spurious paragraph break (honoring: if pPr/rPr contains w:del, join with following paragraph).

### 5.12 Malformed and hostile inputs (must-handle for a console tool)

- **ZIP bombs**: enforce per-entry and total decompressed byte caps (e.g. 512 MB total, configurable), compression-ratio cap (e.g. reject >1000:1 on multi-MB entries), entry-count cap (e.g. 10k). Stream-decompress with caps; never trust the central-directory size fields alone (verify while inflating).
- **Path traversal**: ZIP entry names and relationship Targets containing `../`, leading `/`, `\`, drive letters (`C:`), NTFS ADS (`name:stream`). Resolve part names purely inside an in-memory package map; when extracting media, generate output filenames yourself (`image1.png`, `image2.jpg` ...) — **never** reuse archive entry paths on disk.
- Duplicate ZIP entry names (last-wins vs first-wins ambiguity — pick central-directory order, document it), ZIP64, data descriptors, encrypted entries (reject with a clear message; password-protected docx is actually an OLE/CFB `EncryptedPackage` — detect the `D0 CF 11 E0` magic and report "encrypted document"), files that aren't ZIP at all (`.doc` binary — magic `D0 CF 11 E0`; report "legacy .doc not supported").
- XML: forbid DTDs/external entities (XXE/billion-laughs) — configure the parser to reject `<!DOCTYPE`; cap element nesting depth; tolerate unknown elements/attributes everywhere (skip subtree) per the OOXML compatibility model; tolerate missing optional attributes with spec defaults.
- Encoding: parts are UTF-8 (occasionally UTF-16 with BOM — honor the XML declaration/BOM). Validate UTF-8; replace invalid sequences with U+FFFD rather than aborting.

---

## 6. Recommended C++ pipeline (native, no external converter tools)

### 6.1 Stage overview

```
 [1] ZIP reader          raw bytes -> part map {part name -> bytes}   (security caps here)
 [2] XML parse           per needed part -> in-memory DOM             (namespace-URI aware)
 [3] Package model       [Content_Types], rels graphs, part lookup    (r:id resolution here)
 [4] Style model         styles.xml -> resolved-props cache           (basedOn chains, toggles, docDefaults)
 [5] Numbering model     numbering.xml -> numId->levels w/ overrides  (numStyleLink, startOverride)
 [6] Document walk       document.xml (+footnotes/endnotes) -> IR     (fields, tracked changes, sdt,
                                                                       AlternateContent, bookmarks)
 [7] Run resolution      per run: effective props via [4]             (toggle XOR)
 [8] Coalescing pass     IR spans merged on identical effective props (rsid splits die here;
                                                                       whitespace hoisting)
 [9] Structure pass      list counters, table normalization,          (numbering state per abstractNum,
                         footnote renumbering, anchor assignment       grid padding, header inference)
[10] MD emitter          IR -> UTF-8 text via context-aware writer    (all escaping; blank-line
                                                                       management)
[11] Media extraction    referenced image parts -> output dir          (content-type extensions, dedup,
                                                                       safe generated names)
```

### 6.2 Stage notes and edge-case ownership

**[1] ZIP** (own minimal reader or vendored miniz/minizip-ng): parse End of Central Directory (search the last 65,557 bytes — 22-byte EOCD record + max 65,535-byte comment — or the whole file if smaller, for `PK\x05\x06`), central directory, support ZIP64, stored+deflate methods only. Owns: bombs, traversal names, duplicates, encryption detection, .doc/OLE detection (§5.12). Load lazily: only parts actually referenced get inflated (media stays compressed until extraction).

**[2] XML**: the parser choice is decision **D2 in CLAUDE.md** (recommendation there: hand-rolled repo-owned `XmlPull` streaming pull parser; the vendored alternative is pugixml — single .cpp/.hpp pair, MIT, compiles clean under MSVC v143). Whichever is chosen, the requirements are: namespace resolution to URIs (with the Strict/Transitional alias table), `<!DOCTYPE` rejection, depth cap, attribute defaulting left to callers. Note the shape tension honestly: field/table/list logic needs lookahead and parent context, so a pure event stream is painful — with a pull parser, either build a thin repo-owned tree for the parts that need it (document.xml is tens of MB at most for huge files) or keep explicit context stacks in the walker. A pull tokenizer feeding a thin DOM combines both.

**[3] Package model**: `Package` class: `getPart(name)`, `getRels(partName)`, `resolveRel(partName, rId) -> {targetPartName | externalUrl}`, `contentTypeOf(name)`. Owns: main-part discovery via `_rels/.rels` with content-type cross-check, part-name normalization (case: OPC part names are case-insensitive for comparison — normalize to lowercase keys but preserve original), relative-target resolution.

**[4] Style model**: build `unordered_map<styleId, ResolvedStyle>` where ResolvedStyle = flattened paragraph props + run props + ordered toggle-specification list (needed for XOR at run time), plus normalized-name index for heading/quote/code detection. Owns: cycle guard, defaults style, docDefaults, name normalization (§5.10).

**[5] Numbering model**: `NumberingDef lookup(numId) -> {abstractId, level[9]}` with overrides applied and numStyleLink chased. Runtime counter table keyed `(abstractNumId, ilvl)` with pending `startOverride` resets keyed by numId (applied the first time that numId is used). Owns: dangling refs, restart, lvlRestart.

**[6] Walker -> IR**: define a small intermediate representation — do *not* emit Markdown during the walk:

```cpp
Block = Paragraph {styleInfo, listInfo?, spans}   Span = Text{str, Fmt}
      | Table {grid, rows[cells[Block*]]}              | LineBreak
      | ... (HR, CodeBlock, Quote wrap via styleInfo)  | Image{src, alt, w, h}
                                                       | LinkStart{dest}/LinkEnd
                                                       | NoteRef{idx}  | Anchor{name}
Fmt = bitset{bold, italic, strike, code, sup, sub, underline, highlight...}
```
The walker owns: sdt/smartTag/customXml transparency, ins/del policy, AlternateContent choice, field state machine (suppression + HYPERLINK/REF synthesis + TOC skip), bookmark recording, footnote/endnote reference collection (bodies walked with the same walker recursively), hidden-run dropping, sym mapping, br/tab/softHyphen handling, mid-body sectPr, deleted-paragraph-mark joining.

**[7]+[8] Resolution & coalescing**: for each paragraph, map each run to `(Fmt, text)` using stage-4 cache + direct rPr (toggle XOR at this point), then merge adjacent spans with equal Fmt and hoist leading/trailing whitespace (incl. NBSP) out of each formatted span (§5.3). Also classify paragraph type here: heading level, quote, code-paragraph (all-spans-monospace), list item (via stage 5), HR (pBdr pattern). Owns: §5.1, §5.2, §5.3, §5.5 (drop empties before and after merge).

**[9] Structure pass**: assign list numbers (counters), compute nesting indents, group consecutive code paragraphs into one CodeBlock, group consecutive same-list items, decide table header rows, pad table grids (gridSpan/vMerge policy), renumber footnotes/endnotes into final `[^n]` order, resolve internal-link targets (bookmark -> heading slug or explicit anchor), plan list-separator comments. Owns: §5.4, §5.9, row 22 slug generation (implement GFM slugger: lowercase; remove punctuation except `-` and `_`; spaces->`-`; dedupe duplicates with `-N` suffixes).

**[10] Emitter**: single writer with the Context-typed `writeText` (§4.2 recommendation), delimiter selection (`*`/`**`/`~~`/backtick-run sizing), hard-break rendering (`\` vs `<br>` by context), blank-line discipline (exactly one blank line between blocks; none inside list items except before nested blocks; `> ` prefixing distributed across quoted block lines), line-start escape pass on each completed line, trailing-whitespace policy. Output UTF-8, LF line endings, no BOM (flags for CRLF/BOM if desired — relevant on Windows/MSVC where the tool runs; open output with `"wb"` to control line endings explicitly).

**[11] Media**: extract each image part referenced by an emitted Image span; name `image1.ext` sequentially or keep sanitized basenames; write to a media dir next to the .md; rewrite spans' src to the relative path with `%20` encoding. Owns: content-type extension mapping, dedup, EMF/WMF warning.

### 6.3 Source decomposition

The planned module layout is defined in the repo-root `CLAUDE.md` ("Planned architecture") — that
list is authoritative; stages map onto it as: [1]→`ZipReader`, [2]→`XmlPull`, [3]→`OpcPackage`,
[4]→`StyleModel`, [5]→`NumberingModel`, [6]→`DocWalker`+`Ir`, [7]+[8]→`RunCoalescer`,
[9]→`RunCoalescer`/`MdEmitter`, [10]→`MdEscape`+`MdEmitter`, [11]→`MediaExtractor`.

CLI surface worth planning for (each maps to a policy called out above): `--accept-changes/--show-changes`, `--underline=drop|u`, `--highlight=drop|mark`, `--tables=gfm|html-on-merge`, `--toc=skip|keep`, `--comments=drop|footnotes`, `--media-dir=`, `--hard-break=backslash|spaces`, `--ascii-punct`, `--max-decompressed=`, `--headings=atx` (only atx; setext not worth it).

### 6.4 Testing corpus guidance

Build fixtures per §5 rank: (1) Word file with mid-word run splits + rsids; (2) styles exercising basedOn chains and double-applied bold (toggle XOR); (3) trailing-space-inside-bold; (4) restarting/interleaved/mixed-level lists; (5) merged-cell and nested tables; (6) TOC + hyperlink fields (both syntaxes); (7) footnotes with multi-paragraph bodies and images; (8) tracked-changes document; (9) Google Docs, LibreOffice, and Pandoc exports of the same content; (10) hostile zips (traversal names, bomb, encrypted, `.doc`). Round-trip sanity check: converted output re-rendered by cmark-gfm (or GitHub) and eyeballed against Word's rendering.

---

### Key numeric constants

| Unit | Value |
|---|---|
| Twip (dxa) | 1/20 pt; 1440/inch; 720 = 0.5" (default indent step) |
| Half-point (`w:sz`) | sz 24 = 12 pt |
| EMU | 914400/inch; 9525/px at 96 dpi |
| Eighth-point (border `w:sz`) | 8 = 1 pt |
| ilvl range | 0-8 (9 levels) |
| Footnote separator ids | type attr `separator`/`continuationSeparator` (conventionally id 0/1 or -1/0 — trust the type attribute, not the id) |

All part paths above are conventional; the only guaranteed entry points are `[Content_Types].xml` and `_rels/.rels`. Everything else resolves through relationships — build the converter that way and every producer's files work.
