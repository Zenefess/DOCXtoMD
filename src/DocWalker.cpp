/*
 * File: DocWalker.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-22
 * Description: The body walk: wrappers, paragraph classification, runs and run content into the IR.
 * To Do: 1) Choose an understood mc:Choice by its Requires prefix once an extension namespace is understood,
 *           and honour the mc:Ignorable and mc:ProcessContent *attributes*, which nothing reads today.
 *        2) Uppercase beyond ASCII and Latin-1 for w:caps, which needs Unicode's case tables.
 *        3) Linearize m:oMath and map w:sym, both of which are skipped whole and so lose their text.
 *        4) Read an mc:AlternateContent inside a run as a branch rather than as a picture, for the rare
 *           one that carries text: today it is scanned for a picture reference and otherwise dropped.
 *        5) Cache more than one paragraph style if a document is ever found alternating between many.
 *        6) Read a paragraph's w:shd as the code hint CONVERSION_REFERENCE 2.3 names beside w:rFonts.
 * Dependencies: BuildGuards.h, DocWalker.h, Ir.h, OpcPackage.h, StyleModel.h, Utf.h, XmlPull.h,
 *               typedefs.h, memory management.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros, and memory management.h pulls those two in that order itself.
#include <windows.h>
#include "typedefs.h"
#include "memory management.h"
#include "Ir.h"
#include "OpcPackage.h"
#include "StyleModel.h"
#include "Utf.h"
#include "XmlPull.h"
#include "DocWalker.h"

//-- Tables

// One sentence per WALK_RESULT, in enumeration order.
static constexpr cchptr WALK_RESULT_TEXT[] = {
    "the document part was walked",                           // WALK_OK
    "not enough memory to hold the converted document",       // WALK_ERROR_MEMORY
    "the main document part could not be read",               // WALK_ERROR_PART
    "the main document part is not well-formed XML",          // WALK_ERROR_XML
    "the main document part's root element is not w:document" // WALK_ERROR_ROOT
};

static_assert(sizeof(WALK_RESULT_TEXT) / sizeof(WALK_RESULT_TEXT[0]) == ui64(WALK_RESULT_COUNT),
              "DocWalker: the result sentence table and the WALK_RESULT enumeration have drifted apart.");

//-- Walk state

// Where in the document's shape a child list is being read. The transparent wrappers -- w:ins, w:sdt,
// w:smartTag, w:customXml, mc:AlternateContent -- appear at both levels and are handled once for both.
enum DOC_LEVEL : si32 {
   DOC_LEVEL_BLOCK = 0, ///< Children are block items: paragraphs, tables, wrappers
   DOC_LEVEL_RUN,       ///< Children are run-level items: runs, hyperlinks, wrappers
   DOC_LEVEL_TABLE,     ///< Children are a table's rows
   DOC_LEVEL_ROW        ///< Children are a row's cells
};

typedef const DOC_LEVEL cDOC_LEVEL;

// How many bytes of one bookmark name are kept when it has to be held across the start of a block. Word
// caps a bookmark at forty characters and generates its own -- _GoBack, _Toc0000 -- well inside that.
constexpr cui64 DOC_ANCHOR_BYTES = 128u;

// How many bookmarks standing between blocks are held for the block that follows them. A bookmark is
// almost always written inside the paragraph it marks; this is the legal-but-rare other placement, and
// the ceiling is what stops a part making the walker's own stack frame a function of its content.
constexpr cui32 DOC_PENDING_ANCHORS = 8u;

// What one paragraph's w:numPr named. Both fields are -1 when it named nothing, and 0 is a value and
// not an absence: w:numId 0 is how a paragraph cancels numbering its style chain supplied.
struct DOC_NUM_REF {
   si32 numId; ///< w:numPr/w:numId exactly as written, 0 included; -1 when the paragraph named none
   si32 level; ///< w:numPr/w:ilvl, clamped to 0 to 8; -1 when the paragraph named none
};

typedef DOC_NUM_REF *const DOC_NUM_REFptrc;

// Everything one walk carries. One worker owns one of these on its own stack and never shares it (D6).
//
// The five table fields are the open table, the open row, and the tails of their two chains. They are
// here rather than on the walk's own frames because the transparent wrappers are dispatched by one
// function for every level, so a w:sdt or an mc:AlternateContent standing between a w:tbl and its w:tr
// has to reach the same state a direct child would. Each is saved and restored where a table nests --
// and the two tails are what an unwound mc:Choice restores, which is the whole of what a rewind inside
// a table costs: see IrBeginRow's note on why this module owns the chain and Ir.cpp does not.
struct DOC_CONTEXT {
   IR_DOCUMENTptr  document;                                       ///< Where blocks and spans are being built
   cSTYLE_MODELptr styles;                                         ///< The resolved style cache
   cNUM_MODELptr   numbering;                                      ///< The numbering model, read only to ask whether a numId resolves
   XML_READERptr   reader;                                         ///< The tokenizer over the part
   si32            cachedStyle;                                    ///< What cachedId resolved to, or -1
   char            cachedId[STYLE_MAX_NAME_BYTES];                 ///< The last w:pStyle or w:rStyle value looked up
   char            pending[DOC_PENDING_ANCHORS][DOC_ANCHOR_BYTES]; ///< Bookmarks awaiting the next block
   ui64            pendingLength[DOC_PENDING_ANCHORS];             ///< How long each of them is
   ui32            pendingCount;                                   ///< How many are waiting
   si32            table;                                          ///< The table whose rows are being read, or -1
   si32            row;                                            ///< The row whose cells are being read, or -1
   si32            lastRow;                                        ///< The last row appended to that table, or -1
   si32            lastCell;                                       ///< The last cell appended to that row, or -1
   ui32            depth;                                          ///< How many tables are open around the walk
   IR_ALIGN        justify;                                        ///< What the first aligned paragraph of the open cell said
   bool            inLink;                                         ///< Whether a hyperlink is open around the content
   bool            sawText;                                        ///< Whether the paragraph being walked produced any text
   bool            allMono;                                        ///< Whether every text-bearing run of it was monospace
   bool            memory;                                         ///< Whether an allocation failed; sticky once set
};

typedef DOC_CONTEXT *const DOC_CONTEXTptrc;

static cbool DocWalkChildren(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading);
static cbool DocDispatchChild(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading);
// A table is block content, so DocDispatchChild reaches it; it is declared beside the row and cell
// walks it belongs with, further down, rather than being lifted above the paragraph walk it follows.
static cbool DocWalkTable(DOC_CONTEXTptrc context);
// A picture is run content, so DocWalkRun reaches it; it is declared beside the link and bookmark
// helpers it belongs with, further down, rather than being lifted above the run walk it serves.
static cbool DocWalkImage(DOC_CONTEXTptrc context);

//-- Small helpers

// Copies a view into a NUL-terminated buffer, and reports how many bytes it wrote. Every view the reader
// hands out dies on the next XmlNext call, and a style identifier has to outlive the lookup that follows
// it. The length comes back because a caller that copies the buffer on again must not read past it: the
// bytes after the terminator were never written, and reading one is indeterminate.
static cui64 DocCopyView(cXML_TEXT text, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   if(text.bytes) {
      while(used + 1u < destBytes && used < text.length) {
         dest[used] = text.bytes[used];
         ++used;
      }
   }
   dest[used] = 0;
   return used;
}

// Whether the element the reader is on is one of a nullptr-terminated table of names, in one namespace.
// Matching by name is what keeps an element this build has never heard of from voting on anything.
static cbool DocIsNamed(XML_READERptrc reader, cXML_NS space, cchptrcptr names) {
   for(ui64 index = 0; names[index]; ++index) {
      if(XmlIsElement(reader, space, names[index])) return true;
   }
   return false;
}

// Resolves a style identifier, remembering the last one. Documents reuse a handful of styles over
// thousands of paragraphs, so one cached answer removes almost every linear scan of the style table.
static csi32 DocFindStyle(DOC_CONTEXTptrc context, cXML_TEXT value) {
   char  identifier[STYLE_MAX_NAME_BYTES];
   cui64 length = DocCopyView(value, identifier, sizeof(identifier));

   if(!length) return -1;

   ui64 index = 0;

   while(context->cachedId[index] && context->cachedId[index] == identifier[index]) ++index;
   if(context->cachedId[index] == identifier[index]) return context->cachedStyle;
   // The terminator is copied and nothing past it: the rest of the buffer was never written.
   for(index = 0; index <= length; ++index) context->cachedId[index] = identifier[index];
   context->cachedStyle = StyleFind(context->styles, identifier);
   return context->cachedStyle;
}

// Uppercases one byte pair in place, for the w:caps transform, and reports how many bytes it consumed.
// Two ranges are handled and no more: the ASCII letters, and the Latin-1 supplement's lowercase letters,
// whose uppercase forms sit exactly 0x20 below them. Everything beyond those needs Unicode's case tables,
// which this project does not carry -- see the To Do. The two exclusions are deliberate: U+00DF grows to
// two characters when uppercased and U+00FF's uppercase is not 0x20 away, so neither is touched.
static cui64 DocUpperOne(cchptr bytes, cui64 byteCount, ui8ptrc dest) {
   cui8 lead = ui8(bytes[0]);

   if(lead >= 'a' && lead <= 'z') {
      dest[0] = ui8(lead - 'a' + 'A');
      return 1u;
   }
   if(lead == 0xC3u && byteCount >= 2u) {
      cui8 next = ui8(bytes[1]);

      dest[0] = lead;
      dest[1] = ui8(next >= 0xA0u && next <= 0xBEu && next != 0xB7u ? next - 0x20u : next);
      return 2u;
   }
   dest[0] = lead;
   return 1u;
}

// Whether a range holds anything that would put a visible character on the line, which is what makes a
// run count as content for row 12's vote.
//
// The question is about what the run will *contribute*, not about the bytes it arrived as, so this has
// to agree with DocAppendText below: a CR or an LF inside a w:t is interior whitespace and folds to one
// space there, and a U+00AD is dropped outright, so a run made only of those adds nothing a reader can
// see and must not settle a paragraph either way. Word gives a hyphenation point from a later editing
// session its own w:r, and a soft-hyphen-only run breaking a fence is exactly the fragmentation
// correctness rule 4 exists to absorb.
//
// Whitespace here is the tab and the Zs category, the same class RunCoalescer hoists and MdEmitter
// flanks on, with **one exclusion**: U+00A0 is content, per mapping row 35, so a run of one settles the
// paragraph like any visible character. That is the asymmetry CLAUDE.md records against IrEndBlock, and
// it is deliberate here for the same reason -- a non-breaking space is a typographic act rather than
// the incidental gap Word leaves between two runs it split at an rsid boundary.
static cui64 DocInvisibleAt(cchptr bytes, cui64 at, cui64 byteCount) {
   cui8 lead = ui8(bytes[at]);

   if(lead == ' ' || lead == '\t' || lead == '\r' || lead == '\n') return 1u; // U+0020, U+0009, and the
   if(at + 1u >= byteCount) return 0;                                         //   line ends that fold

   cui8 second = ui8(bytes[at + 1u]);

   if(lead == 0xC2u && second == 0xADu) return 2u; // U+00AD, dropped outright
   if(at + 2u >= byteCount) return 0;

   cui8 third = ui8(bytes[at + 2u]);

   if(lead == 0xE1u && second == 0x9Au && third == 0x80u) return 3u; // U+1680
   if(lead == 0xE3u && second == 0x80u && third == 0x80u) return 3u; // U+3000
   if(lead == 0xE2u && second == 0x81u && third == 0x9Fu) return 3u; // U+205F
   if(lead != 0xE2u || second != 0x80u) return 0;
   if(third == 0xAFu) return 3u;                       // U+202F
   return (third >= 0x80u && third <= 0x8Au ? 3u : 0); // U+2000..U+200A
}

static cbool DocIsSolid(cchptr bytes, cui64 byteCount) {
   ui64 index = 0;

   while(index < byteCount) {
      cui64 width = DocInvisibleAt(bytes, index, byteCount);

      if(!width) return true;
      index += width;
   }
   return false;
}

// Appends text to the open span, dropping the soft hyphens CONVERSION_REFERENCE row 34 says to remove
// and uppercasing when row 37's w:caps is in force. U+00AD is invisible and splits a word wherever a
// renderer decides not to hyphenate, so keeping one corrupts the word for every reader that does not.
static cbool DocAppendText(DOC_CONTEXTptrc context, cchptr bytes, cui64 byteCount, cbool upper, boolptrc solid) {
   ui64 run = 0;

   if(DocIsSolid(bytes, byteCount)) *solid = true;

   for(ui64 index = 0; index < byteCount; ++index) {
      cbool soft = (ui8(bytes[index]) == 0xC2u && index + 1u < byteCount && ui8(bytes[index + 1u]) == 0xADu);
      cbool ends = (bytes[index] == '\n' || bytes[index] == '\r');

      // A line end inside a w:t is interior whitespace and not a break: WordprocessingML spells a break
      // w:br. Emitting the byte would end the Markdown block it stands in -- a heading would gain a
      // second line, a paragraph would become two -- so it folds to one space. A carriage return can
      // only arrive as a character reference, which XML says is not line-end normalised; a pair of them
      // is one line end and becomes one space.
      if(ends) {
         if(run && !IrAppendText(context->document, bytes + index - run, run)) return false;
         run = 0;
         if(bytes[index] == '\r' && index + 1u < byteCount && bytes[index + 1u] == '\n') ++index;
         if(!IrAppendText(context->document, " ", 1u)) return false;
         continue;
      }
      if(!soft) {
         if(!upper) {
            ++run;
            continue;
         }

         ui8   folded[UTF_MAX_ENCODED];
         cui64 took = DocUpperOne(bytes + index, byteCount - index, folded);

         if(!IrAppendText(context->document, (cchptr)folded, took)) return false;
         index += took - 1u;
         continue;
      }
      if(run && !IrAppendText(context->document, bytes + index - run, run)) return false;
      run = 0;
      ++index; // Step over the continuation byte as well as the lead
   }
   if(run && !IrAppendText(context->document, bytes + byteCount - run, run)) return false;
   return true;
}

//-- Run properties

// Reads the w:rPr the reader is on into a direct-formatting record, and consumes it. The property
// vocabulary itself lives in StyleModel, so a style's w:rPr and a run's own w:rPr can never disagree
// about what an element means; only w:rStyle is handled here, because resolving it needs the model.
static cbool DocReadRunProperties(DOC_CONTEXTptrc context, STYLE_DIRECT_RUNptrc direct) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!XmlIsElement(context->reader, XML_NS_W, "rStyle")) {
         StyleReadDirectProperty(context->reader, direct);
      } else {
         direct->characterStyle = DocFindStyle(context, XmlAttribute(context->reader, XML_NS_W, "val"));
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Records that the paragraph being walked has produced text, and whether that run was monospace.
// CONVERSION_REFERENCE row 12's code-block heuristic is "every run is monospace", so a single run that
// is not settles the paragraph, and a run that produces no text at all must not vote either way.
//
// Nor may a run that produced nothing but whitespace. Word splits a logical run at every rsid boundary
// and the space *between* two monospace runs routinely lands in the body font, so counting it would
// break the fence on exactly the fragmentation correctness rule 4 exists to absorb -- and a space
// renders identically in every face, so ignoring it loses nothing at all.
static void DocNoteRunText(DOC_CONTEXTptrc context, cbool mono) {
   context->sawText = true;
   if(!mono) context->allMono = false;
}

// Turns resolved WordprocessingML run properties into the output model's formatting bits. Only what a
// Markdown delimiter could express survives: the other eight toggles have no mapping and are dropped.
static cui32 DocFormatBits(cSTYLE_RUN_PROPS props) {
   ui32 bits = IR_FMT_NONE;

   // The complex-script twins fold into the same bit rather than getting their own. Word writes w:b and
   // w:bCs together whenever a user bolds anything, so a run carrying only w:bCs is complex-script text
   // that really is bold; giving it a separate bit would only stop M6's coalescer merging two runs that
   // render identically, which is the fragmentation bug of correctness rule 4.
   cui16 bold   = ui16(StyleToggleBit(STYLE_TOGGLE_BOLD) | StyleToggleBit(STYLE_TOGGLE_BOLD_CS));
   cui16 italic = ui16(StyleToggleBit(STYLE_TOGGLE_ITALIC) | StyleToggleBit(STYLE_TOGGLE_ITALIC_CS));

   if(props.toggles & bold) bits |= IR_FMT_BOLD;
   if(props.toggles & italic) bits |= IR_FMT_ITALIC;
   if((props.toggles & StyleToggleBit(STYLE_TOGGLE_STRIKE)) || props.doubleStrike) bits |= IR_FMT_STRIKE;
   if(props.vertAlign == STYLE_VERT_SUPERSCRIPT) bits |= IR_FMT_SUPER;
   if(props.vertAlign == STYLE_VERT_SUBSCRIPT) bits |= IR_FMT_SUB;
   // Both halves of CONVERSION_REFERENCE row 11 land on one bit, because two runs that render as the
   // same code span have to coalesce: one carrying a Code character style and one merely set in a
   // monospace family are indistinguishable in the output, and correctness rule 4 is about the output.
   if(props.codeStyle || props.monospace) bits |= IR_FMT_CODE;
   // Row 11 rules that code wins over bold and italic, and the bit is cleared here rather than at
   // emission for the same reason the complex-script twins share one: two runs that come out as the
   // same code span must merge, and a bold one beside a plain one would not. Left set, their two
   // backtick delimiters would meet and a renderer would read the pair as one span with backticks in it.
   if(bits & IR_FMT_CODE) bits &= ~(IR_FMT_BOLD | IR_FMT_ITALIC);
   return bits;
}

//-- Runs

// Reads the text of the w:t the reader is on into the open span.
static cbool DocReadTextElement(DOC_CONTEXTptrc context, cbool upper, boolptrc solid) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token == XML_TOKEN_TEXT) {
         // One w:t can arrive as several text tokens, because a comment or a processing instruction ends
         // a run of character data. Nothing here trims: xml:space is the producer's business, and
         // CONVERSION_REFERENCE 2.2 says to parse a w:t literally either way.
         if(!DocAppendText(context, context->reader->text.bytes, context->reader->text.length, upper, solid)) {
            context->memory = true;
            return false;
         }
         continue;
      }
      if(token == XML_TOKEN_START_ELEMENT && !XmlSkipElement(context->reader)) return false;
   }
}

// Walks one w:r, emitting spans for whatever content it carries.
// The four shapes a picture arrives in. mc:AlternateContent belongs on the list because inside a run
// it is what Word writes around a drawing that has a VML fallback -- but it is matched separately,
// because it is the one of the four that is not in the WordprocessingML namespace.
static constexpr cchptr DOC_PICTURES[] = {"drawing", "pict", "object", nullptr};

static cbool DocWalkRun(DOC_CONTEXTptrc context, csi32 paragraphStyle, cbool heading) {
   cui32            depthHere = context->reader->depth;
   STYLE_DIRECT_RUN direct;
   bool             textOpen = false;
   bool             resolved = false;
   ui32             bits     = IR_FMT_NONE;
   bool             hidden   = false;
   bool             upper    = false;
   bool             mono     = false;
   bool             solid    = false; // Whether this run produced a byte that is neither space nor tab

   StyleClearDirect(&direct);
   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) {
         // The vote is taken once the whole run is read, because a run that produced nothing but
         // whitespace must not settle row 12's "every run is monospace" either way.
         if(solid) DocNoteRunText(context, mono);
         return true;
      }
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!resolved && XmlIsElement(context->reader, XML_NS_W, "rPr")) {
         if(!DocReadRunProperties(context, &direct)) return false;
         continue;
      }

      // w:rPr is the first child when it is present at all, so the first non-property child is where the
      // run's formatting is settled once and for the whole run.
      if(!resolved) {
         cSTYLE_RUN_PROPS props = StyleResolveRun(context->styles, paragraphStyle, &direct);

         // Hidden text is omitted whole whichever property says so. w:vanish is a toggle and
         // w:webHidden is not, but CONVERSION_REFERENCE 2.3 drops a run for either.
         hidden = ((props.toggles & StyleToggleBit(STYLE_TOGGLE_VANISH)) != 0) || props.webHidden;
         // Row 37: caps uppercases the text, smallCaps leaves it as typed. It is a transform on the
         // bytes rather than a delimiter, so it happens here, where the bytes are copied.
         upper = (props.toggles & StyleToggleBit(STYLE_TOGGLE_CAPS)) != 0;
         bits  = DocFormatBits(props);
         // Row 12's code-block heuristic is stated over the font alone, so it is props.monospace that is
         // remembered here and not the IR_FMT_CODE bit: a paragraph of runs wearing a Code *character*
         // style is an ordinary paragraph holding code spans, not a fenced block.
         mono = props.monospace;
         // A heading's bold is style-borne, and CLAUDE.md's mapping row 1 rules that heading text is
         // never additionally bolded. The bit is cleared here because IR_FMT is the only channel the
         // emitter has: left set, M6 would wrap every heading in delimiters it already carries.
         if(heading) bits &= ~IR_FMT_BOLD;
         resolved = true;
      }
      if(hidden) {
         // Hidden text is omitted whole, which is CONVERSION_REFERENCE row 10. Word hides the
         // instruction half of a field this way, so emitting it would put field codes in the document.
         if(!XmlSkipElement(context->reader)) return false;
         continue;
      }
      if(XmlIsElement(context->reader, XML_NS_W, "t")) {
         if(!textOpen) {
            if(!IrAddSpan(context->document, IR_SPAN_TEXT, bits)) {
               context->memory = true;
               return false;
            }
            textOpen = true;
         }
         if(!DocReadTextElement(context, upper, &solid)) return false;
         continue;
      }

      cbool isTab    = XmlIsElement(context->reader, XML_NS_W, "tab") || XmlIsElement(context->reader, XML_NS_W, "ptab");
      cbool isHyphen = XmlIsElement(context->reader, XML_NS_W, "noBreakHyphen");
      cbool isReturn = XmlIsElement(context->reader, XML_NS_W, "cr");
      cbool isBreak  = XmlIsElement(context->reader, XML_NS_W, "br");

      if(isTab || isHyphen) {
         if(!textOpen) {
            if(!IrAddSpan(context->document, IR_SPAN_TEXT, bits)) {
               context->memory = true;
               return false;
            }
            textOpen = true;
         }
         // A tab becomes one space (row 28) and a non-breaking hyphen an ordinary one (2.2); a
         // w:softHyphen becomes nothing at all, which is what the absence of a case for it does.
         if(!IrAppendText(context->document, (isTab ? " " : "-"), 1u)) {
            context->memory = true;
            return false;
         }
         // A tab is whitespace and votes on nothing; a non-breaking hyphen is a visible character.
         if(isHyphen) solid = true;
         if(!XmlSkipElement(context->reader)) return false;
         continue;
      }
      if(isReturn || isBreak) {
         cXML_TEXT type = XmlAttribute(context->reader, XML_NS_W, "type");
         // A page or column break maps to nothing (row 27); only a textWrapping break, which is what an
         // absent w:type means, becomes a hard line break.
         cbool wraps = (isReturn || !type.bytes || XmlTextEqual(type, "textWrapping"));

         if(wraps) {
            if(!IrAddSpan(context->document, IR_SPAN_BREAK, bits)) {
               context->memory = true;
               return false;
            }
            textOpen = false;
         }
         if(!XmlSkipElement(context->reader)) return false;
         continue;
      }
      // A picture, in any of the four shapes one arrives in. Skipping an mc:AlternateContent whole --
      // which is what happened before M7 -- lost the picture in both of its branches at once.
      cbool picture = DocIsNamed(context->reader, XML_NS_W, DOC_PICTURES) || XmlIsElement(context->reader, XML_NS_MC, "AlternateContent");

      if(picture) {
         // A picture ends the text span beside it, and votes on nothing: row 12 asks whether every
         // text-bearing run is monospace, and a run bearing a picture bears no text at all.
         textOpen = false;
         if(!DocWalkImage(context)) return false;
         continue;
      }
      // Everything else a run can hold belongs to a later milestone: the field and note elements to
      // M10, w:sym to a symbol table. w:instrText in particular must never be emitted as text, and
      // skipping it whole is how that is kept true.
      if(!XmlSkipElement(context->reader)) return false;
   }
}

//-- Links, images and bookmarks

// Where a drawing object's alt text is looked for, in the order CONVERSION_REFERENCE 2.6 prefers: the
// description first, the title behind it, and the object's own name last. All three are unprefixed.
static constexpr cchptr DOC_DRAWING_ALT[] = {"descr", "title", "name", nullptr};

// The same for VML, whose shape carries its description in an unprefixed alt or title...
static constexpr cchptr DOC_VML_ALT[] = {"alt", "title", nullptr};

// ...and whose image data carries it in the Office extension namespace instead.
static constexpr cchptr DOC_VML_TITLE[] = {"title", nullptr};

// Which relationship a DrawingML blip is drawn from. r:embed names a part inside the package and r:link
// an external one; a blip may carry both, in which case the embedded copy is the one to prefer.
static constexpr cchptr DOC_BLIP_REF[] = {"embed", "link", nullptr};

// The same for VML, where the one attribute serves both and r:href is the external form.
static constexpr cchptr DOC_IMAGEDATA_REF[] = {"id", "href", nullptr};

// The run containers whose own meaning waits for a later milestone but whose text is content now.
static constexpr cchptr DOC_CONTAINERS[] = {"fldSimple", "dir", "bdo", nullptr};

// The VML shapes that can carry a picture, and so an alt text worth reading.
static constexpr cchptr DOC_VML_SHAPES[] = {"shape", "rect", "roundrect", "oval", "shapetype", nullptr};

// Appends an attribute value to the span being built, folding every line end and tab to one space.
//
// Alt text becomes a Markdown link label and a label cannot hold a line end, which is the whole of
// CONVERSION_REFERENCE 2.6's "sanitize ] and newlines" that is not MdEscape's business: the bracket is
// escaped by MD_CONTEXT_ALT_TEXT, and this is the other half.
static cbool DocAppendFlat(DOC_CONTEXTptrc context, cXML_TEXT value) {
   ui64 at = 0;

   while(at < value.length) {
      ui64 run = at;

      while(run < value.length && value.bytes[run] != '\r' && value.bytes[run] != '\n' && value.bytes[run] != '\t') ++run;
      if(run > at && !IrAppendText(context->document, value.bytes + at, run - at)) return false;
      at = run;
      if(at >= value.length) break;
      while(at < value.length && (value.bytes[at] == '\r' || value.bytes[at] == '\n' || value.bytes[at] == '\t')) ++at;
      if(!IrAppendText(context->document, " ", 1u)) return false;
   }
   return true;
}

// Takes the first of a set of attributes the element carries as the image's alt text, once only.
static cbool DocTakeAlt(DOC_CONTEXTptrc context, cXML_NS space, cchptrcptr names, boolptrc taken) {
   if(*taken) return true;
   for(ui64 index = 0; names[index]; ++index) {
      cXML_TEXT value = XmlAttribute(context->reader, space, names[index]);

      if(!value.bytes || !value.length) continue;
      if(!DocAppendFlat(context, value)) return false;
      *taken = true;
      return true;
   }
   return true;
}

// Takes the first of a set of relationship attributes as the image's source, once only.
static cbool DocTakeRef(DOC_CONTEXTptrc context, cchptrcptr names, boolptrc taken) {
   if(*taken) return true;
   for(ui64 index = 0; names[index]; ++index) {
      cXML_TEXT value = XmlAttribute(context->reader, XML_NS_R, names[index]);

      if(!value.bytes || !value.length) continue;
      if(!IrAppendDest(context->document, value.bytes, value.length)) return false;
      *taken = true;
      return true;
   }
   return true;
}

// Walks a picture container -- w:drawing, w:pict, w:object, or an mc:AlternateContent standing in for
// one -- into a single image span, or into nothing at all.
//
// One scan serves all four because a picture is identified by the markers inside it rather than by the
// element it arrived in, and both markup families are looked for at once. The first alt text found wins
// and so does the first relationship, which is what makes mc:AlternateContent right here without a
// branch selector: its mc:Choice and its mc:Fallback describe the *same* picture in two vocabularies, so
// reading both and keeping the first reference emits it exactly once -- the double-emit
// CONVERSION_REFERENCE 5.8 warns about, closed by arithmetic rather than by understanding the branches.
// A container holding no picture reference at all -- a chart, a SmartArt diagram, a drawn shape -- is
// rewound to nothing, because none of them has a bitmap the document could show.
static cbool DocWalkImage(DOC_CONTEXTptrc context) {
   cui32    depthHere = context->reader->depth;
   cIR_MARK mark      = IrMark(context->document);
   ui32     fillDepth = 0;
   bool     altTaken  = false;
   bool     refTaken  = false;

   // An image carries no formatting of its own: bold around a picture renders nothing, and a delimiter
   // pair with an image between it is one more thing for the flanking rules to fail on.
   if(!IrAddSpan(context->document, IR_SPAN_IMAGE, IR_FMT_NONE)) {
      context->memory = true;
      return false;
   }
   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) break;
      // The picture fill is left behind the moment anything at its own level or above appears.
      if(fillDepth && context->reader->depth <= fillDepth) fillDepth = 0;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_PIC, "blipFill")) {
         fillDepth = context->reader->depth;
         continue;
      }
      if(XmlIsElement(context->reader, XML_NS_WP, "docPr")) {
         if(!DocTakeAlt(context, XML_NS_NONE, DOC_DRAWING_ALT, &altTaken)) return false;
         continue;
      }
      // A blip is the document's picture only under a pic:blipFill, which is the DrawingML picture
      // vocabulary. The same element under an a:blipFill is a *fill* -- the bitmap a drawn shape, a
      // chart wall or a table cell is painted with -- and taking it would emit the wallpaper of a
      // shape as the figure the paragraph shows, which is also the opposite of this walk's own rule
      // that a container with no picture in it comes to nothing.
      if(fillDepth && context->reader->depth == fillDepth + 1u && XmlIsElement(context->reader, XML_NS_A, "blip")) {
         if(!DocTakeRef(context, DOC_BLIP_REF, &refTaken)) return false;
         continue;
      }
      if(XmlIsElement(context->reader, XML_NS_V, "imagedata")) {
         if(!DocTakeAlt(context, XML_NS_O, DOC_VML_TITLE, &altTaken)) return false;
         if(!DocTakeRef(context, DOC_IMAGEDATA_REF, &refTaken)) return false;
         continue;
      }
      if(!DocIsNamed(context->reader, XML_NS_V, DOC_VML_SHAPES)) continue;
      if(!DocTakeAlt(context, XML_NS_NONE, DOC_VML_ALT, &altTaken)) return false;
   }
   if(!refTaken) {
      IrRewind(context->document, mark);
      return true;
   }

   IR_SPANptr span = IrSpanMutable(context->document, mark.spanAt);

   // The reference is a relationship id and nothing more until LinkResolve has looked it up, which is
   // where correctness rule 1 is kept for content: ids are scoped to the part they were found in, so
   // the walk records what it read and the resolution happens where the part is known.
   if(span) span->flags = IR_SPAN_FLAG_REL;
   return true;
}

// Walks a w:hyperlink into a link span pair wrapped around its content.
//
// The destination recorded here is the reference as written, not a URL: an r:id is a relationship id
// that LinkResolve turns into one, and a w:anchor is a bookmark name that the same pass turns into a
// heading slug or an explicit anchor. Where both are present the relationship decides and the anchor
// becomes the fragment of whatever it resolves to, which is what a link into another document means;
// the two are joined with the '#' that will separate them in the output, and a relationship id cannot
// hold one, so splitting them again is unambiguous.
static cbool DocWalkHyperlink(DOC_CONTEXTptrc context, csi32 paragraphStyle, cbool heading) {
   cXML_TEXT reference = XmlAttribute(context->reader, XML_NS_R, "id");
   cXML_TEXT anchor    = XmlAttribute(context->reader, XML_NS_W, "anchor");
   cbool     related   = (reference.bytes != nullptr && reference.length != 0);
   cbool     internal  = (anchor.bytes != nullptr && anchor.length != 0);

   // A hyperlink naming nothing is a container and nothing else, and so is one inside another: links do
   // not nest in Markdown, and the outer one is the one a reader was given.
   if((!related && !internal) || context->inLink) return DocWalkChildren(context, DOC_LEVEL_RUN, paragraphStyle, heading);
   if(!IrAddSpan(context->document, IR_SPAN_LINK_START, IR_FMT_NONE)) {
      context->memory = true;
      return false;
   }
   if(related && !IrAppendDest(context->document, reference.bytes, reference.length)) {
      context->memory = true;
      return false;
   }
   if(internal) {
      if(!IrAppendDest(context->document, "#", 1u) || !IrAppendDest(context->document, anchor.bytes, anchor.length)) {
         context->memory = true;
         return false;
      }
   }

   IR_SPANptr span = IrSpanMutable(context->document, IrSpanCount(context->document) - 1u);

   if(span && related) span->flags = IR_SPAN_FLAG_REL;
   context->inLink = true;

   cbool walked = DocWalkChildren(context, DOC_LEVEL_RUN, paragraphStyle, heading);

   context->inLink = false;
   if(!walked) return false;
   if(!IrAddSpan(context->document, IR_SPAN_LINK_END, IR_FMT_NONE)) {
      context->memory = true;
      return false;
   }
   return true;
}

// Records a w:bookmarkStart as an anchor a link may target.
//
// Inside a paragraph the anchor is a span at the point the bookmark stood. Between paragraphs -- legal,
// and what a producer writes when a bookmark wraps whole blocks -- there is no block to put one in, so
// the name is held and attached to the next block that opens. Nothing is filtered out here, _GoBack
// included: LinkResolve mutes every anchor nothing points at, which covers the generated ones without
// this having to know their names.
static cbool DocReadBookmark(DOC_CONTEXTptrc context, cDOC_LEVEL level) {
   cXML_TEXT name = XmlAttribute(context->reader, XML_NS_W, "name");

   if(name.bytes && name.length) {
      if(level == DOC_LEVEL_RUN) {
         if(!IrAddSpan(context->document, IR_SPAN_ANCHOR, IR_FMT_NONE) || !IrAppendDest(context->document, name.bytes, name.length)) {
            context->memory = true;
            return false;
         }
      } else if(context->pendingCount < DOC_PENDING_ANCHORS) {
         context->pendingLength[context->pendingCount] = DocCopyView(name, context->pending[context->pendingCount], DOC_ANCHOR_BYTES);
         ++context->pendingCount;
      }
   }
   return XmlSkipElement(context->reader);
}

// Attaches every bookmark that stood between blocks to the block that has just been opened.
static cbool DocFlushBookmarks(DOC_CONTEXTptrc context) {
   for(ui32 index = 0; index < context->pendingCount; ++index) {
      cchptr name = context->pending[index];

      if(!IrAddSpan(context->document, IR_SPAN_ANCHOR, IR_FMT_NONE) || !IrAppendDest(context->document, name, context->pendingLength[index])) {
         context->pendingCount = 0;
         context->memory       = true;
         return false;
      }
   }
   context->pendingCount = 0;
   return true;
}

// Whether a block holds anything a reader would see, rather than an anchor and nothing else.
//
// A bookmark keeps a block alive, because it is a link target and something has to carry it. Mapping
// row 25's horizontal rule asks a different question -- whether the paragraph "came to nothing" -- and
// a paragraph holding one bookmark did.
// The span range is the mark's rather than the block's, because a block's own spanAt and spanCount are
// not written until IrEndBlock: while a paragraph is still being walked, its spans are simply every
// span added since it began.
static cbool DocBlockIsInk(cIR_DOCUMENTptr document, cIR_MARK mark) {
   if(mark.block < 0) return false; // Nothing was ever begun
   return IrHasInk(document, mark.spanAt, IrSpanCount(document));
}

//-- Paragraphs

// The two halves of CT_PBdr: the sides that make an empty paragraph a horizontal rule, and the sides
// that say it is a box or a rule above rather than below. w:start and w:end belong to the table border
// types and never appear in a paragraph's own w:pBdr.
static constexpr cchptr DOC_BORDERS_UNDER[]  = {"bottom", "between", nullptr};
static constexpr cchptr DOC_BORDERS_BESIDE[] = {"top", "left", "right", "bar", nullptr};

// Whether the element the reader is on is one of a paragraph's border names, in the WordprocessingML
// namespace, which is the one namespace a border may be spelled in.
static cbool DocIsBorder(XML_READERptrc reader, cchptrcptr names) { return DocIsNamed(reader, XML_NS_W, names); }

// Reads the w:pBdr the reader is on, and reports whether its borders are the pattern Word writes for an
// autoformatted horizontal rule: a bottom or a between border and no other (CONVERSION_REFERENCE 2.4).
// A w:val of none or nil is a border switched off, which every producer writes rather than omitting the
// element; a border element with no w:val at all is taken as present, since its presence is the signal.
static cbool DocReadBorders(DOC_CONTEXTptrc context, boolptrc rule) {
   cui32 depthHere = context->reader->depth;
   bool  below     = false;
   bool  other     = false;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) {
         *rule = below && !other;
         return true;
      }
      if(token != XML_TOKEN_START_ELEMENT) continue;

      cXML_TEXT value = XmlAttribute(context->reader, XML_NS_W, "val");
      cbool     drawn = !XmlTextEqual(value, "none") && !XmlTextEqual(value, "nil");
      cbool     under = DocIsBorder(context->reader, DOC_BORDERS_UNDER);
      // The sides of CT_PBdr are tested by name rather than by exclusion. An element this build has
      // never heard of -- a vendor extension, an mc:AlternateContent -- is ignored rather than counted
      // as a border, which is the OOXML compatibility model: what is not understood gets no vote.
      cbool beside = DocIsBorder(context->reader, DOC_BORDERS_BESIDE);

      if(drawn && under) below = true;
      else if(drawn && beside) other = true;
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Reads the w:val of the element the reader is on as a decimal integer, reporting whether it was one.
// A value that is absent, empty or not all digits leaves the destination alone, which is what keeps
// "the paragraph said nothing" apart from "the paragraph said zero".
static cbool DocReadDecimal(DOC_CONTEXTptrc context, si32ptrc out) {
   cXML_TEXT value = XmlAttribute(context->reader, XML_NS_W, "val");

   // No cap on how many digits are read, because ST_DecimalNumber is an xsd:integer and leading zeros
   // are legal in one. A cap on the value's *length* rather than on its magnitude turns "007" into a
   // refusal, and every refusal here is silent: a padded w:outlineLvl stopped being a heading, and a
   // padded w:ilvl lost the level it named. StyleModel's twin dropped its own cap during M8 for this
   // reason; this is the other half of it. The overflow test below is the real bound and it stops
   // after ten significant digits whatever the value is padded to.
   if(!value.bytes || !value.length) return false;

   si64 parsed = 0;

   for(ui64 index = 0; index < value.length; ++index) {
      if(value.bytes[index] < '0' || value.bytes[index] > '9') return false;
      parsed = parsed * 10 + si64(value.bytes[index] - '0');
      if(parsed > 0x7FFFFFFF) return false;
   }
   *out = si32(parsed);
   return true;
}

// Reads the w:numPr the reader is on, and consumes it. Its two values live in children rather than in
// attributes, so it needs a reader of its own exactly as w:pBdr does.
//
// The two are recorded independently. Word writes a w:numPr carrying only w:ilvl when a user changes a
// paragraph's level without changing which list it is in, and one carrying only w:numId when the level
// comes from the style chain; treating the pair as one indivisible property would throw away whichever
// half the paragraph did not restate. Everything CT_NumPr may also carry -- w:numberingChange, a
// tracked w:ins -- falls through to the skip at the end of the loop, which is the compatibility model
// this file applies to every element it has not heard of.
static cbool DocReadNumbering(DOC_CONTEXTptrc context, DOC_NUM_REFptrc num) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_W, "numId")) {
         DocReadDecimal(context, &num->numId);
      } else if(XmlIsElement(context->reader, XML_NS_W, "ilvl")) {
         si32 parsed = 0;

         // 0 to 8 is every level the schema has. A deeper one is malformed and is clamped rather than
         // refused, which is CONVERSION_REFERENCE 5.4's whole treatment of a broken numbering value.
         if(DocReadDecimal(context, &parsed)) num->level = (parsed > 8 ? 8 : parsed);
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// The alignment one w:jc value names, for the delimiter row of mapping row 18. Everything a GFM
// delimiter row cannot spell -- both, distribute, a token this build has never heard of -- is no
// alignment rather than a guess, because a delimiter row that says nothing is what a renderer defaults
// to anyway. ST_Jc's start and end are the bidirectional spellings of left and right, and this build
// has no bidirectional layout to reverse them against (w:bidiVisual is CONVERSION_REFERENCE 2.5's
// "note and ignore"), so they are read as what they mean in a left-to-right table.
static cIR_ALIGN DocAlignOf(cXML_TEXT value) {
   if(XmlTextEqual(value, "center")) return IR_ALIGN_CENTRE;
   if(XmlTextEqual(value, "right") || XmlTextEqual(value, "end")) return IR_ALIGN_RIGHT;
   if(XmlTextEqual(value, "left") || XmlTextEqual(value, "start")) return IR_ALIGN_LEFT;
   return IR_ALIGN_NONE;
}

// Reads the w:pPr the reader is on, and consumes it.
static cbool DocReadParagraphProperties(DOC_CONTEXTptrc context, si32ptrc style, si32ptrc outline, boolptrc rule, DOC_NUM_REFptrc num) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_W, "pBdr")) {
         if(!DocReadBorders(context, rule)) return false;
         continue;
      }
      if(XmlIsElement(context->reader, XML_NS_W, "numPr")) {
         if(!DocReadNumbering(context, num)) return false;
         continue;
      }
      if(XmlIsElement(context->reader, XML_NS_W, "jc")) {
         // Mapping row 18's column alignment. The first paragraph of a cell that states one speaks for
         // the cell, and only the header row's cells reach the delimiter row -- but every paragraph is
         // read here, because the walk cannot know which cell it is in without asking.
         if(context->justify == IR_ALIGN_NONE) context->justify = DocAlignOf(XmlAttribute(context->reader, XML_NS_W, "val"));
      } else if(XmlIsElement(context->reader, XML_NS_W, "pStyle")) {
         csi32 found = DocFindStyle(context, XmlAttribute(context->reader, XML_NS_W, "val"));

         if(found >= 0) *style = found;
      } else if(XmlIsElement(context->reader, XML_NS_W, "outlineLvl")) {
         si32 parsed = 0;

         // 9 is body text and anything past it is malformed, so neither is a heading level.
         if(DocReadDecimal(context, &parsed) && parsed <= 9) *outline = parsed;
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Turns a paragraph's resolved role into the block kind that carries it. A heading wins over a quote and
// over a code style, because a heading is the document's structure while the other two are its voice --
// and because an ATX heading cannot hold either construct anyway, one being a prefix on every line and
// the other a fence around them.
static cIR_BLOCK_KIND DocBlockKind(cSTYLE_PARAGRAPH_PROPS props) {
   if(props.headingLevel > 0) return IR_BLOCK_HEADING;
   if(props.role == STYLE_ROLE_QUOTE) return IR_BLOCK_QUOTE;
   if(props.role == STYLE_ROLE_CODE) return IR_BLOCK_CODE;
   return IR_BLOCK_PARAGRAPH;
}

// Whether a paragraph's numbering survives its block kind. It is a separate question from the kind and
// not a sixth value of it: a list item's kind is what its content is, and being an item of a list is a
// second fact the document may state about the same paragraph.
//
// A heading is the one kind that cancels it, and CONVERSION_REFERENCE 5.4 rules it outright: "Headings
// with numbering (multilevel heading numbering): heading wins". That is not an edge case but the
// common one -- Word's Multilevel List linked to headings puts a w:numPr on every Heading N style, and
// without this rule every heading in such a document becomes a list item and the structure inverts.
//
// A w:numId of 0 is a specification of "no numbering" (CONVERSION_REFERENCE 2.4) and cancels whatever
// the style chain supplied, which is why the test is > 0 and not >= 0.
static cbool DocListSurvives(cIR_BLOCK_KIND kind, cSTYLE_PARAGRAPH_PROPS props, cNUM_MODELptr numbering) {
   return props.numId > 0 && kind != IR_BLOCK_HEADING && NumFind(numbering, props.numId) >= 0;
}

// Walks one w:p into one block, which IrEndBlock throws away again when it holds nothing.
static cbool DocWalkParagraph(DOC_CONTEXTptrc context) {
   cui32         depthHere = context->reader->depth;
   si32          style     = StyleDefaultParagraph(context->styles);
   si32          outline   = -1;
   DOC_NUM_REF   num       = {-1, -1};
   IR_MARK       mark      = {-1, 0, 0, 0, 0, 0, 0, 0};
   IR_BLOCK_KIND kind      = IR_BLOCK_PARAGRAPH;
   ui8           level     = 0;
   si32          listId    = -1;
   si32          listLevel = 0;
   bool          list      = false;
   bool          rule      = false;
   bool          settled   = false;
   bool          begun     = false;
   bool          head      = false;
   bool          ok        = true;
   // Saved and restored rather than merely cleared: a paragraph nests inside a table cell from M9, and a
   // cell's paragraph must not settle the classification of the one the table stands in.
   cbool outerText = context->sawText;
   cbool outerMono = context->allMono;

   context->sawText = false;
   context->allMono = true;
   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!settled && XmlIsElement(context->reader, XML_NS_W, "pPr")) {
         if(!DocReadParagraphProperties(context, &style, &outline, &rule, &num)) return false;
         continue;
      }
      if(!settled) {
         // The properties are settled by the time any content is reached: w:pPr is the paragraph's first
         // child whenever it is present, so anything else means there is no more of it to come.
         cSTYLE_PARAGRAPH_PROPS props = StyleResolveParagraph(context->styles, style, outline, num.numId, num.level);

         head      = (props.headingLevel > 0);
         level     = props.headingLevel;
         kind      = DocBlockKind(props);
         list      = DocListSurvives(kind, props, context->numbering);
         listId    = props.numId;
         listLevel = (props.numLevel > 0 ? props.numLevel : 0);
         settled   = true;
      }
      if(!begun) {
         mark  = IrBeginBlock(context->document, kind, level);
         begun = true;
         if(mark.block < 0) {
            context->memory = true;
            return false;
         }
         if(!DocFlushBookmarks(context)) return false;
      }
      // A paragraph's children are run-level content, and every transparent wrapper is handled there.
      if(!DocDispatchChild(context, DOC_LEVEL_RUN, style, head)) {
         ok = false;
         break;
      }
   }
   if(!settled) {
      cSTYLE_PARAGRAPH_PROPS props = StyleResolveParagraph(context->styles, style, outline, num.numId, num.level);

      level     = props.headingLevel;
      kind      = DocBlockKind(props);
      list      = DocListSurvives(kind, props, context->numbering);
      listId    = props.numId;
      listLevel = (props.numLevel > 0 ? props.numLevel : 0);
   }
   // A paragraph with no children at all is how every producer writes an empty line, and inside a run of
   // code paragraphs that is a blank line of the fence rather than nothing -- so a code paragraph gets
   // its block even when there was never any content to open one. The emitter drops such a block again
   // wherever it falls at the edge of a fence, which is the only place it would be a blank line.
   // A list item gets its block whatever else the paragraph carried, because a marker on a line of its
   // own is content the border test below has already been told not to speak for.
   if(!begun && ok && (list || (!rule && kind == IR_BLOCK_CODE))) {
      mark  = IrBeginBlock(context->document, kind, level);
      begun = true;
      if(mark.block < 0) {
         context->memory = true;
         return false;
      }
      if(!DocFlushBookmarks(context)) return false;
   }

   bool kept = false;
   bool ink  = false;

   if(begun) {
      // CONVERSION_REFERENCE row 12's second detection: a paragraph whose every text-bearing run is set
      // in a monospace family is code even where no style says so. It is settled here rather than in
      // RunCoalescer because the font is a run property the intermediate representation does not carry,
      // and re-resolving it from the spans afterwards would mean carrying it only to answer this once.
      // A list item is exempt, and the reasoning is StyleReadBaseline's: the font is a *guess* at what
      // a paragraph is, and a paragraph carrying a w:numPr has already stated it. A list of code lines
      // set in Consolas would otherwise become a run of fences, each having lost its marker.
      if(kind == IR_BLOCK_PARAGRAPH && !list && context->sawText && context->allMono) {
         IR_BLOCKptr block = IrBlockMutable(context->document, ui32(mark.block));

         if(block) block->kind = IR_BLOCK_CODE;
      }
      // Recorded before the block is ended, because IrEndBlock reads it: an empty list item is a marker
      // on a line of its own and must not be unwound the way an empty paragraph is.
      if(list) IrSetListRef(context->document, mark, listId, ui32(listLevel));
      ink  = DocBlockIsInk(context->document, mark);
      kept = IrEndBlock(context->document, mark);
   }
   context->sawText = outerText;
   context->allMono = outerMono;
   // Row 25: a lone bottom border on a paragraph that came to nothing is Word's autoformatted horizontal
   // rule. The test is "came to nothing" and not "has no runs", so a paragraph of empty runs is one too.
   // A bookmark keeps a block alive without putting anything on the page, so the rule's test is what
   // the block would have *shown* rather than whether it survived being ended.
   // A paragraph carrying a live w:numPr did not come to nothing, whatever its runs held: Word draws
   // its marker and its border both. Emitting "---" there would delete the item and invent a rule the
   // document does not have -- and Word's own autoformatted rule never carries numbering, so the
   // exemption costs nothing. The same reasoning IrHasInk gives for counting a picture as ink.
   if(ok && rule && !list && (!kept || !ink)) {
      cIR_MARK ruled = IrBeginBlock(context->document, IR_BLOCK_RULE, 0);

      if(ruled.block < 0) {
         context->memory = true;
         return false;
      }
      IrEndBlock(context->document, ruled);
   }
   return ok;
}

//-- Tables

// Counts the w:gridCol children of the w:tblGrid the reader is on. CONVERSION_REFERENCE 2.5 makes that
// count the authoritative column width of the table -- rows may hold fewer w:tc than it because of
// w:gridSpan, and the emitter pads them back out to it.
static cbool DocReadGrid(DOC_CONTEXTptrc context, ui32ptrc columns) {
   cui32 depthHere = context->reader->depth;
   ui32  found     = 0;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) {
         *columns = found;
         return true;
      }
      if(token != XML_TOKEN_START_ELEMENT) continue;
      // A w:gridCol past the cap stops being counted rather than refusing the table: the grid is a
      // width and a document that declares a million of them has said nothing a reader could use.
      if(XmlIsElement(context->reader, XML_NS_W, "gridCol") && found < IR_MAX_COLUMNS) ++found;
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Reads the w:trPr the reader is on, and reports what it said about the row. w:tblHeader marks a row
// that repeats at a page break, which is the only thing WordprocessingML has to say "this row is a
// header"; w:del marks a row a tracked change removed, and accept-all drops it with its content
// (correctness rule 8). A w:val of none or nil on either switches it off, the way every toggle spells
// "not set" rather than being omitted.
static cbool DocReadRowProperties(DOC_CONTEXTptrc context, boolptrc header, boolptrc deleted) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;

      cXML_TEXT value = XmlAttribute(context->reader, XML_NS_W, "val");
      cbool     on    = !XmlTextEqual(value, "none") && !XmlTextEqual(value, "nil") && !XmlTextEqual(value, "0") && !XmlTextEqual(value, "false");

      if(XmlIsElement(context->reader, XML_NS_W, "tblHeader")) *header = on;
      else if(XmlIsElement(context->reader, XML_NS_W, "del")) *deleted = true;
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Reads the w:tcPr the reader is on, and reports what it said about the cell: how many grid columns it
// covers, and whether it is the start of a vertical merge or a continuation of one. A w:vMerge with no
// w:val, or one saying "continue", is a continuation -- which is the cell the row above is still
// filling, and which carries an empty paragraph rather than content of its own.
static cbool DocReadCellProperties(DOC_CONTEXTptrc context, ui32ptrc span, ui8ptrc flags) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_W, "gridSpan")) {
         si32 parsed = 0;

         if(DocReadDecimal(context, &parsed) && parsed > 0) *span = ui32(parsed);
      } else if(XmlIsElement(context->reader, XML_NS_W, "vMerge")) {
         cXML_TEXT value = XmlAttribute(context->reader, XML_NS_W, "val");

         *flags |= ui8(XmlTextEqual(value, "restart") ? IR_CELL_VRESTART : IR_CELL_VMERGED);
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Opens one cell, recording where its content will start and how much of the grid it claims. That a
// cell merges is a fact IrEndTable derives from the cells that survive rather than one recorded here,
// because an mc:Fallback may replace this cell and a discarded branch must declare nothing.
static cbool DocOpenCell(DOC_CONTEXTptrc context, cui32 span, cui8 flags, si32ptrc cell, ui32ptrc blockAt) {
   *cell = IrBeginCell(context->document, context->row, context->lastCell, span, flags);
   if(*cell < 0) {
      context->memory = true;
      return false;
   }
   context->lastCell = *cell;
   *blockAt          = IrBlockCount(context->document);
   return true;
}

// Opens one row, linking it behind the row before it.
static cbool DocOpenRow(DOC_CONTEXTptrc context, cbool header, si32ptrc row) {
   *row = IrBeginRow(context->document, context->table, context->lastRow, header);
   if(*row < 0) {
      context->memory = true;
      return false;
   }
   context->lastRow = *row;
   context->row     = *row;
   return true;
}

// Walks one w:tc into one cell, whose content is ordinary block content walked where it stands.
//
// A cell's alignment is the first w:jc any of its paragraphs states, and it is recorded on the cell
// rather than spread over the table's columns here: which row a cell is in is the table's question,
// and only the first row's cells can reach a delimiter row. The question is reopened per cell and
// closed again on the way out, so a nested table's cells never answer it for the cell they stand in.
static cbool DocWalkCell(DOC_CONTEXTptrc context) {
   cui32     depthHere    = context->reader->depth;
   cIR_ALIGN outerJustify = context->justify;
   ui32      span         = 1u;
   ui8       flags        = IR_CELL_NONE;
   si32      cell         = -1;
   ui32      blockAt      = IrBlockCount(context->document);
   bool      settled      = false;
   bool      ok           = true;

   context->justify = IR_ALIGN_NONE;
   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!settled && XmlIsElement(context->reader, XML_NS_W, "tcPr")) {
         if(!DocReadCellProperties(context, &span, &flags)) return false;
         continue;
      }
      if(!settled) {
         // w:tcPr is the cell's first child whenever it is present, so anything else means the
         // properties are settled and the cell can be opened where its content starts.
         if(!DocOpenCell(context, span, flags, &cell, &blockAt)) return false;
         settled = true;
      }
      if(!DocDispatchChild(context, DOC_LEVEL_BLOCK, -1, false)) {
         ok = false;
         break;
      }
   }
   // A w:tc of nothing but its w:tcPr, or of nothing whatever. It is still a cell and still a column
   // the grid has to account for: the schema says a cell always holds a w:p, and a producer that leaves
   // one out must not cost its row a column.
   if(!settled && !DocOpenCell(context, span, flags, &cell, &blockAt)) return false;
   IrEndCell(context->document, cell, blockAt, context->justify);
   context->justify = outerJustify;
   return ok;
}

// Walks one w:tr into one row.
static cbool DocWalkRow(DOC_CONTEXTptrc context) {
   cui32 depthHere = context->reader->depth;
   bool  header    = false;
   bool  deleted   = false;
   bool  settled   = false;
   bool  ok        = true;
   si32  row       = -1;

   context->lastCell = -1;
   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!settled && !deleted && XmlIsElement(context->reader, XML_NS_W, "trPr")) {
         if(!DocReadRowProperties(context, &header, &deleted)) return false;
         continue;
      }
      // Accept-all revisions, correctness rule 8: a row a tracked change deleted is not there, and
      // neither is its content. It is skipped whole rather than kept empty, because an empty row is a
      // row a reader still sees.
      if(deleted) {
         if(!XmlSkipElement(context->reader)) return false;
         continue;
      }
      if(!settled) {
         if(!DocOpenRow(context, header, &row)) return false;
         settled = true;
      }
      if(!DocDispatchChild(context, DOC_LEVEL_ROW, -1, false)) {
         ok = false;
         break;
      }
   }
   if(deleted) return ok;
   // A w:tr with no w:tc at all. Word writes one while a user is building a table, and it is a row of
   // empty cells on the page, so it is a row here too.
   if(!settled && !DocOpenRow(context, header, &row)) return false;
   IrEndRow(context->document, row, context->lastCell);
   return ok;
}

// Walks one w:tbl into one table block and the rows and cells beside it.
//
// The table's own block is opened and closed before any row is read, because it carries no spans at all
// -- its content is the blocks of its cells -- and because every pass above the walk reads one flat
// array in document order, so the table has to stand where the document put it.
static cbool DocWalkTable(DOC_CONTEXTptrc context) {
   cui32 depthHere = context->reader->depth;

   // A table nested past the cap is dropped rather than refused, which is CONVERSION_REFERENCE 5.4's
   // treatment of everything a document overdoes. The tokenizer's own element cap would stop a runaway
   // eventually; this is the bound that keeps this walk's *stack* off the document's content.
   if(context->depth >= IR_MAX_TABLE_DEPTH) return XmlSkipElement(context->reader);

   cIR_MARK before = IrMark(context->document);
   cIR_MARK mark   = IrBeginBlock(context->document, IR_BLOCK_TABLE, 0);

   if(mark.block < 0) {
      context->memory = true;
      return false;
   }
   // A table block never holds a span, so it is ended at once; a bookmark that stood in front of the
   // table is deliberately not flushed into it and waits for the first paragraph of the first cell,
   // which is the nearest place in the output a link could land on.
   IrEndBlock(context->document, mark);

   csi32 table = IrBeginTable(context->document, mark);

   if(table < 0) {
      context->memory = true;
      return false;
   }

   // Saved and restored around the whole table, because a cell may hold another one and the two must
   // not share a chain. The two tails are what an unwound mc:Choice puts back, which is why they live
   // on the context rather than on this frame: the wrapper that unwinds one is shared by every level
   // and cannot reach a caller's locals.
   csi32     outerTable    = context->table;
   csi32     outerRow      = context->row;
   csi32     outerLastRow  = context->lastRow;
   csi32     outerLastCell = context->lastCell;
   cIR_ALIGN outerJustify  = context->justify;

   context->table    = table;
   context->row      = -1;
   context->lastRow  = -1;
   context->lastCell = -1;
   context->justify  = IR_ALIGN_NONE;
   context->depth += 1u;

   ui32 columns = 0;
   bool ok      = true;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) {
         ok = false;
         break;
      }
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_W, "tblGrid")) {
         ui32 declared = 0;

         if(!DocReadGrid(context, &declared)) {
            ok = false;
            break;
         }
         if(declared > columns) columns = declared;
         continue;
      }
      // w:tblPr describes borders, widths and a look this mapping has nothing to say about, and
      // anything else is an element this build has never heard of. Both fall through to the
      // dispatcher's skip, which also handles every transparent wrapper a w:tr may stand inside.
      if(!DocDispatchChild(context, DOC_LEVEL_TABLE, -1, false)) {
         ok = false;
         break;
      }
   }
   // Whether any row survived is this walk's own tail and never the table's firstRow, because a rewind
   // inside a discarded mc:Choice truncates the row array without touching the record that names it:
   // reading firstRow there keeps a table of no usable rows, which emits an empty <table> or nothing.
   cbool empty = (context->lastRow < 0);

   // A table that came to nothing is unwound entirely, which is what keeps an empty w:tbl -- and one
   // whose every row a tracked change removed -- from costing a blank line and a delimiter row.
   // Nothing is marked on the parent here: IrEndTable derives IR_TABLE_NESTED from the blocks its own
   // cells ended up holding, so a nested table an mc:Fallback discarded leaves no trace on it. Marked
   // from here, that parent stayed flagged after the rewind and was emitted as raw HTML it did not need.
   if(empty) IrRewind(context->document, before);
   else IrEndTable(context->document, table, context->lastRow, columns);
   context->table    = outerTable;
   context->row      = outerRow;
   context->lastRow  = outerLastRow;
   context->lastCell = outerLastCell;
   context->justify  = outerJustify;
   context->depth -= 1u;
   return ok;
}

//-- Wrappers

// Walks the w:sdt the reader is on, descending only into its w:sdtContent. The properties half is
// metadata; the content half is ordinary document content that must not disappear with its wrapper.
static cbool DocWalkStructuredTag(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_W, "sdtContent")) {
         if(!DocWalkChildren(context, level, paragraphStyle, heading)) return false;
         continue;
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Walks the w:ruby the reader is on, descending only into its w:rubyBase. The w:rt half is the
// annotation printed above the base text, which Markdown has nowhere to put; the base is the sentence.
static cbool DocWalkRuby(DOC_CONTEXTptrc context, csi32 paragraphStyle, cbool heading) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(context->reader, XML_NS_W, "rubyBase")) {
         if(!DocWalkChildren(context, DOC_LEVEL_RUN, paragraphStyle, heading)) return false;
         continue;
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

// Walks the mc:AlternateContent the reader is on. This build understands no extension namespace, so it
// understands no mc:Choice and the mc:Fallback is the branch to take -- but a Fallback stands after the
// Choices and a pull tokenizer cannot look ahead. So the first Choice is walked speculatively and rewound
// again if a Fallback turns out to exist.
//
// RULE-DEV:correctness-rule-2 ISO/IEC 29500-3 10.2 says an element with no selectable mc:Choice and no
// mc:Fallback contributes nothing at all. Taking the first Choice in that case instead is a deliberate
// leniency: this build understands no extension namespace, so it would otherwise drop every branch of
// every such element, and skipping unknown markup means nothing in a Choice can be misread anyway. The
// milestone that first understands a Requires namespace has to select on it, and must decide then
// whether to keep this fallback-of-last-resort.
static cbool DocWalkAlternate(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading) {
   cui32    depthHere    = context->reader->depth;
   cIR_MARK mark         = IrMark(context->document);
   bool     tookChoice   = false;
   bool     tookFallback = false;
   // The paragraph's row 12 vote is walker state rather than IR, so IR_MARK does not carry it and a
   // rewind has to undo it here. Without this a discarded Choice votes: a plain Choice beside an
   // all-monospace Fallback demotes the fence that survives to an inline code span.
   cbool markedText = context->sawText;
   cbool markedMono = context->allMono;
   // The two chain tails are walker state for the same reason and are restored the same way: an
   // mc:AlternateContent is legal around a w:tr and around a w:tc, so a discarded mc:Choice can leave
   // a row or a cell behind that the rewind has already thrown away. Putting the tails back is the
   // whole of what that costs -- the next append links behind the record that really precedes it, and
   // IrEndRow and IrEndTable write the terminator from the tail rather than from what was appended
   // last, so a row nothing points at simply is not in the chain.
   csi32 markedRow  = context->lastRow;
   csi32 markedCell = context->lastCell;
   // A cell's alignment latches on the first w:jc it sees, so one inside a discarded mc:Choice settled
   // the column and the surviving mc:Fallback's own w:jc was then ignored -- which reaches the
   // delimiter row for a first-row cell and aligns the whole column by a branch that was thrown away.
   // The queued bookmarks go back for the same reason, and that one has been here since M7: a
   // w:bookmarkStart in a discarded Choice was flushed into the Fallback's first block instead.
   cIR_ALIGN markedJustify = context->justify;
   cui32     markedPending = context->pendingCount;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!tookFallback && XmlIsElement(context->reader, XML_NS_MC, "Fallback")) {
         if(tookChoice) {
            IrRewind(context->document, mark);
            context->sawText      = markedText;
            context->allMono      = markedMono;
            context->lastRow      = markedRow;
            context->lastCell     = markedCell;
            context->justify      = markedJustify;
            context->pendingCount = markedPending;
         }
         if(!DocWalkChildren(context, level, paragraphStyle, heading)) return false;
         tookFallback = true;
         continue;
      }
      if(!tookChoice && !tookFallback && XmlIsElement(context->reader, XML_NS_MC, "Choice")) {
         if(!DocWalkChildren(context, level, paragraphStyle, heading)) return false;
         tookChoice = true;
         continue;
      }
      if(!XmlSkipElement(context->reader)) return false;
   }
}

//-- The walk

// Handles the one start element the reader is on, at one of the two levels, consuming it whole. Every
// transparent wrapper appears at both levels and is handled here once for both, which is why the two
// levels are one function: w:ins around a paragraph and w:ins around a run mean exactly the same thing.
static cbool DocDispatchChild(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading) {
   // Accept-all revisions, correctness rule 8: an insertion is not there, and a deletion is gone.
   cbool inserted = XmlIsElement(context->reader, XML_NS_W, "ins") || XmlIsElement(context->reader, XML_NS_W, "moveTo");
   cbool deleted  = XmlIsElement(context->reader, XML_NS_W, "del") || XmlIsElement(context->reader, XML_NS_W, "moveFrom");
   cbool tagged   = XmlIsElement(context->reader, XML_NS_W, "smartTag") || XmlIsElement(context->reader, XML_NS_W, "customXml");

   if(deleted) return XmlSkipElement(context->reader);
   if(inserted || tagged) return DocWalkChildren(context, level, paragraphStyle, heading);
   // A bookmark is a range marker rather than content, and it appears at both levels for the same
   // reason every wrapper does: a bookmark may wrap whole paragraphs or part of one.
   if(XmlIsElement(context->reader, XML_NS_W, "bookmarkStart")) return DocReadBookmark(context, level);
   if(XmlIsElement(context->reader, XML_NS_W, "sdt")) return DocWalkStructuredTag(context, level, paragraphStyle, heading);
   if(XmlIsElement(context->reader, XML_NS_MC, "AlternateContent")) return DocWalkAlternate(context, level, paragraphStyle, heading);
   if(level == DOC_LEVEL_TABLE) {
      if(XmlIsElement(context->reader, XML_NS_W, "tr")) return DocWalkRow(context);
      // w:tblPr, w:tblPrEx and anything else a w:tbl may carry beside its rows and its grid.
      return XmlSkipElement(context->reader);
   }
   if(level == DOC_LEVEL_ROW) {
      if(XmlIsElement(context->reader, XML_NS_W, "tc")) return DocWalkCell(context);
      // w:trPr is read by the row walk itself, before any cell opens; everything else is unknown.
      return XmlSkipElement(context->reader);
   }
   if(level == DOC_LEVEL_BLOCK) {
      if(XmlIsElement(context->reader, XML_NS_W, "p")) return DocWalkParagraph(context);
      if(XmlIsElement(context->reader, XML_NS_W, "tbl")) return DocWalkTable(context);
      // w:sectPr describes page layout the mapping ignores, and everything else is an element this
      // build has not heard of. Both are skipped whole.
      return XmlSkipElement(context->reader);
   }
   if(XmlIsElement(context->reader, XML_NS_W, "r")) return DocWalkRun(context, paragraphStyle, heading);
   if(XmlIsElement(context->reader, XML_NS_W, "hyperlink")) return DocWalkHyperlink(context, paragraphStyle, heading);
   // w:dir and w:bdo are bidirectional run containers and nothing else; w:fldSimple gets its field
   // semantics at M10. All three hold text that is content now, so all three are descended into.
   cbool container = DocIsNamed(context->reader, XML_NS_W, DOC_CONTAINERS);

   if(container) return DocWalkChildren(context, DOC_LEVEL_RUN, paragraphStyle, heading);
   if(XmlIsElement(context->reader, XML_NS_W, "ruby")) return DocWalkRuby(context, paragraphStyle, heading);
   return XmlSkipElement(context->reader);
}

// Walks every child of the element the reader is on, handing each start element to the dispatcher.
static cbool DocWalkChildren(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!DocDispatchChild(context, level, paragraphStyle, heading)) return false;
   }
}

//== Entry points

cWALK_STATUS DocWalk(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, csi32 partIndex) {
   WALK_STATUS status = {WALK_OK, XML_OK, OPC_OK};

   cOPC_RESULT loaded = OpcLoadXmlPart(package, partIndex);

   if(loaded != OPC_OK) {
      status.result = (loaded == OPC_ERROR_MEMORY ? WALK_ERROR_MEMORY : WALK_ERROR_PART);
      status.opc    = loaded;
      return status;
   }
   cui8ptr bytes = OpcPartBytes(package, partIndex);

   return DocWalkBytes(document, styles, numbering, bytes, OpcPartByteCount(package, partIndex));
}

cWALK_STATUS DocWalkBytes(IR_DOCUMENTptrc document, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, cui8ptr bytes, cui64 byteCount) {
   WALK_STATUS status = {WALK_OK, XML_OK, OPC_OK};
   XML_READER  reader;
   cXML_RESULT opened = XmlOpen(&reader, bytes, byteCount);

   if(opened != XML_OK) {
      XmlClose(&reader);
      status.result = WALK_ERROR_XML;
      status.xml    = opened;
      return status;
   }

   DOC_CONTEXT context;

   context.document     = document;
   context.styles       = styles;
   context.numbering    = numbering;
   context.reader       = &reader;
   context.cachedStyle  = -1;
   context.cachedId[0]  = 0;
   context.pendingCount = 0;
   context.table        = -1;
   context.row          = -1;
   context.lastRow      = -1;
   context.lastCell     = -1;
   context.depth        = 0;
   context.justify      = IR_ALIGN_NONE;
   context.inLink       = false;
   context.sawText      = false;
   context.allMono      = true;
   context.memory       = false;

   bool sawDocument = false;
   bool sawBody     = false;

   for(;;) {
      cXML_TOKEN token = XmlNext(&reader);

      if(token == XML_TOKEN_END_OF_INPUT || token == XML_TOKEN_ERROR) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!sawDocument) {
         if(!XmlIsElement(&reader, XML_NS_W, "document")) {
            status.result = WALK_ERROR_ROOT;
            break;
         }
         sawDocument = true;
         continue;
      }
      if(reader.depth == 2u && XmlIsElement(&reader, XML_NS_W, "body")) {
         sawBody = true;
         if(!DocWalkChildren(&context, DOC_LEVEL_BLOCK, -1, false)) break;
         continue;
      }
      // w:background is the only other child w:document has, and it describes a page colour.
      if(!XmlSkipElement(&reader)) break;
   }

   cXML_RESULT broke = reader.result;

   XmlClose(&reader);
   if(context.memory || IrFailed(document)) {
      status.result = WALK_ERROR_MEMORY;
      return status;
   }
   if(broke != XML_OK) {
      status.result = WALK_ERROR_XML;
      status.xml    = broke;
      return status;
   }
   if(status.result != WALK_OK) return status;
   // A w:document with no w:body carries no content at all, which is a defective part rather than an
   // empty document: the schema makes the body mandatory.
   if(!sawDocument || !sawBody) status.result = WALK_ERROR_ROOT;
   return status;
}

cchptr DocWalkResultText(OPC_PACKAGEptrc package, cWALK_STATUS status) {
   if(status.result == WALK_ERROR_PART && status.opc != OPC_OK) return OpcResultText(package, status.opc);
   if(status.result == WALK_ERROR_XML && status.xml != XML_OK) return XmlResultText(status.xml);
   if(status.result < 0 || status.result >= WALK_RESULT_COUNT) return "the main document part could not be read";
   return WALK_RESULT_TEXT[status.result];
}
