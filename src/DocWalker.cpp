/*
 * File: DocWalker.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: The body walk: transparent wrappers, paragraph properties, runs and run content.
 * To Do: 1) Choose an understood mc:Choice by its Requires prefix once an extension namespace is understood.
 *        2) Map w:sym through a Symbol and Wingdings table instead of dropping it.
 *        3) Cache more than one paragraph style if a document is ever found alternating between many.
 * Dependencies: BuildGuards.h, DocWalker.h, Ir.h, OpcPackage.h, StyleModel.h, XmlPull.h, typedefs.h,
 *               memory management.h, windows.h
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
   bool            memory;                         ///< Whether an allocation failed; sticky once set
};

typedef DOC_CONTEXT *const DOC_CONTEXTptrc;

static cbool DocWalkChildren(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading);
static cbool DocDispatchChild(DOC_CONTEXTptrc context, cDOC_LEVEL level, csi32 paragraphStyle, cbool heading);

//-- Small helpers

// Copies a view into a NUL-terminated buffer. Every view the reader hands out dies on the next XmlNext
// call, and a style identifier has to outlive the lookup that follows it.
static void DocCopyView(cXML_TEXT text, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   if(text.bytes) {
      while(used + 1u < destBytes && used < text.length) {
         dest[used] = text.bytes[used];
         ++used;
      }
   }
   dest[used] = 0;
}

// Resolves a style identifier, remembering the last one. Documents reuse a handful of styles over
// thousands of paragraphs, so one cached answer removes almost every linear scan of the style table.
static csi32 DocFindStyle(DOC_CONTEXTptrc context, cXML_TEXT value) {
   char identifier[STYLE_MAX_NAME_BYTES];

   DocCopyView(value, identifier, sizeof(identifier));
   if(!identifier[0]) return -1;

   ui64 index = 0;

   while(context->cachedId[index] && context->cachedId[index] == identifier[index]) ++index;
   if(context->cachedId[index] == identifier[index]) return context->cachedStyle;
   for(index = 0; index < sizeof(context->cachedId); ++index) context->cachedId[index] = identifier[index];
   context->cachedStyle = StyleFind(context->styles, identifier);
   return context->cachedStyle;
}

// Appends text to the open span, dropping the soft hyphens CONVERSION_REFERENCE row 34 says to remove.
// U+00AD is invisible and splits a word wherever a renderer decides not to hyphenate, so keeping one
// corrupts the word for every reader that does not.
static cbool DocAppendText(DOC_CONTEXTptrc context, cchptr bytes, cui64 byteCount) {
   ui64 run = 0;

   for(ui64 index = 0; index < byteCount; ++index) {
      cbool soft = (ui8(bytes[index]) == 0xC2u && index + 1u < byteCount && ui8(bytes[index + 1u]) == 0xADu);

      if(!soft) {
         ++run;
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
   return bits;
}

//-- Runs

// Reads the text of the w:t the reader is on into the open span.
static cbool DocReadTextElement(DOC_CONTEXTptrc context) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token == XML_TOKEN_TEXT) {
         // One w:t can arrive as several text tokens, because a comment or a processing instruction ends
         // a run of character data. Nothing here trims: xml:space is the producer's business, and
         // CONVERSION_REFERENCE 2.2 says to parse a w:t literally either way.
         if(!DocAppendText(context, context->reader->text.bytes, context->reader->text.length)) {
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

   StyleClearDirect(&direct);
   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!resolved && XmlIsElement(context->reader, XML_NS_W, "rPr")) {
         if(!DocReadRunProperties(context, &direct)) return false;
         continue;
      }

      // w:rPr is the first child when it is present at all, so the first non-property child is where the
      // run's formatting is settled once and for the whole run.
      if(!resolved) {
         cSTYLE_RUN_PROPS props = StyleResolveRun(context->styles, paragraphStyle, &direct);

         hidden = (props.toggles & StyleToggleBit(STYLE_TOGGLE_VANISH)) != 0;
         bits   = DocFormatBits(props);
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
         if(!DocReadTextElement(context)) return false;
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

// Reads the w:pPr the reader is on, and consumes it.
static cbool DocReadParagraphProperties(DOC_CONTEXTptrc context, si32ptrc style, si32ptrc outline) {
   cui32 depthHere = context->reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
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

// Walks one w:p into one block, which IrEndBlock throws away again when it holds nothing.
static cbool DocWalkParagraph(DOC_CONTEXTptrc context) {
   cui32   depthHere = context->reader->depth;
   si32    style     = StyleDefaultParagraph(context->styles);
   si32    outline   = -1;
   IR_MARK mark      = {-1, 0, 0};
   bool    begun     = false;
   bool    head      = false;
   bool    ok        = true;

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!begun && XmlIsElement(context->reader, XML_NS_W, "pPr")) {
         if(!DocReadParagraphProperties(context, &style, &outline)) return false;
         continue;
      }
      if(!begun) {
         // The properties are settled by the time any content is reached: w:pPr is the paragraph's first
         // child whenever it is present, so anything else means there is no more of it to come.
         cSTYLE_PARAGRAPH_PROPS props = StyleResolveParagraph(context->styles, style, outline);

         head  = (props.headingLevel > 0);
         mark  = IrBeginBlock(context->document, (head ? IR_BLOCK_HEADING : IR_BLOCK_PARAGRAPH), props.headingLevel);
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
   if(begun) IrEndBlock(context->document, mark);
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

   for(;;) {
      cXML_TOKEN token = XmlNext(context->reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && context->reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!tookFallback && XmlIsElement(context->reader, XML_NS_MC, "Fallback")) {
         if(tookChoice) IrRewind(context->document, mark);
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
   // mc:ProcessContent names an element to ignore while still processing its children, which is what
   // descending into it without doing anything else is.
   if(XmlIsElement(context->reader, XML_NS_MC, "ProcessContent")) return DocWalkChildren(context, level, paragraphStyle, heading);
   if(level == DOC_LEVEL_BLOCK) {
      if(XmlIsElement(context->reader, XML_NS_W, "p")) return DocWalkParagraph(context);
      // w:tbl waits for M9, w:sectPr describes page layout the mapping ignores, and everything else is
      // an element this build has not heard of. All three are skipped whole.
      return XmlSkipElement(context->reader);
   }
   if(XmlIsElement(context->reader, XML_NS_W, "r")) return DocWalkRun(context, paragraphStyle, heading);
   // A hyperlink and a simple field are run containers: their brackets and their field semantics arrive
   // at M7 and M10, but their text is content now and dropping it would lose part of the document.
   if(XmlIsElement(context->reader, XML_NS_W, "hyperlink") || XmlIsElement(context->reader, XML_NS_W, "fldSimple")) {
      return DocWalkChildren(context, DOC_LEVEL_RUN, paragraphStyle, heading);
   }
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
