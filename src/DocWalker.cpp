/*
 * File: DocWalker.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: The body walk: wrappers, paragraph classification, runs and run content into the IR.
 * To Do: 1) Choose an understood mc:Choice by its Requires prefix once an extension namespace is understood,
 *           and honour the mc:Ignorable and mc:ProcessContent *attributes*, which nothing reads today.
 *        2) Uppercase beyond ASCII and Latin-1 for w:caps, which needs Unicode's case tables.
 *        3) Linearize m:oMath and map w:sym, both of which are skipped whole and so lose their text.
 *        4) Cache more than one paragraph style if a document is ever found alternating between many.
 *        5) Read a paragraph's w:shd as the code hint CONVERSION_REFERENCE 2.3 names beside w:rFonts.
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
   DOC_LEVEL_RUN        ///< Children are run-level items: runs, hyperlinks, wrappers
};

typedef const DOC_LEVEL cDOC_LEVEL;

// Everything one walk carries. One worker owns one of these on its own stack and never shares it (D6).
struct DOC_CONTEXT {
   IR_DOCUMENTptr  document;                       ///< Where blocks and spans are being built
   cSTYLE_MODELptr styles;                         ///< The resolved style cache
   XML_READERptr   reader;                         ///< The tokenizer over the part
   si32            cachedStyle;                    ///< What cachedId resolved to, or -1
   char            cachedId[STYLE_MAX_NAME_BYTES]; ///< The last w:pStyle or w:rStyle value looked up
   bool            sawText;                        ///< Whether the paragraph being walked produced any text
   bool            allMono;                        ///< Whether every text-bearing run of it was monospace
   bool            memory;                         ///< Whether an allocation failed; sticky once set
};

typedef DOC_CONTEXT *const DOC_CONTEXTptrc;

static cbool DocWalkChildren(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading);
static cbool DocDispatchChild(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading);

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
      // Everything else a run can hold belongs to a later milestone: w:drawing and w:pict to M7, the
      // field and note elements to M10, w:sym to a symbol table. w:instrText in particular must never
      // be emitted as text, and skipping it whole is how that is kept true.
      if(!XmlSkipElement(context->reader)) return false;
   }
}

//-- Paragraphs

// The two halves of CT_PBdr: the sides that make an empty paragraph a horizontal rule, and the sides
// that say it is a box or a rule above rather than below. w:start and w:end belong to the table border
// types and never appear in a paragraph's own w:pBdr.
static constexpr cchptr DOC_BORDERS_UNDER[]  = {"bottom", "between", nullptr};
static constexpr cchptr DOC_BORDERS_BESIDE[] = {"top", "left", "right", "bar", nullptr};

// Whether the element the reader is on is one of a paragraph's border names, in the WordprocessingML
// namespace. Matching by name is what keeps an element this build has never heard of from voting.
static cbool DocIsBorder(XML_READERptrc reader, cchptrcptr names) {
   for(ui64 index = 0; names[index]; ++index) {
      if(XmlIsElement(reader, XML_NS_W, names[index])) return true;
   }
   return false;
}

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

// Reads the w:pPr the reader is on, and consumes it.
static cbool DocReadParagraphProperties(DOC_CONTEXTptrc context, si32ptrc style, si32ptrc outline, boolptrc rule) {
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
      if(XmlIsElement(context->reader, XML_NS_W, "pStyle")) {
         csi32 found = DocFindStyle(context, XmlAttribute(context->reader, XML_NS_W, "val"));

         if(found >= 0) *style = found;
      } else if(XmlIsElement(context->reader, XML_NS_W, "outlineLvl")) {
         cXML_TEXT value = XmlAttribute(context->reader, XML_NS_W, "val");

         if(value.bytes && value.length && value.length <= 2u) {
            si32 parsed = 0;
            bool digits = true;

            for(ui64 index = 0; index < value.length; ++index) {
               if(value.bytes[index] < '0' || value.bytes[index] > '9') digits = false;
               else parsed = parsed * 10 + si32(value.bytes[index] - '0');
            }
            if(digits && parsed <= 9) *outline = parsed;
         }
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

// Walks one w:p into one block, which IrEndBlock throws away again when it holds nothing.
static cbool DocWalkParagraph(DOC_CONTEXTptrc context) {
   cui32         depthHere = context->reader->depth;
   si32          style     = StyleDefaultParagraph(context->styles);
   si32          outline   = -1;
   IR_MARK       mark      = {-1, 0, 0};
   IR_BLOCK_KIND kind      = IR_BLOCK_PARAGRAPH;
   ui8           level     = 0;
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
         if(!DocReadParagraphProperties(context, &style, &outline, &rule)) return false;
         continue;
      }
      if(!settled) {
         // The properties are settled by the time any content is reached: w:pPr is the paragraph's first
         // child whenever it is present, so anything else means there is no more of it to come.
         cSTYLE_PARAGRAPH_PROPS props = StyleResolveParagraph(context->styles, style, outline);

         head    = (props.headingLevel > 0);
         level   = props.headingLevel;
         kind    = DocBlockKind(props);
         settled = true;
      }
      if(!begun) {
         mark  = IrBeginBlock(context->document, kind, level);
         begun = true;
         if(mark.block < 0) {
            context->memory = true;
            return false;
         }
      }
      // A paragraph's children are run-level content, and every transparent wrapper is handled there.
      if(!DocDispatchChild(context, DOC_LEVEL_RUN, style, head)) {
         ok = false;
         break;
      }
   }
   if(!settled) {
      cSTYLE_PARAGRAPH_PROPS props = StyleResolveParagraph(context->styles, style, outline);

      level = props.headingLevel;
      kind  = DocBlockKind(props);
   }
   // A paragraph with no children at all is how every producer writes an empty line, and inside a run of
   // code paragraphs that is a blank line of the fence rather than nothing -- so a code paragraph gets
   // its block even when there was never any content to open one. The emitter drops such a block again
   // wherever it falls at the edge of a fence, which is the only place it would be a blank line.
   if(!begun && ok && !rule && kind == IR_BLOCK_CODE) {
      mark  = IrBeginBlock(context->document, kind, level);
      begun = true;
      if(mark.block < 0) {
         context->memory = true;
         return false;
      }
   }

   bool kept = false;

   if(begun) {
      // CONVERSION_REFERENCE row 12's second detection: a paragraph whose every text-bearing run is set
      // in a monospace family is code even where no style says so. It is settled here rather than in
      // RunCoalescer because the font is a run property the intermediate representation does not carry,
      // and re-resolving it from the spans afterwards would mean carrying it only to answer this once.
      if(kind == IR_BLOCK_PARAGRAPH && context->sawText && context->allMono) {
         IR_BLOCKptr block = IrBlockMutable(context->document, ui32(mark.block));

         if(block) block->kind = IR_BLOCK_CODE;
      }
      kept = IrEndBlock(context->document, mark);
   }
   context->sawText = outerText;
   context->allMono = outerMono;
   // Row 25: a lone bottom border on a paragraph that came to nothing is Word's autoformatted horizontal
   // rule. The test is "came to nothing" and not "has no runs", so a paragraph of empty runs is one too.
   if(ok && rule && !kept) {
      cIR_MARK ruled = IrBeginBlock(context->document, IR_BLOCK_RULE, 0);

      if(ruled.block < 0) {
         context->memory = true;
         return false;
      }
      IrEndBlock(context->document, ruled);
   }
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

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!tookFallback && XmlIsElement(context->reader, XML_NS_MC, "Fallback")) {
         if(tookChoice) {
            IrRewind(context->document, mark);
            context->sawText = markedText;
            context->allMono = markedMono;
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
   if(XmlIsElement(context->reader, XML_NS_W, "sdt")) return DocWalkStructuredTag(context, level, paragraphStyle, heading);
   if(XmlIsElement(context->reader, XML_NS_MC, "AlternateContent")) return DocWalkAlternate(context, level, paragraphStyle, heading);
   if(level == DOC_LEVEL_BLOCK) {
      if(XmlIsElement(context->reader, XML_NS_W, "p")) return DocWalkParagraph(context);
      // w:tbl waits for M9, w:sectPr describes page layout the mapping ignores, and everything else is
      // an element this build has not heard of. All three are skipped whole.
      return XmlSkipElement(context->reader);
   }
   if(XmlIsElement(context->reader, XML_NS_W, "r")) return DocWalkRun(context, paragraphStyle, heading);
   // A hyperlink and a simple field are run containers: their brackets and their field semantics arrive
   // at M7 and M10, but their text is content now and dropping it would lose part of the document.
   // w:dir and w:bdo are bidirectional run containers and nothing else; w:hyperlink and w:fldSimple get
   // their brackets and their field semantics at M7 and M10, but all four hold text that is content now.
   cbool container = XmlIsElement(context->reader, XML_NS_W, "hyperlink") || XmlIsElement(context->reader, XML_NS_W, "fldSimple") ||
                     XmlIsElement(context->reader, XML_NS_W, "dir") || XmlIsElement(context->reader, XML_NS_W, "bdo");

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

cWALK_STATUS DocWalk(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cSTYLE_MODELptr styles, csi32 partIndex) {
   WALK_STATUS status = {WALK_OK, XML_OK, OPC_OK};

   cOPC_RESULT loaded = OpcLoadXmlPart(package, partIndex);

   if(loaded != OPC_OK) {
      status.result = (loaded == OPC_ERROR_MEMORY ? WALK_ERROR_MEMORY : WALK_ERROR_PART);
      status.opc    = loaded;
      return status;
   }
   return DocWalkBytes(document, styles, OpcPartBytes(package, partIndex), OpcPartByteCount(package, partIndex));
}

cWALK_STATUS DocWalkBytes(IR_DOCUMENTptrc document, cSTYLE_MODELptr styles, cui8ptr bytes, cui64 byteCount) {
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

   context.document    = document;
   context.styles      = styles;
   context.reader      = &reader;
   context.cachedStyle = -1;
   context.cachedId[0] = 0;
   context.sawText     = false;
   context.allMono     = true;
   context.memory      = false;

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
