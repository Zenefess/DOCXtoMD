/*
 * File: TestMdEmitter.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-23
 * Description: Unit tests for the arena, the blank-line discipline, delimiters and every block kind.
 * To Do: 1) Drive a fence inside a list item that also holds text, which needs the code paragraph and
 *           the text to be one item, and so waits for a block that can hold children.
 *        2) Drive a picture and a real external link inside a table cell, which needs a package.
 *        3) Drive an image with a real destination once a package can be built without an archive;
 *           with no package a reference resolves to nothing, so only the anchor half is reachable here.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, LinkResolver.h, MdEmitter.h,
 *               NumberingModel.h, RunCoalescer.h, StyleModel.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "DocWalker.h"
#include "Ir.h"
#include "LinkResolver.h"
#include "MdEmitter.h"
#include "NumberingModel.h"
#include "RunCoalescer.h"
#include "StyleModel.h"

//-- Helpers

// The three style parts the cases below reach for, named rather than repeated: a quote style, a
// code paragraph style and a code character style, each the shortest form that carries its role.
static constexpr cchptr STYLE_QUOTE = "<w:style w:type=\"paragraph\" w:styleId=\"Q\"><w:name w:val=\"Quote\"/></w:style>";
static constexpr cchptr STYLE_CODE  = "<w:style w:type=\"paragraph\" w:styleId=\"SC\"><w:name w:val=\"Source Code\"/></w:style>";
// Property bags and a paragraph reference the cases below carry often enough that spelling them
// out lands a line past e2's 150 columns once the formatter has joined what it can.
#define SUPER      "<w:vertAlign w:val=\"superscript\"/>"
#define SUB        "<w:vertAlign w:val=\"subscript\"/>"
#define CODE_STYLE "<w:rStyle w:val=\"CC\"/>"
#define IN_CODE    "<w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"

static constexpr cchptr STYLE_SPAN = "<w:style w:type=\"character\" w:styleId=\"CC\"><w:name w:val=\"Code\"/></w:style>";

// One numbering part covering every shape the list cases below need: a bullet definition, a decimal one
// starting at 3, a wide-marker one starting at 9 with a bullet beneath it, a restart over the decimal
// one, and a definition whose second level carries w:numFmt none.
//
// Each piece is one line on purpose. A multi-line macro has its backslashes aligned out to the
// formatter's own 180-column limit, which is past e2's 150 whatever the content is.
#define NUM_A "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>"
#define NUM_B "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"3\"/>"
#define NUM_C "<w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
#define NUM_D "<w:abstractNum w:abstractNumId=\"2\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"9\"/>"
#define NUM_E "<w:numFmt w:val=\"decimal\"/></w:lvl><w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>"
#define NUM_F "<w:abstractNum w:abstractNumId=\"3\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl>"
#define NUM_G "<w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"none\"/></w:lvl></w:abstractNum>"
#define NUM_H "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
#define NUM_I "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
#define NUM_J "<w:num w:numId=\"3\"><w:abstractNumId w:val=\"1\"/><w:lvlOverride w:ilvl=\"0\">"
#define NUM_K "<w:startOverride w:val=\"1\"/></w:lvlOverride></w:num>"
#define NUM_L "<w:num w:numId=\"4\"><w:abstractNumId w:val=\"2\"/></w:num>"
#define NUM_M "<w:num w:numId=\"5\"><w:abstractNumId w:val=\"3\"/></w:num>"

static constexpr cchptr NUMS = NUM_A NUM_B NUM_C NUM_D NUM_E NUM_F NUM_G NUM_H NUM_I NUM_J NUM_K NUM_L NUM_M;

// M9's table cases. A cell holding one run is what most of them are built from.
#define CELL(t)    "<w:tc><w:p><w:r><w:t>" t "</w:t></w:r></w:p></w:tc>"
#define ROW1(a)    "<w:tr>" CELL(a) "</w:tr>"
#define ROW2(a, b) "<w:tr>" CELL(a) CELL(b) "</w:tr>"
#define GRID(n)    "<w:tblGrid>" n "</w:tblGrid>"
#define COL        "<w:gridCol/>"
#define WIDE2      "<w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr>"
#define VRESTART   "<w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr>"
#define VMERGED    "<w:tcPr><w:vMerge/></w:tcPr>"
#define WIDE2V     "<w:tcPr><w:gridSpan w:val=\"2\"/><w:vMerge w:val=\"restart\"/></w:tcPr>"
#define GOBACK     "<w:p><w:bookmarkStart w:id=\"1\" w:name=\"_GoBack\"/><w:bookmarkEnd w:id=\"1\"/></w:p>"
#define ABCD       "<w:tr>" CELL("a") CELL("b") "</w:tr><w:tr>" CELL("c") CELL("d") "</w:tr></w:tbl>"
#define T1         "<w:tbl>" GRID(COL)
#define T2         "<w:tbl>" GRID(COL COL)
#define CELLS(c)   "<w:tr><w:tc>" c "</w:tc></w:tr></w:tbl>"

// M11's rows that start part-way across the grid, and the three-column table they are measured against.
#define ROWP(p, c)      "<w:tr><w:trPr>" p "</w:trPr>" c "</w:tr>"
#define GRID_BEFORE(n)  "<w:gridBefore w:val=\"" n "\"/>"
#define GRID_AFTER(n)   "<w:gridAfter w:val=\"" n "\"/>"
#define RESTART_CELL(t) "<w:tc>" VRESTART "<w:p><w:r><w:t>" t "</w:t></w:r></w:p></w:tc>"
#define ABC3            "<w:tbl>" GRID(COL COL COL) "<w:tr>" CELL("a") CELL("b") CELL("c") "</w:tr>"

// One numbered paragraph: its level, its numId and its text. Two halves, for the same reason.
#define NUM_PR(level, id)     "<w:numPr><w:ilvl w:val=\"" level "\"/><w:numId w:val=\"" id "\"/></w:numPr>"
#define ITEM_HEAD(level, id)  "<w:p><w:pPr>" NUM_PR(level, id) "</w:pPr>"
#define ITEM(level, id, text) ITEM_HEAD(level, id) "<w:r><w:t>" text "</w:t></w:r></w:p>"
#define BARE(id)              "<w:p><w:pPr><w:numPr><w:numId w:val=\"" id "\"/></w:numPr></w:pPr></w:p>"
#define CODE_ON(level, id)    "<w:p><w:pPr><w:pStyle w:val=\"SC\"/>" NUM_PR(level, id) "</w:pPr>"
#define CODE_LINE(l, id, txt) CODE_ON(l, id) "<w:r><w:t>" txt "</w:t></w:r></w:p>"
#define PARA(text)            "<w:p><w:r><w:t>" text "</w:t></w:r></w:p>"

// M10's pieces: a text run, a note reference of each story, one note of each, a note paragraph opening
// with the w:footnoteRef marker every Word note opens with, a quotation, a hard break and a complex
// field's w:fldChar runs. Then the wrappers that leave each case showing only what it is about -- a
// paragraph, a heading's and a quotation's properties, a horizontal rule, and a paragraph linking to
// the bookmark bm.
#define RUN(t)     "<w:r><w:t xml:space=\"preserve\">" t "</w:t></w:r>"
#define FREF(i)    "<w:r><w:footnoteReference w:id=\"" i "\"/></w:r>"
#define EREF(i)    "<w:r><w:endnoteReference w:id=\"" i "\"/></w:r>"
#define FOOT(i, b) "<w:footnote w:id=\"" i "\">" b "</w:footnote>"
#define ENDN(i, b) "<w:endnote w:id=\"" i "\">" b "</w:endnote>"
#define NOTE_P(t)  "<w:p><w:r><w:footnoteRef/></w:r>" RUN(" " t) "</w:p>"
#define QUOTED(t)  PARA_OF(IN_QUOTE RUN(t))
#define BR         "<w:r><w:br/></w:r>"
#define FLD_BEGIN  "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
#define FLD_SEP    "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
#define FLD(c)     FLD_BEGIN "<w:r><w:instrText>" c "</w:instrText></w:r>" FLD_SEP
#define FLD_END    "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r>"
#define PARA_OF(b) "<w:p>" b "</w:p>"
#define IN_H1      "<w:pPr><w:pStyle w:val=\"H1\"/></w:pPr>"
#define IN_QUOTE   "<w:pPr><w:pStyle w:val=\"Q\"/></w:pPr>"
#define RULE_P     "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>"
#define TO_BM      "<w:p><w:hyperlink w:anchor=\"bm\">" RUN("go") "</w:hyperlink></w:p>"

// A heading style, for the cases that have to show a slug the renderer will generate for itself.
static constexpr cchptr STYLE_H1 = "<w:style w:type=\"paragraph\" w:styleId=\"H1\"><w:name w:val=\"heading 1\"/></w:style>";

// The root element every body below is wrapped in, kept out of the helper so no line reaches the
// column limit once the formatter has joined what it can.
static constexpr cchptr EMIT_HEAD = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>";
static constexpr cchptr EMIT_TAIL = "</w:body></w:document>";

// Bytes before the terminator.
static cui64 EmitLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Whether an emitter's bytes are exactly a literal, terminator excluded.
static cbool EmittedIs(cMD_EMITTERptr emitter, cchptr wanted) {
   cui64 length = EmitLength(wanted);

   if(MdByteCount(emitter) != length) return false;
   for(ui64 index = 0; index < length; ++index) {
      if(MdBytes(emitter)[index] != wanted[index]) return false;
   }
   return true;
}

// Adds one text span carrying a NUL-terminated literal.
static cbool EmitText(IR_DOCUMENTptrc document, cchptr text) {
   if(!IrAddSpan(document, IR_SPAN_TEXT, IR_FMT_NONE)) return false;
   return IrAppendText(document, text, EmitLength(text));
}

// The notes parts a case supplies, and the policies it runs under. A null notes body is a document
// without that part.
struct EMIT_CASE {
   cchptr     footnotes; ///< The children of w:footnotes, or null
   cchptr     endnotes;  ///< The children of w:endnotes, or null
   HARD_BREAK hardBreak; ///< How a hard break is spelled
   TABLE_MODE tables;    ///< What a merged table is written as
};

typedef const EMIT_CASE *const cEMIT_CASEptrc;

// Walks one notes part, wrapped in its root element, into a document the body has already been walked into.
static cbool EmitNotes(IR_DOCUMENTptrc document, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, cchptr notes, cIR_NOTE_KIND kind) {
   char   part[4096];
   ui64   used = 0;
   cchptr root = (kind == IR_NOTE_END ? "endnotes" : "footnotes");

   if(!notes) return true;

   cchptr pieces[5] = {"<w:", root, " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">", notes, "</w:"};

   for(ui32 index = 0; index < 5u; ++index) {
      cui64 length = EmitLength(pieces[index]);

      for(ui64 at = 0; at < length && used + 1u < sizeof(part); ++at) part[used++] = pieces[index][at];
   }
   for(ui64 at = 0; root[at] && used + 2u < sizeof(part); ++at) part[used++] = root[at];
   part[used++] = '>';
   return DocWalkNotesBytes(document, styles, numbering, (cui8ptr)part, used, kind, -1).result == WALK_OK;
}

// Converts one body, with an optional styles part, an optional numbering part and optional notes parts
// in front of it, straight through to Markdown.
static cbool ConvertsFull(cchptr styleBody, cchptr numberBody, cchptr body, cchptr wanted, cEMIT_CASEptrc policy) {
   char part[4096];
   ui64 used = 0;

   cchptr pieces[3] = {EMIT_HEAD, body, EMIT_TAIL};

   for(ui32 index = 0; index < 3u; ++index) {
      cui64 length = EmitLength(pieces[index]);

      for(ui64 at = 0; at < length && used + 1u < sizeof(part); ++at) part[used++] = pieces[index][at];
   }
   part[used] = 0;

   STYLE_MODEL styles;
   NUM_MODEL   numbering;
   IR_DOCUMENT document;
   MD_EMITTER  emitter;

   StyleOpen(&styles);
   NumOpen(&numbering);
   if(styleBody) {
      char stylePart[2048];
      ui64 styleUsed = 0;

      cchptr wrap[3] = {"<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">", styleBody, "</w:styles>"};

      for(ui32 index = 0; index < 3u; ++index) {
         cui64 length = EmitLength(wrap[index]);

         for(ui64 at = 0; at < length && styleUsed + 1u < sizeof(stylePart); ++at) stylePart[styleUsed++] = wrap[index][at];
      }
      stylePart[styleUsed] = 0;
      if(StyleLoadBytes(&styles, (cui8ptr)stylePart, styleUsed) != STYLE_OK) {
         NumClose(&numbering);
         StyleClose(&styles);
         return false;
      }
   }
   if(numberBody) {
      char numberPart[2048];
      ui64 numberUsed = 0;

      cchptr wrap[3] = {"<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">", numberBody, "</w:numbering>"};

      for(ui32 index = 0; index < 3u; ++index) {
         cui64 length = EmitLength(wrap[index]);

         for(ui64 at = 0; at < length && numberUsed + 1u < sizeof(numberPart); ++at) numberPart[numberUsed++] = wrap[index][at];
      }
      numberPart[numberUsed] = 0;
      if(NumLoadBytes(&numbering, (cui8ptr)numberPart, numberUsed, &styles) != NUM_OK) {
         NumClose(&numbering);
         StyleClose(&styles);
         return false;
      }
   }
   IrOpen(&document);
   MdOpen(&emitter, policy->hardBreak, policy->tables);

   cWALK_STATUS status  = DocWalkBytes(&document, &styles, &numbering, (cui8ptr)part, used);
   bool         matched = false;

   // Every pass Convert.cpp runs between the walk and the emitter, except MediaPlan, runs here too, in
   // the same order: the emitter's contract since M6 is that every formatted span it is handed is
   // already merged and already trimmed, and since M7 that every link it is handed has a destination
   // and every block it is handed produces a byte. Testing it against a document no pass had been over
   // would test a shape the program never produces. There is no package here, so a relationship
   // resolves to nothing and only a w:anchor link reaches the emitter with a destination -- which is
   // the half that needs no archive to be worth testing.
   bool ready = status.result == WALK_OK && EmitNotes(&document, &styles, &numbering, policy->footnotes, IR_NOTE_FOOT);

   if(ready) ready = EmitNotes(&document, &styles, &numbering, policy->endnotes, IR_NOTE_END);
   if(ready) ready = RunCoalesce(&document);
   if(ready) ready = LinkResolveRefs(&document, nullptr, -1);
   if(ready) ready = LinkResolveNotes(&document);
   if(ready) ready = LinkResolveAnchors(&document);
   if(ready) ready = RunCoalesce(&document); // Again, because muting makes spans adjacent -- see Convert.cpp
   if(ready) ready = NumAssignMarkers(&document, &numbering);
   if(ready) IrDropEmptyBlocks(&document);
   if(ready && MdEmitDocument(&emitter, &document) == MD_OK) {
      matched = EmittedIs(&emitter, wanted);
   }
   MdClose(&emitter);
   IrClose(&document);
   NumClose(&numbering);
   StyleClose(&styles);
   return matched;
}

// Converts one body with no notes parts, under chosen policies.
static cbool ConvertsUnder(cchptr styleBody, cchptr numberBody, cchptr body, cchptr wanted, cHARD_BREAK hardBreak, cTABLE_MODE tables) {
   EMIT_CASE policy = {nullptr, nullptr, hardBreak, tables};

   return ConvertsFull(styleBody, numberBody, body, wanted, &policy);
}

// Converts one body and its footnotes and endnotes, under the default policies, against the numbering
// part the list cases use and a styles part holding the quote, code and heading styles.
static cbool Noted(cchptr body, cchptr footnotes, cchptr endnotes, cchptr wanted) {
   EMIT_CASE policy = {footnotes, endnotes, HARD_BREAK_BACKSLASH, TABLE_MODE_GFM};
   char      style[512];
   ui64      used = 0;

   cchptr pieces[3] = {STYLE_QUOTE, STYLE_CODE, STYLE_H1};

   for(ui32 index = 0; index < 3u; ++index) {
      cui64 length = EmitLength(pieces[index]);

      for(ui64 at = 0; at < length && used + 1u < sizeof(style); ++at) style[used++] = pieces[index][at];
   }
   style[used] = 0;
   return ConvertsFull(style, NUMS, body, wanted, &policy);
}

// Converts a body of one paragraph citing footnote 2, whose content is the literal given. The cases about
// what a note may hold differ in nothing else, so this is what keeps each of them to one line.
static cbool Cited(cchptr note, cchptr wanted) {
   char notes[2048];
   ui64 used = 0;

   cchptr pieces[3] = {"<w:footnote w:id=\"2\">", note, "</w:footnote>"};

   for(ui32 index = 0; index < 3u; ++index) {
      cui64 length = EmitLength(pieces[index]);

      for(ui64 at = 0; at < length && used + 1u < sizeof(notes); ++at) notes[used++] = pieces[index][at];
   }
   notes[used] = 0;
   return Noted(PARA_OF(FREF("2")), notes, nullptr, wanted);
}

// Converts one body against optional styles and numbering parts, under a chosen hard-break policy and
// the default table policy, which is every case but the ones that name --tables.
static cbool ConvertsWith(cchptr styleBody, cchptr numberBody, cchptr body, cchptr wanted, cHARD_BREAK hardBreak) {
   return ConvertsUnder(styleBody, numberBody, body, wanted, hardBreak, TABLE_MODE_GFM);
}

// Appends a NUL-terminated literal to a buffer the caller has sized, for the cases that build one in a loop.
static void EmitPut(chptrc dest, ui64ptrc length, cchptr text) {
   for(ui64 index = 0; text[index]; ++index) dest[(*length)++] = text[index];
   dest[*length] = 0;
}

// Converts under the html-on-merge policy, which is the one thing --tables changes.
static cbool Merged(cchptr body, cchptr wanted) {
   cTABLE_MODE html = TABLE_MODE_HTML_ON_MERGE;

   return ConvertsUnder(nullptr, nullptr, body, wanted, HARD_BREAK_BACKSLASH, html);
}

// Converts one body with no styles or numbering part, under a chosen hard-break policy.
static cbool ConvertsTo(cchptr body, cchptr wanted, cHARD_BREAK hardBreak) { return ConvertsWith(nullptr, nullptr, body, wanted, hardBreak); }

// Converts one body with the default hard-break policy, which is what all but one case wants.
static cbool Converts(cchptr body, cchptr wanted) { return ConvertsTo(body, wanted, HARD_BREAK_BACKSLASH); }

// Converts one body against a styles part, which is the only way to reach a quote or a code block.
static cbool Styled(cchptr styleBody, cchptr body, cchptr wanted) { return ConvertsWith(styleBody, nullptr, body, wanted, HARD_BREAK_BACKSLASH); }

// Converts one body against a numbering part, which is the only way to reach a list.
static cbool Listed(cchptr numberBody, cchptr body, cchptr wanted) { return ConvertsWith(nullptr, numberBody, body, wanted, HARD_BREAK_BACKSLASH); }

// The same, under a chosen hard-break policy, for the continuation line inside an item.
static cbool ListedTo(cchptr numbers, cchptr body, cchptr wanted, cHARD_BREAK breaks) { return ConvertsWith(nullptr, numbers, body, wanted, breaks); }

//== The suite

void TestMdEmitter(void);

void TestMdEmitter(void) {
   CheckGroup("Ir: blocks, spans and the arena");
   IR_DOCUMENT document;

   IrOpen(&document);
   CHECK(IrBlockCount(&document) == 0);
   CHECK(IrBlockAt(&document, 0) == nullptr);
   CHECK(IrSpanAt(&document, 0) == nullptr);
   CHECK(!IrFailed(&document));

   cIR_MARK first = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(first.block == 0);
   CHECK(EmitText(&document, "hello"));
   CHECK(IrEndBlock(&document, first));
   CHECK(IrBlockCount(&document) == 1u);
   CHECK(IrBlockAt(&document, 0)->spanCount == 1u);
   CHECK(IrSpanAt(&document, 0)->textBytes == 5u);

   // A block holding nothing but ASCII whitespace is unwound completely, arena included.
   cui64    before = document.heapUsed;
   cIR_MARK blank  = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "   "));
   CHECK(!IrEndBlock(&document, blank));
   CHECK(IrBlockCount(&document) == 1u);
   CHECK(document.heapUsed == before);

   // A non-breaking space is content, and a paragraph written to hold one keeps it.
   cIR_MARK nbsp = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "\xC2\xA0"));
   CHECK(IrEndBlock(&document, nbsp));
   CHECK(IrBlockCount(&document) == 2u);

   // A mark taken between blocks unwinds every block added after it.
   cIR_MARK here  = IrMark(&document);
   cIR_MARK extra = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "gone"));
   CHECK(IrEndBlock(&document, extra));
   CHECK(IrBlockCount(&document) == 3u);
   IrRewind(&document, here);
   CHECK(IrBlockCount(&document) == 2u);
   IrClose(&document);
   CHECK(IrBlockCount(&document) == 0);

   CheckGroup("MdEmitter: the blank-line discipline");
   MD_EMITTER emitter;

   IrOpen(&document);
   MdOpen(&emitter, HARD_BREAK_BACKSLASH, TABLE_MODE_GFM);
   CHECK(MdEmitDocument(&emitter, &document) == MD_OK);
   CHECK(MdByteCount(&emitter) == 0);

   cIR_MARK one = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "a"));
   CHECK(IrEndBlock(&document, one));
   CHECK(MdEmitDocument(&emitter, &document) == MD_OK);
   CHECK(EmittedIs(&emitter, "a\n"));
   MdClose(&emitter);

   cIR_MARK two = IrBeginBlock(&document, IR_BLOCK_HEADING, 2u);

   CHECK(EmitText(&document, "b"));
   CHECK(IrEndBlock(&document, two));
   MdOpen(&emitter, HARD_BREAK_BACKSLASH, TABLE_MODE_GFM);
   CHECK(MdEmitDocument(&emitter, &document) == MD_OK);
   CHECK(EmittedIs(&emitter, "a\n\n## b\n"));
   MdClose(&emitter);
   IrClose(&document);

   CheckGroup("MdEmitter: paragraphs, trimming and line starts");
   CHECK(Converts("<w:p><w:r><w:t>plain</w:t></w:r></w:p>", "plain\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">  padded  </w:t></w:r></w:p>", "padded\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">  a</w:t></w:r><w:r><w:t xml:space=\"preserve\">b  </w:t></w:r></w:p>", "ab\n"));
   CHECK(Converts("<w:p><w:r><w:t># head</w:t></w:r></w:p>", "\\# head\n"));
   CHECK(Converts("<w:p><w:r><w:t>1. item</w:t></w:r></w:p>", "1\\. item\n"));
   CHECK(Converts("<w:p><w:r><w:t>a*b</w:t></w:r></w:p>", "a\\*b\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r></w:p><w:p><w:r><w:t>b</w:t></w:r></w:p>", "a\n\nb\n"));

   CheckGroup("MdEmitter: hard breaks");
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "a\\\nb\n"));
   CHECK(ConvertsTo("<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "a  \nb\n", HARD_BREAK_SPACES));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>- b</w:t></w:r></w:p>", "a\\\n\\- b\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:br/><w:t>b</w:t></w:r></w:p>", "a\\\nb\n"));
   CHECK(Converts("<w:p><w:r><w:t>a\\</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "a\\\\\\\nb\n"));
   // A break whose next line turns out to hold nothing but padding is dropped rather than left dangling.
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>", "a\n"));

   CheckGroup("MdEmitter: the whole output obeys its own invariants");
   {
      IR_DOCUMENT many;
      MD_EMITTER  writer;

      IrOpen(&many);
      for(ui32 index = 0; index < 6u; ++index) {
         cIR_MARK at = IrBeginBlock(&many, (index & 1u ? IR_BLOCK_HEADING : IR_BLOCK_PARAGRAPH), ui8(index & 1u ? 3u : 0u));

         CHECK(EmitText(&many, "  text  "));
         CHECK(IrEndBlock(&many, at));
      }
      MdOpen(&writer, HARD_BREAK_BACKSLASH, TABLE_MODE_GFM);
      CHECK(MdEmitDocument(&writer, &many) == MD_OK);
      CHECK(MdByteCount(&writer) > 0);
      CHECK(MdBytes(&writer)[0] != '\n');
      CHECK(MdBytes(&writer)[MdByteCount(&writer) - 1u] == '\n');

      bool clean = true;

      for(ui64 index = 0; index < MdByteCount(&writer); ++index) {
         cchar byte = MdBytes(&writer)[index];

         if(byte == '\r' || !byte) clean = false;
         if(byte == '\n' && index + 2u < MdByteCount(&writer) && MdBytes(&writer)[index + 1u] == '\n' && MdBytes(&writer)[index + 2u] == '\n') {
            clean = false;
         }
         if(index + 1u == MdByteCount(&writer)) continue;
         if(byte == ' ' && MdBytes(&writer)[index + 1u] == '\n') clean = false;
      }
      CHECK(clean);
      MdClose(&writer);
      IrClose(&many);
   }

   CheckGroup("MdEmitter: a line is escaped once, not span by span");
   // Word fragments a run wherever a revision or spellcheck boundary falls, so the output must not
   // depend on where the split landed. Both spellings of the same text produce the same bytes.
   CHECK(Converts("<w:p><w:r><w:t>A&amp;amp;B</w:t></w:r></w:p>", "A&amp;amp;B\n"));
   CHECK(Converts("<w:p><w:r><w:t>A&amp;</w:t></w:r><w:r><w:t>amp;B</w:t></w:r></w:p>", "A&amp;amp;B\n"));
   CHECK(Converts("<w:p><w:r><w:t>AT&amp;T</w:t></w:r></w:p>", "AT&T\n"));
   CHECK(Converts("<w:p><w:r><w:t>AT&amp;</w:t></w:r><w:r><w:t>T</w:t></w:r></w:p>", "AT&T\n"));
   // The same for a construct the line-start pass judges, which needs the whole line to judge it.
   CHECK(Converts("<w:p><w:r><w:t>19</w:t></w:r><w:r><w:t>98. a year</w:t></w:r></w:p>", "1998\\. a year\n"));
   CHECK(Converts("<w:p><w:r><w:t>-</w:t></w:r><w:r><w:t>--</w:t></w:r></w:p>", "\\---\n"));
   CHECK(Converts("<w:p><w:r><w:t>a&lt;</w:t></w:r><w:r><w:t>b&gt;</w:t></w:r></w:p>", "a\\<b>\n"));
   CHECK(Converts("<w:p><w:r><w:t>a&lt;b&gt;</w:t></w:r></w:p>", "a\\<b>\n"));

   CheckGroup("MdEmitter: a setext underline needs a line above it");
   CHECK(Converts("<w:p><w:r><w:t>===</w:t></w:r></w:p>", "===\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>===</w:t></w:r></w:p>", "a\\\n\\===\n"));
   CHECK(Converts("<w:p><w:r><w:t>--</w:t></w:r></w:p>", "--\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>--</w:t></w:r></w:p>", "a\\\n\\--\n"));

   CheckGroup("MdEmitter: headings");
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "# t\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"5\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "###### t\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"8\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "###### t\n"));
   // A heading is one line, so a break inside it becomes a space, and its content is not a line start.
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "# a b\n"));
   // Exactly one space, whatever padding stood on either side of the break: two would render as one
   // and would only put an invisible difference into a file that is compared byte for byte.
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
                  "<w:r><w:t xml:space=\"preserve\">a </w:t><w:br/>"
                  "<w:t xml:space=\"preserve\"> b</w:t></w:r></w:p>",
                  "# a b\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>- b</w:t></w:r></w:p>", "# - b\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>Sharp #</w:t></w:r></w:p>", "# Sharp \\#\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>C#</w:t></w:r></w:p>", "# C#\n"));

   CheckGroup("MdEmitter: the inline delimiters");
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r></w:p>", "**a**\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:i/></w:rPr><w:t>a</w:t></w:r></w:p>", "*a*\n"));
   // Mapping row 5 fixes the order, so no document depends on which of the two a producer named first.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>a</w:t></w:r></w:p>", "***a***\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:i/><w:b/></w:rPr><w:t>a</w:t></w:r></w:p>", "***a***\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:strike/></w:rPr><w:t>a</w:t></w:r></w:p>", "~~a~~\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:dstrike/></w:rPr><w:t>a</w:t></w:r></w:p>", "~~a~~\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "<sup>a</sup>\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:vertAlign w:val=\"subscript\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "<sub>a</sub>\n"));
   // The nesting is fixed outermost first: the HTML wrapper, then the strike, then the emphasis.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:i/><w:strike/></w:rPr><w:t>a</w:t></w:r></w:p>", "<del>***a***</del>\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:strike/>" SUPER "</w:rPr><w:t>a</w:t></w:r></w:p>", "<sup><del>**a**</del></sup>\n"));
   // Underline and highlight have no Markdown equivalent and are dropped (rows 8 and 9).
   CHECK(Converts("<w:p><w:r><w:rPr><w:u w:val=\"single\"/><w:highlight w:val=\"yellow\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "a\n"));
   // Inside a raw-HTML wrapper the text still needs its two entities, which no other context adds.
   CHECK(Converts("<w:p><w:r><w:rPr>" SUB "</w:rPr><w:t>a &amp; &lt;b&gt;</w:t></w:r></w:p>", "<sub>a &amp; &lt;b></sub>\n"));
   // Two adjacent formatted spans put their delimiter runs side by side, which CommonMark resolves the
   // way the source meant: "**bo**" then "***ld***" is bold then bold-italic, not one run of five.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bo</w:t></w:r>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>ld</w:t></w:r></w:p>",
                  "**bo*****ld***\n"));
   // A literal asterisk inside a formatted span is escaped, so it cannot join the emitted delimiter.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>*a*</w:t></w:r></w:p>", "**\\*a\\***\n"));
   // A heading's bold is style-borne (row 1), but its italic is not and survives.
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>a</w:t></w:r></w:p>",
                  "# *a*\n"));

   CheckGroup("MdEmitter: the flanking fallback to raw HTML");
   // CommonMark refuses a delimiter run that is both preceded by a letter and followed by punctuation,
   // and the mirror image at the closing end -- so "word**(a)**after" would emit four literal asterisks
   // and lose the emphasis. An HTML element has no flanking rule at all, so it takes over there.
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>(a)</w:t></w:r>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "word<strong>(a)</strong>after\n"));
   // The mirror image, which the cases above cannot reach: nothing stands in front of the span but a
   // space, so the opening half is satisfied and only the closing half can refuse. Delete it and
   // "word **a(**b" is emitted, which renders as five literal characters and no emphasis.
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">word </w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>a(</w:t></w:r><w:r><w:t>b</w:t></w:r></w:p>",
                  "word <strong>a(</strong>b\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">x </w:t></w:r>"
                  "<w:r><w:rPr><w:strike/></w:rPr><w:t>a#</w:t></w:r><w:r><w:t>b</w:t></w:r></w:p>",
                  "x <del>a#</del>b\n"));
   // Every Zs counts as whitespace for flanking, not the ASCII space alone, so an EN SPACE in front of
   // the span is enough to keep the Markdown spelling -- reading it as a word character would spend an
   // HTML element where a delimiter parses perfectly well.
   // Every member of the class needs its own case: the emitter's classifier is reached only through the
   // *preceding* span, which no fixture exercises, so a member left out of it goes unnoticed while an
   // HTML element is silently spent where a delimiter parses. Verified against markdown-it: all six
   // license the Markdown spelling, and U+200B, being Cf, correctly does not.
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#8194;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xE2\x80\x82"
                  "**(b**c\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#160;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xC2\xA0"
                  "**(b**c\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#5760;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xE1\x9A\x80"
                  "**(b**c\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#8239;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xE2\x80\xAF"
                  "**(b**c\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#8287;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xE2\x81\x9F"
                  "**(b**c\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#12288;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xE3\x80\x80"
                  "**(b**c\n"));
   // U+200B is Cf rather than Zs, so it is a word character here and the element is correct.
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">a&#8203;</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>(b</w:t></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                  "a\xE2\x80\x8B"
                  "<strong>(b</strong>c\n"));
   // A space on either side is all it takes for the Markdown spelling to be safe again, which is why
   // the fallback is rare: it needs punctuation at the very edge of the span and no space outside it.
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">word </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>(a)</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\"> after</w:t></w:r></w:p>",
                  "word **(a)** after\n"));
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>a</w:t></w:r>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "word*a*after\n"));
   // A strikethrough is the one that cannot survive wrapping anything: its opening "~~" is then always
   // followed by punctuation, so a letter in front of it is enough on its own.
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r><w:r><w:rPr><w:b/><w:strike/></w:rPr><w:t>struck</w:t></w:r>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "word<del>**struck**</del>after\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:strike/></w:rPr><w:t>#hash</w:t></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "a<del>#hash</del>b\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:strike/></w:rPr><w:t>plain</w:t></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "a~~plain~~b\n"));
   // Two delimiter runs that meet are one run to a parser, so what stands in front of the first is what
   // the flanking rules look at: "***T***" then "**=eq=**" is a run of five preceded by a letter.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>T</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>=eq=</w:t></w:r></w:p>",
                  "***T***<strong>=eq=</strong>\n"));
   // The same shape with no punctuation at the seam needs no fallback, and CommonMark reads the run of
   // five the way the source meant it.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bo</w:t></w:r>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>ld</w:t></w:r></w:p>",
                  "**bo*****ld***\n"));
   // An element already shields whatever is inside it, so a superscript never needs the fallback.
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r>"
                  "<w:r><w:rPr><w:b/>" SUPER "</w:rPr><w:t>(a)</w:t></w:r><w:r><w:t>after</w:t></w:r></w:p>",
                  "word<sup>**(a)**</sup>after\n"));
   // A code span has no flanking rule of its own, so it keeps its backticks wherever it stands.
   CHECK(Styled(STYLE_SPAN,
                "<w:p><w:r><w:t>word</w:t></w:r>"
                "<w:r><w:rPr>" CODE_STYLE "</w:rPr><w:t>(a)</w:t></w:r><w:r><w:t>after</w:t></w:r></w:p>",
                "word`(a)`after\n"));

   CheckGroup("MdEmitter: code spans and their delimiter sizing");
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "`x`\n"));
   // A backtick inside a code span cannot be escaped, so the delimiter grows past the longest run.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>a`b</w:t></w:r></w:p>", "``a`b``\n"));
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>a``b</w:t></w:r></w:p>", "```a``b```\n"));
   // Content that begins or ends with a backtick is padded, and CommonMark strips exactly that pair.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>`x</w:t></w:r></w:p>", "`` `x ``\n"));
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>x`</w:t></w:r></w:p>", "`` x` ``\n"));
   // Nothing at all is escaped inside a code span.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>*a* &amp; &lt;b&gt;</w:t></w:r></w:p>", "`*a* & <b>`\n"));
   // Row 11's ruling on the collision: code drops bold and italic, and keeps what wraps it cleanly.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:b/><w:i/><w:rStyle w:val=\"CC\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "`x`\n"));
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:strike/>" SUPER CODE_STYLE "</w:rPr><w:t>x</w:t></w:r></w:p>", "<sup><del>`x`</del></sup>\n"));

   CheckGroup("MdEmitter: fenced code blocks");
   CHECK(Styled(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "```\na\n```\n"));
   // Consecutive code paragraphs merge into one fence, and the leading whitespace is the indentation.
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t xml:space=\"preserve\">  b</w:t></w:r></w:p>",
                "```\na\n  b\n```\n"));
   // An empty code paragraph is a blank line of the fence, and nothing at either end of one.
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>",
                "```\na\n\nb\n```\n"));
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>",
                "```\na\n```\n"));
   // A fence of nothing but blank code paragraphs is no fence at all, and leaves no separator behind.
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:r><w:t>x</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>"
                "<w:p><w:r><w:t>y</w:t></w:r></w:p>",
                "x\n\ny\n"));
   // The fence is longer than the longest backtick run inside it, or the content would close it.
   CHECK(Styled(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a ``` b</w:t></w:r></w:p>", "````\na ``` b\n````\n"));
   // Nothing is escaped in a fence, and a hard break inside one is simply the next line.
   CHECK(Styled(STYLE_CODE, "<w:p>" IN_CODE "<w:r><w:t>*a*</w:t><w:br/><w:t># b</w:t></w:r></w:p>", "```\n*a*\n# b\n```\n"));
   // Row 12's second detection: every run monospace makes the paragraph a fence with no style at all.
   CHECK(Converts("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a();</w:t></w:r></w:p>", "```\na();\n```\n"));

   CheckGroup("MdEmitter: three delimiter runs meeting");
   // CommonMark matches openers to closers by run length -- its rule of three -- so three emphasis
   // spans that meet with no text between them can leave a run no pairing resolves, and
   // "**bo*****th****ree*" loses all three to six literal asterisks. A span abutted by an identical run
   // on both sides is written as an element instead, which has neither a run length nor a flanking rule.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bo</w:t></w:r>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>th</w:t></w:r>"
                  "<w:r><w:rPr><w:i/></w:rPr><w:t>ree</w:t></w:r></w:p>",
                  "**bo**<strong><em>th</em></strong>*ree*\n"));
   // Two of them meeting is not three, and CommonMark resolves that pair the way the source meant it,
   // so the narrower shape keeps the Markdown spelling.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bo</w:t></w:r>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>ld</w:t></w:r></w:p>",
                  "**bo*****ld***\n"));
   // A run of a different character on one side does not abut, so neither does the test fire.
   CHECK(Converts("<w:p><w:r><w:rPr><w:strike/></w:rPr><w:t>a</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>b</w:t></w:r>"
                  "<w:r><w:rPr><w:i/></w:rPr><w:t>c</w:t></w:r></w:p>",
                  "~~a~~**b***c*\n"));

   CheckGroup("MdEmitter: what a fence must not lose");
   // The backtick run is measured across a block's spans, not within each: a code block's spans need
   // not carry equal formatting, and a fence of three would be closed by the content's own three.
   CHECK(Styled(STYLE_CODE,
                "<w:p>" IN_CODE "<w:r><w:rPr><w:b/></w:rPr><w:t>``</w:t></w:r>"
                "<w:r><w:t>`x</w:t></w:r></w:p>",
                "````\n```x\n````\n"));
   // A code paragraph of nothing but padding is a blank line, so it is trimmed at the fence's edge --
   // by the same test IrEndBlock uses on every other kind, not by a byte count.
   CHECK(Styled(STYLE_CODE,
                "<w:p>" IN_CODE "<w:r><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>"
                "<w:p>" IN_CODE "<w:r><w:t>a</w:t></w:r></w:p>",
                "```\na\n```\n"));
   // Inside a fence a break is a real newline and no marker is written for it, so a trailing one is a
   // blank line of the code rather than the stray marker IrEndBlock trims everywhere else.
   CHECK(Styled(STYLE_CODE, "<w:p>" IN_CODE "<w:r><w:t>x</w:t><w:br/></w:r></w:p>", "```\nx\n\n```\n"));

   CheckGroup("MdEmitter: blockquotes");
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "> a\n"));
   // Every line of the block takes the prefix, the continuation line included.
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "> a\\\n> b\n"));
   // Two consecutive quote paragraphs are one quotation: a blank line would close the blockquote and
   // open a second, so the separator is a bare marker instead. That is the one exception to the rule.
   CHECK(Styled(STYLE_QUOTE,
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>",
                "> a\n>\n> b\n"));
   // A paragraph between them really does end the quotation.
   CHECK(Styled(STYLE_QUOTE,
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:r><w:t>x</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>",
                "> a\n\nx\n\n> b\n"));
   // The line-start pass runs on the content after the prefix, not on the marker the emitter wrote.
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>- a</w:t></w:r></w:p>", "> \\- a\n"));
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r></w:p>", "> **a**\n"));

   CheckGroup("MdEmitter: lists");
   {
      cchptr wide    = ITEM("0", "4", "Nine") ITEM("1", "4", "under nine") ITEM("0", "4", "Ten") ITEM("1", "4", "under ten");
      cchptr skip    = ITEM("0", "1", "a") ITEM("3", "1", "deep") ITEM("0", "1", "b");
      cchptr split   = ITEM("0", "2", "a") ITEM("0", "2", "b") ITEM("0", "3", "c");
      cchptr broken  = ITEM("0", "2", "a") PARA("between") ITEM("0", "2", "b");
      cchptr hollow  = ITEM("0", "1", "a") BARE("1") ITEM("0", "1", "c");
      cchptr edge    = ITEM("0", "1", "a") BARE("1");
      cchptr carry   = ITEM("0", "5", "first") ITEM("1", "5", "more") ITEM("0", "5", "second");
      cchptr looks   = ITEM("0", "1", "- not a bullet") ITEM("0", "1", "1. not ordered");
      cchptr around  = PARA("before") ITEM("0", "1", "a") PARA("after");
      cchptr broke2  = ITEM_HEAD("0", "2") "<w:r><w:t>one</w:t></w:r><w:r><w:br/></w:r><w:r><w:t>two</w:t></w:r></w:p>";
      cchptr broke1  = ITEM_HEAD("0", "1") "<w:r><w:t>one</w:t></w:r><w:r><w:br/></w:r><w:r><w:t>two</w:t></w:r></w:p>";
      cchptr buried  = ITEM("0", "2", "a") ITEM("1", "2", "b") ITEM("0", "3", "c");
      cchptr cont    = ITEM("0", "2", "a") ITEM("1", "2", "b") ITEM("0", "2", "c");
      cchptr spent   = ITEM("0", "2", "a") ITEM("1", "3", "b") ITEM("0", "3", "c");
      cchptr hollow2 = ITEM("0", "5", "one") ITEM_HEAD("1", "5") "</w:p>" ITEM("1", "5", "more") ITEM("0", "5", "two");
      cchptr orphan  = ITEM_HEAD("0", "4") "</w:p>" ITEM("1", "4", "child") ITEM("0", "4", "later");
      cchptr voided  = ITEM("0", "5", "a") ITEM_HEAD("1", "5") "</w:p>" ITEM("0", "5", "b");

      // A tight list: nothing at all stands between two items of one list, which is the whole reason a
      // run of them is emitted as a group rather than through the block separator.
      CHECK(Listed(NUMS, ITEM("0", "1", "one") ITEM("0", "1", "two"), "- one\n- two\n"));
      // Mapping row 15: real computed numbers, and w:start honoured on the first item -- which is the
      // only one a renderer reads, so a w:start dropped on the floor is invisible anywhere but here.
      CHECK(Listed(NUMS, ITEM("0", "2", "one") ITEM("0", "2", "two"), "3. one\n4. two\n"));
      // A child is indented to its parent item's *content* column, which is a fact about the marker the
      // parent actually wrote: "9. " is three columns and "10. " is four, so two siblings of one list
      // have their children indented differently. A fixed step per level is correct only by luck, and
      // stops being correct at item 10 -- where the child flattens into the outer list without a sound.
      CHECK(Listed(NUMS, wide, "9. Nine\n   - under nine\n10. Ten\n    - under ten\n"));
      // A level the document skipped over is normalised to one step rather than three, because more
      // would put the marker four columns past its parent's content column: an indented code block.
      CHECK(Listed(NUMS, skip, "- a\n  - deep\n- b\n"));
      // Row 17: two lists that meet with the same marker merge into one and the second one's start
      // number is discarded, so a restart takes the HTML comment. It needs no blank line either side.
      CHECK(Listed(NUMS, split, "3. a\n4. b\n<!-- -->\n1. c\n"));
      // An interrupting paragraph ends the list in Markdown, and the true number is what makes the next
      // one continue where the first left off. That is CONVERSION_REFERENCE 5.4's own remedy.
      CHECK(Listed(NUMS, broken, "3. a\n\nbetween\n\n4. b\n"));
      // An empty item keeps its marker and loses the space after it, because a space before a newline
      // is Markdown's other spelling of a hard break.
      CHECK(Listed(NUMS, hollow, "- a\n-\n- c\n"));
      // At either *edge* of a list it is trimmed instead: that is the paragraph a user leaves behind on
      // pressing Enter to get out of a list, and a bare marker there is noise rather than content.
      CHECK(Listed(NUMS, edge, "- a\n"));
      // Mapping row 16: w:numFmt none is a continuation of the item above it -- indented, no marker,
      // and a blank line in front of it, because a paragraph cannot interrupt a paragraph.
      CHECK(Listed(NUMS, carry, "1. first\n\n   more\n2. second\n"));
      // The line-start pass still runs over the item's *content*, because the marker goes to the output
      // and never into the line buffer. Text that merely looks like a marker is escaped; ours never is.
      CHECK(Listed(NUMS, looks, "- \\- not a bullet\n- 1\\. not ordered\n"));
      // A continuation line after a hard break is indented to the item's content column rather than
      // left at column zero: laziness would carry it, but only for text and only until a rule changes.
      CHECK(Listed(NUMS, broke2, "3. one\\\n   two\n"));
      CHECK(ListedTo(NUMS, broke1, "- one  \n  two\n", HARD_BREAK_SPACES));
      // A list takes the ordinary one blank line on each side of it.
      CHECK(Listed(NUMS, around, "before\n\n- a\n\nafter\n"));
      // A numId no w:num declares is not numbered at all, and the paragraph keeps its text.
      CHECK(Listed(NUMS, ITEM("0", "77", "plain"), "plain\n"));
      // Whether an item joins a list a reader already has open is a question about the *open list at
      // its depth* and not about the block above it: a nested item sits between these two, so the
      // block above is deeper while the list this one joins is the one two markers back. Comparing
      // against the block above dropped the comment, and the restart then renumbered from the first
      // list's own start -- "1. c" reaching the reader as 4.
      CHECK(Listed(NUMS, buried, "3. a\n   1. b\n<!-- -->\n1. c\n"));
      // And the same shape where the counter did *not* restart takes no comment, which is what says the
      // rule above fires on the list changing rather than on a nested item having been written.
      CHECK(Listed(NUMS, cont, "3. a\n   1. b\n4. c\n"));
      // The same output through the other door: numId 3 spends its w:startOverride at the nested item,
      // so by the time an item of it reaches level 0 the counter there has been restarted and nothing
      // at that item says so. What makes it a new list is that its counter had to be seeded.
      CHECK(Listed(NUMS, spent, "3. a\n   1. b\n<!-- -->\n1. c\n"));
      // A marker-less continuation with nothing in it is nothing at all -- unlike an empty item, which
      // keeps the marker that stands for it. Emitted, its line landed on top of the blank line a
      // continuation already takes and the two read as one loose list with a hole in the middle.
      CHECK(Listed(NUMS, voided, "1. a\n2. b\n"));
      // The blank line a continuation needs is owed by the last block that actually put a line out, not
      // by the block before it in the array: a content-free continuation is skipped whole, so asking
      // the previous *block* reads one that was never emitted. Left that way the blank line went
      // missing and the continuation became a lazy line of the item above it -- two paragraphs of one
      // item merged into one, and the emitter's own blank-line discipline broken silently.
      CHECK(Listed(NUMS, hollow2, "1. one\n\n   more\n2. two\n"));
      // A content-free item at the head of a run is an artefact unless the item after it is deeper --
      // then it is the parent those items hang from. Trimmed away, its children were promoted to the
      // outer list and the item after them became their sibling, so a renderer counted it on from
      // their numbers: the document's "10." reached the page as one more than it says.
      CHECK(Listed(NUMS, orphan, "9.\n   - child\n10. later\n"));
   }
   {
      cchptr quoted = "<w:p><w:pPr><w:pStyle w:val=\"Q\"/><w:numPr><w:numId w:val=\"1\"/></w:numPr></w:pPr>"
                      "<w:r><w:t>a</w:t></w:r></w:p>"
                      "<w:p><w:pPr><w:pStyle w:val=\"Q\"/><w:numPr><w:numId w:val=\"1\"/></w:numPr></w:pPr>"
                      "<w:r><w:t>b</w:t></w:r></w:p>";
      cchptr merged = ITEM("0", "5", "head") CODE_LINE("1", "5", "one") CODE_LINE("1", "5", "two");
      cchptr apart  = CODE_LINE("0", "1", "one") CODE_LINE("0", "1", "two");
      cchptr fenced = "<w:p><w:pPr><w:pStyle w:val=\"SC\"/><w:numPr><w:numId w:val=\"1\"/></w:numPr></w:pPr>"
                      "<w:r><w:t>code</w:t></w:r></w:p>"
                      "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>outside</w:t></w:r></w:p>";

      // A quotation that is also an item takes its marker first and its "> " after it, and the bare ">"
      // that joins two quote paragraphs must not reach across the item boundary.
      CHECK(ConvertsWith(STYLE_QUOTE, NUMS, quoted, "- > a\n- > b\n", HARD_BREAK_BACKSLASH));
      // A code paragraph that is also an item is its own fence inside its own item, and never joins the
      // run of fences outside it -- which would emit it at column zero and end the list.
      CHECK(ConvertsWith(STYLE_CODE, NUMS, fenced, "- ```\n  code\n  ```\n\n```\noutside\n```\n", HARD_BREAK_BACKSLASH));
      // Row 12 merges consecutive all-monospace paragraphs into one fence, and two marker-less
      // continuations of one item are exactly that -- what says they belong to one item is that their
      // lines land in one column. Left apart they became two fences inside one item, which is a
      // document that never had them.
      CHECK(ConvertsWith(STYLE_CODE, NUMS, merged, "1. head\n\n   ```\n   one\n   two\n   ```\n", HARD_BREAK_BACKSLASH));
      // And two that each carry a marker of their own stay apart, because merging them would delete an
      // item: the run ends at the first block whose own marker a reader is meant to see.
      CHECK(ConvertsWith(STYLE_CODE, NUMS, apart, "- ```\n  one\n  ```\n- ```\n  two\n  ```\n", HARD_BREAK_BACKSLASH));
   }

   CheckGroup("MdEmitter: horizontal rules");
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "---\n"));
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:between w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "---\n"));
   // The blank lines row 25 asks for on either side are the block separator's own doing, and they are
   // what keeps the rule from being read as a setext underline for the paragraph above it.
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r></w:p>"
                  "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>"
                  "<w:p><w:r><w:t>b</w:t></w:r></w:p>",
                  "a\n\n---\n\nb\n"));
   // "Lone" is enforced at both ends: a box is not a rule, and neither is a bordered paragraph of text.
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:top w:val=\"single\"/><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", ""));
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"none\"/></w:pBdr></w:pPr></w:p>", ""));
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "a\n"));
   // A paragraph of nothing but whitespace still came to nothing, so its border still means a rule.
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr>"
                  "<w:r><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>",
                  "---\n"));

   CheckGroup("MdEmitter: links, anchors and the slugs they reach");
   // Mapping row 22's two halves. A bookmark that sits in a heading resolves to that heading's own GFM
   // slug, so the link needs no markup at all; one that sits anywhere else becomes an <a id> element.
   CHECK(Styled(STYLE_H1,
                "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:bookmarkStart w:id=\"1\" w:name=\"t\"/>"
                "<w:r><w:t>Getting Started</w:t></w:r></w:p>"
                "<w:p><w:hyperlink w:anchor=\"t\"><w:r><w:t>go</w:t></w:r></w:hyperlink></w:p>",
                "# Getting Started\n\n[go](#getting-started)\n"));
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>here</w:t></w:r></w:p>"
                  "<w:p><w:hyperlink w:anchor=\"m\"><w:r><w:t>go</w:t></w:r></w:hyperlink></w:p>",
                  "<a id=\"m\"></a>here\n\n[go](#m)\n"));
   // Two headings with one name are numbered the way the renderer numbers them, or the second link
   // would reach the first heading.
   CHECK(Styled(STYLE_H1,
                "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:t>Notes</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:bookmarkStart w:id=\"1\" w:name=\"t\"/>"
                "<w:r><w:t>Notes</w:t></w:r></w:p>"
                "<w:p><w:hyperlink w:anchor=\"t\"><w:r><w:t>go</w:t></w:r></w:hyperlink></w:p>",
                "# Notes\n\n# Notes\n\n[go](#notes-1)\n"));
   // An anchor nothing points at emits nothing, which is what keeps Word's generated bookmarks out of
   // the output; a paragraph that held nothing else goes with it.
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"_GoBack\"/><w:r><w:t>a</w:t></w:r></w:p>", "a\n"));
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"_GoBack\"/></w:p><w:p><w:r><w:t>a</w:t></w:r></w:p>", "a\n"));
   // A link naming a bookmark the document does not define keeps its text and loses its brackets, the
   // same degradation a dangling relationship gets.
   CHECK(Converts("<w:p><w:hyperlink w:anchor=\"gone\"><w:r><w:t>go</w:t></w:r></w:hyperlink></w:p>", "go\n"));
   // CONVERSION_REFERENCE 5.6: a hyperlink with no text is skipped rather than emitted empty.
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r><w:hyperlink w:anchor=\"m\"><w:r><w:t></w:t></w:r></w:hyperlink>"
                  "<w:r><w:t>b</w:t></w:r></w:p><w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>c</w:t></w:r></w:p>",
                  "ab\n\nc\n"));
   // Formatting goes inside the brackets, and the delimiter beside a bracket still has to parse: the
   // opening "**" stands after a '[', which is punctuation, and the closing one before a ']'.
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                  "<w:p><w:hyperlink w:anchor=\"m\"><w:r><w:rPr><w:b/></w:rPr><w:t>bold</w:t></w:r></w:hyperlink></w:p>",
                  "<a id=\"m\"></a>t\n\n[**bold**](#m)\n"));
   // A link may run over a hard break, and Markdown has no way to spell one that does -- so it is
   // closed at the end of the line and opened again on the next.
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                  "<w:p><w:hyperlink w:anchor=\"m\"><w:r><w:t>one</w:t><w:br/><w:t>two</w:t></w:r></w:hyperlink></w:p>",
                  "<a id=\"m\"></a>t\n\n[one](#m)\\\n[two](#m)\n"));
   // A heading is one line by construction, so a break inside a link inside one is a space.
   CHECK(Styled(STYLE_H1,
                "<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr>"
                "<w:hyperlink w:anchor=\"m\"><w:r><w:t>one</w:t><w:br/><w:t>two</w:t></w:r></w:hyperlink></w:p>",
                "<a id=\"m\"></a>t\n\n# [one two](#m)\n"));
   // What stands after a span, for the flanking test, is the markup and not the text beyond it: a '['
   // is punctuation, so a closing "**" behind punctuation and in front of one still parses. Reading
   // past the bracket to the word after it would take the HTML fallback for no reason.
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                  "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>(a)</w:t></w:r>"
                  "<w:hyperlink w:anchor=\"m\"><w:r><w:t>go</w:t></w:r></w:hyperlink></w:p>",
                  "<a id=\"m\"></a>t\n\n**(a)**[go](#m)\n"));
   // CONVERSION_REFERENCE 4.2's pitfall 7. An exclamation mark in front of a link's '[' makes the pair
   // an image marker, so the sentence loses its link and gains a broken picture. MdEscape leaves the
   // mark alone by design -- it is only dangerous next to a bracket the emitter itself writes.
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                  "<w:p><w:r><w:t>wow!</w:t></w:r><w:hyperlink w:anchor=\"m\"><w:r><w:t>go</w:t></w:r></w:hyperlink></w:p>",
                  "<a id=\"m\"></a>t\n\nwow\\![go](#m)\n"));
   // A hard break at the very edge of a link leaves one of its two halves with nothing between the
   // brackets. "[](#m)" is a link a reader can neither see nor click, so the bracket is unwound.
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                  "<w:p><w:r><w:t>a</w:t></w:r><w:hyperlink w:anchor=\"m\">"
                  "<w:r><w:br/><w:t>two</w:t></w:r></w:hyperlink></w:p>",
                  "<a id=\"m\"></a>t\n\na\\\n[two](#m)\n"));
   CHECK(Converts("<w:p><w:bookmarkStart w:id=\"1\" w:name=\"m\"/><w:r><w:t>t</w:t></w:r></w:p>"
                  "<w:p><w:hyperlink w:anchor=\"m\"><w:r><w:t>one</w:t><w:br/></w:r></w:hyperlink>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "<a id=\"m\"></a>t\n\n[one](#m)\\\nb\n"));
   // Nothing but text reaches a fence: a link inside one has no brackets to write, because the content
   // of a code block is literal.
   CHECK(Styled(STYLE_CODE,
                "<w:p>" IN_CODE "<w:bookmarkStart w:id=\"1\" w:name=\"m\"/>"
                "<w:hyperlink w:anchor=\"m\"><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>",
                "```\nx\n```\n"));

   CheckGroup("MdEmitter: a GFM pipe table");
   // The delimiter row is what makes the lines around it a table at all, and GFM reads one only where
   // it holds exactly as many cells as the header -- so it is written from the table's own width.
   CHECK(Converts("<w:tbl>" GRID(COL COL) ROW2("a", "b") ROW2("c", "d") "</w:tbl>", "| a | b |\n| --- | --- |\n| c | d |\n"));
   // A one-row table is a legal GFM table with a header and no body.
   CHECK(Converts("<w:tbl>" GRID(COL) ROW1("a") "</w:tbl>", "| a |\n| --- |\n"));
   // A row holding fewer cells than the grid is padded to it, which is what keeps the delimiter row's
   // width true for every row and the table a table.
   CHECK(Converts("<w:tbl>" GRID(COL COL COL) ROW2("a", "b") "</w:tbl>", "| a | b |  |\n| --- | --- | --- |\n"));
   // Alignment comes from the first row's own w:jc, spread over the columns each cell covers.
   CHECK(Converts("<w:tbl>" GRID(COL COL) "<w:tr><w:tc><w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p></w:tc>"
                                          "<w:tc><w:p><w:pPr><w:jc w:val=\"right\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "| a | b |\n| :---: | ---: |\n"));
   // A pipe ends a cell wherever it stands, so it is escaped in a cell's text and inside a code span
   // alike -- the second is the one escape GFM honours inside one.
   CHECK(Converts("<w:tbl>" GRID(COL) ROW1("a | b") "</w:tbl>", "| a \\| b |\n| --- |\n"));
   CHECK(Styled(STYLE_SPAN, T1 CELLS("<w:p><w:r><w:rPr>" CODE_STYLE "</w:rPr><w:t>a | b</w:t></w:r></w:p>"), "| `a \\| b` |\n| --- |\n"));
   // A pipe table's cell is inline content, so a hard break and a paragraph boundary are both "<br>"
   // and nothing a cell holds can be a block.
   CHECK(Converts(T1 CELLS("<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>"), "| a<br>b |\n| --- |\n"));
   CHECK(Converts(T1 CELLS("<w:p><w:r><w:t>a</w:t></w:r></w:p><w:p><w:r><w:t>b</w:t></w:r></w:p>"), "| a<br>b |\n| --- |\n"));
   // A break with nothing after it is dropped in a cell exactly as it is in a paragraph: a cell is one
   // line by construction, so a trailing "<br>" is a line ending on a line that has no next one. The
   // padding that hid it goes first, and the two are stripped in a loop because either may follow the
   // other -- line 335 above is the same rule outside a table.
   CHECK(Converts(T1 CELLS("<w:p><w:r><w:t>a</w:t><w:br/></w:r></w:p>"), "| a |\n| --- |\n"));
   CHECK(Converts(T1 CELLS("<w:p><w:r><w:t>a</w:t><w:br/><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>"), "| a |\n| --- |\n"));
   // The raw-HTML form holds a cell in the output buffer rather than in the line buffer, so it trims
   // the same two things from a different place. Left alone it emitted "<th>a<br>   </th>", a blank
   // line inside the cell that the pipe form of the same document does not have.
   {
      cchptr trail = T1 "<w:tr><w:tc><w:p><w:r><w:t>a</w:t><w:br/>"
                        "<w:t xml:space=\"preserve\">   </w:t></w:r></w:p></w:tc></w:tr>"
                        "<w:tr><w:tc>" VRESTART "<w:p/></w:tc></w:tr></w:tbl>";

      CHECK(Merged(trail, "<table>\n<tr><th>a</th></tr>\n<tr><td></td></tr>\n</table>\n"));
   }
   // A cell whose only paragraph is empty is an empty cell and not a "<br>" a reader sees.
   CHECK(Converts("<w:tbl>" GRID(COL COL) "<w:tr>" CELL("a") "<w:tc><w:p/></w:tc></w:tr></w:tbl>", "| a |  |\n| --- | --- |\n"));
   // A blank line stands between a table and whatever is on either side of it, or the paragraph after
   // it would be read as one more row and the table before it would merge into this one.
   CHECK(Converts("<w:p><w:r><w:t>before</w:t></w:r></w:p><w:tbl>" GRID(COL) ROW1("a") "</w:tbl>"
                                                                                       "<w:p><w:r><w:t>after</w:t></w:r></w:p>",
                  "before\n\n| a |\n| --- |\n\nafter\n"));
   CHECK(Converts("<w:tbl>" GRID(COL) ROW1("a") "</w:tbl><w:tbl>" GRID(COL) ROW1("b") "</w:tbl>", "| a |\n| --- |\n\n| b |\n| --- |\n"));

   CheckGroup("MdEmitter: what a cell does with a block it cannot carry");
   // A list item keeps its marker as literal text, because losing "1." from a cell loses the count.
   CHECK(Listed(NUMS,
                "<w:tbl>" GRID(COL) "<w:tr><w:tc>"
                                    "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"2\"/></w:numPr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                                    "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"2\"/></w:numPr></w:pPr><w:r><w:t>b</w:t></w:r></w:p>"
                                    "</w:tc></w:tr></w:tbl>",
                "| 3. a<br>4. b |\n| --- |\n"));
   // A code paragraph has no fence to become in a cell, so it becomes the inline form of one.
   CHECK(Styled(STYLE_CODE, T1 CELLS("<w:p>" IN_CODE "<w:r><w:t>a();</w:t></w:r></w:p>"), "| `a();` |\n| --- |\n"));
   // A quotation and a heading keep only what they say: a "> " or a "#" in a cell is literal text.
   CHECK(Styled(STYLE_QUOTE, T1 CELLS("<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"), "| a |\n| --- |\n"));

   // D12's scope is the assembled line, and a cell is one line of its row however many paragraphs it
   // holds -- so the count is taken over the whole cell. Counted per block instead, these two hold one
   // dollar each, neither is escaped, and GitHub reads "$5<br>and $10" as math: exactly the corruption
   // D12 was ruled to fix, arriving through a door M9 opened.
   CHECK(Converts(T2 "<w:tr><w:tc><w:p><w:r><w:t>costs $5</w:t></w:r></w:p>"
                     "<w:p><w:r><w:t>and $10</w:t></w:r></w:p></w:tc>" CELL("b") "</w:tr></w:tbl>",
                  "| costs \\$5<br>and \\$10 | b |\n| --- | --- |\n"));
   // A hard break with nothing after it is dropped inside a cell as it is everywhere else: a trailing
   // "<br>" is a line ending in a cell that has no next line.
   CHECK(Converts(T1 CELLS("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:br/></w:r></w:p>"), "| a |\n| --- |\n"));

   CheckGroup("MdEmitter: a merge padded into the grid");
   // CONVERSION_REFERENCE row 19's policy A: the content lands in the first column the cell covers and
   // every other column it covers is an empty pad, so no row is ever narrower than the delimiter row.
   CHECK(Converts("<w:tbl>" GRID(COL COL) ROW2("a", "b") "<w:tr><w:tc>" WIDE2 "<w:p><w:r><w:t>wide</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "| a | b |\n| --- | --- |\n| wide |  |\n"));
   // A vertical merge's continuation is an empty cell, because that is what Word draws.
   CHECK(Converts("<w:tbl>" GRID(COL) "<w:tr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc></w:tr>"
                                      "<w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p><w:r><w:t>ignored</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "| a |\n| --- |\n|  |\n"));

   // A block dropped *before* a table moves every block after it down, and a cell names its blocks by
   // index -- so IrDropEmptyBlocks shifting a table's records by the delta that applies where it stands
   // is what keeps a cell pointing at its own content. Word writes a _GoBack nothing points at into
   // every document it saves, so the paragraph here is the ordinary case rather than a contrived one;
   // without the shift this table reads its neighbours' blocks and the whole document slides.
   {
      cchptr shifted = GOBACK T2 ABCD "<w:p><w:r><w:t>after</w:t></w:r></w:p>";

      CHECK(Converts(shifted, "| a | b |\n| --- | --- |\n| c | d |\n\nafter\n"));
   }

   CheckGroup("MdEmitter: a row that starts part-way across the grid");
   // w:gridBefore, which Word writes for an indented row and for a row whose leading cells were deleted:
   // the row's cells start where it says, and the columns before them are empty cells rather than the
   // row sliding left. Both table forms fill the gap the same way.
   {
      cchptr indented = ABC3 ROWP(GRID_BEFORE("1"), CELL("e") CELL("f")) ROWP(GRID_BEFORE("2") GRID_AFTER("0"), CELL("i")) "</w:tbl>";

      CHECK(Converts(indented, "| a | b | c |\n| --- | --- | --- |\n|  | e | f |\n|  |  | i |\n"));
      CHECK(Merged(ABC3 ROWP(GRID_BEFORE("1"), "<w:tc>" WIDE2 "<w:p><w:r><w:t>ef</w:t></w:r></w:p></w:tc>") "</w:tbl>",
                   "<table>\n<tr><th>a</th><th>b</th><th>c</th></tr>\n<tr><td></td><td colspan=\"2\">ef</td></tr>\n</table>\n"));
   }
   // A vertical merge is matched by column, so a row shifted by w:gridBefore continues the merge in the
   // column the document meant rather than in the one its cell count would put it in.
   CHECK(Merged(T2 "<w:tr>" CELL("a") RESTART_CELL("b") "</w:tr>" ROWP(GRID_BEFORE("1"), "<w:tc>" VMERGED "<w:p/></w:tc>") "</w:tbl>",
                "<table>\n<tr><th>a</th><th rowspan=\"2\">b</th></tr>\n<tr><td></td></tr>\n</table>\n"));
   // A cell starting inside the grid and spanning past IR_MAX_COLUMNS spans only what is left of it, in the
   // raw-HTML form as in the pipe one: written with its whole w:gridSpan its row came out wider than
   // every other. The grid oracle found it at the cap's edge, which no fixture reaches.
   {
      static char edge[4096];
      static char wanted[4096];
      ui64        used = 0;
      ui64        at   = 0;

      EmitPut(edge, &used, "<w:tbl><w:tr>");
      EmitPut(wanted, &at, "<table>\n<tr>");
      for(ui32 column = 0; column + 1u < IR_MAX_COLUMNS; ++column) {
         EmitPut(edge, &used, "<w:tc/>");
         EmitPut(wanted, &at, "<th></th>");
      }
      EmitPut(edge, &used, "<w:tc><w:tcPr><w:gridSpan w:val=\"3\"/></w:tcPr><w:p><w:r><w:t>x</w:t></w:r></w:p></w:tc></w:tr></w:tbl>");
      EmitPut(wanted, &at, "<th>x</th></tr>\n</table>\n");
      CHECK(Merged(edge, wanted));
   }
   // w:gridAfter reaches the table's width when nothing else declares the columns it leaves empty.
   CHECK(Converts("<w:tbl>" ROWP(GRID_AFTER("2"), CELL("a")) "</w:tbl>", "| a |  |  |\n| --- | --- | --- |\n"));

   CheckGroup("MdEmitter: the raw-HTML fallback");
   // A nested table has no pipe form at all, so it takes the fallback whatever --tables says.
   CHECK(Converts("<w:tbl>" GRID(COL) "<w:tr><w:tc><w:tbl>" GRID(COL) ROW1("in") "</w:tbl><w:p/></w:tc></w:tr></w:tbl>",
                  "<table>\n<tr><th><table>\n<tr><th>in</th></tr>\n</table>\n</th></tr>\n</table>\n"));
   // A merge takes it only under html-on-merge, and then it keeps the merge rather than padding it.
   CHECK(Merged("<w:tbl>" GRID(COL COL) ROW2("a", "b") "<w:tr><w:tc>" WIDE2 "<w:p><w:r><w:t>wide</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                "<table>\n<tr><th>a</th><th>b</th></tr>\n<tr><td colspan=\"2\">wide</td></tr>\n</table>\n"));
   CHECK(Merged("<w:tbl>" GRID(COL COL) "<w:tr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc>" CELL(
                    "b") "</w:tr>"
                         "<w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc>" CELL("c") "</w:tr></w:tbl>",
                "<table>\n<tr><th rowspan=\"2\">a</th><th>b</th></tr>\n<tr><td>c</td></tr>\n</table>\n"));
   // Inside a raw-HTML block no Markdown is parsed, so every delimiter is an element and every escape
   // an entity -- and a pipe is inert there, because no row is being split.
   CHECK(Merged(T2 CELLS(WIDE2 "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a &amp; b</w:t></w:r><w:r><w:t> | c</w:t></w:r></w:p>"),
                "<table>\n<tr><th colspan=\"2\"><strong>a &amp; b</strong> | c</th></tr>\n</table>\n"));
   // A w:vMerge continuation whose merge nothing above it still covers -- a row spanning across the
   // column the merge was opened in breaks the chain, and producers write one -- is an ordinary empty
   // cell and not nothing at all. Dropped, it would leave the row a column short, which is the
   // silently narrower table row 19 forbids. Found by the grid oracle, which no fixture would have.
   {
      cchptr broken = T2 "<w:tr><w:tc>" VRESTART "<w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc>"
                         "<w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr>"
                         "<w:tr><w:tc>" WIDE2 "<w:p><w:r><w:t>wide</w:t></w:r></w:p></w:tc></w:tr>"
                         "<w:tr><w:tc>" VMERGED "<w:p/></w:tc>"
                         "<w:tc><w:p><w:r><w:t>c</w:t></w:r></w:p></w:tc></w:tr></w:tbl>";

      CHECK(Merged(broken, "<table>\n<tr><th>a</th><th>b</th></tr>\n<tr><td colspan=\"2\">wide</td></tr>\n"
                           "<tr><td></td><td>c</td></tr>\n</table>\n"));
   }
   // A vertical merge whose restart is WIDER than the row continuing it. A rowspan can only promise a
   // rectangle, so it is written only where every column the restart covers is continued -- counted at
   // the restart's first column alone, this claimed both columns of the last row, the browser's own
   // grid algorithm then put "beside" in a third column, and the raw-HTML form rendered one column
   // wider than the pipe form of the same document. Found by the grid oracle once its generator was
   // widened to put a w:vMerge restart on a cell spanning more than one column.
   {
      cchptr ragged = T2 "<w:tr><w:tc>" WIDE2 "<w:p><w:r><w:t>head</w:t></w:r></w:p></w:tc></w:tr>"
                         "<w:tr><w:tc>" WIDE2V "<w:p><w:r><w:t>merged</w:t></w:r></w:p></w:tc></w:tr>"
                         "<w:tr><w:tc>" VMERGED "<w:p/></w:tc>" CELL("beside") "</w:tr></w:tbl>";

      CHECK(Merged(ragged, "<table>\n<tr><th colspan=\"2\">head</th></tr>\n"
                           "<tr><td colspan=\"2\">merged</td></tr>\n"
                           "<tr><td></td><td>beside</td></tr>\n</table>\n"));
      // The same table with the continuation covering the whole span keeps its rowspan, which is what
      // makes the case above a rule about raggedness rather than about merges that span columns.
      cchptr square = T2 "<w:tr><w:tc>" WIDE2 "<w:p><w:r><w:t>head</w:t></w:r></w:p></w:tc></w:tr>"
                         "<w:tr><w:tc>" WIDE2V "<w:p><w:r><w:t>merged</w:t></w:r></w:p></w:tc></w:tr>"
                         "<w:tr><w:tc><w:tcPr><w:gridSpan w:val=\"2\"/><w:vMerge/></w:tcPr><w:p/></w:tc></w:tr></w:tbl>";

      CHECK(Merged(square, "<table>\n<tr><th colspan=\"2\">head</th></tr>\n"
                           "<tr><td colspan=\"2\" rowspan=\"2\">merged</td></tr>\n<tr></tr>\n</table>\n"));
   }

   CheckGroup("MdEmitter: a note reference and its definition");
   CHECK(Noted(PARA_OF(RUN("a") FREF("2") RUN("b")), FOOT("2", NOTE_P("Note.")), nullptr, "a[^1]b\n\n[^1]: Note.\n"));
   // A parenthesis after a reference would make the pair a link and lose the reference; a colon after
   // one that opens a line would make the line a definition of its own. Mid-line a colon is harmless.
   CHECK(Noted(PARA_OF(RUN("a") FREF("2") RUN("(see)")), FOOT("2", NOTE_P("N.")), nullptr, "a[^1]\\(see)\n\n[^1]: N.\n"));
   CHECK(Noted(PARA_OF(FREF("2") RUN(": b")), FOOT("2", NOTE_P("N.")), nullptr, "[^1]\\: b\n\n[^1]: N.\n"));
   CHECK(Noted(PARA_OF(RUN("a") FREF("2") RUN(": b")), FOOT("2", NOTE_P("N.")), nullptr, "a[^1]: b\n\n[^1]: N.\n"));
   CHECK(Noted(PARA_OF(RUN("a") BR FREF("2") RUN(": b")), FOOT("2", NOTE_P("N.")), nullptr, "a\\\n[^1]\\: b\n\n[^1]: N.\n"));
   {
      // One sequence for both stories, in the order the references are read, and the definitions in the
      // order of their labels; a reference to a note the document does not hold emits nothing at all.
      cchptr body = PARA_OF(EREF("7") RUN(" x ") FREF("2") RUN(" y") FREF("9") RUN("z"));

      CHECK(Noted(body, FOOT("2", PARA("foot")), ENDN("7", PARA("end")), "[^1] x [^2] yz\n\n[^1]: end\n\n[^2]: foot\n"));
   }
   // A note that came to nothing is an empty definition, which is still a definition to GFM; leaving
   // it out would turn the reference into the literal text "[^1]".
   CHECK(Cited(PARA_OF("<w:r><w:footnoteRef/></w:r>"), "[^1]\n\n[^1]:\n"));

   CheckGroup("MdEmitter: a note holds whatever the body can");
   // Every line of a definition after its first is indented four columns, which is what keeps a second
   // paragraph, a list, a fence, a table, a quotation and a heading inside it.
   CHECK(Cited(NOTE_P("One.") PARA("Two."), "[^1]\n\n[^1]: One.\n\n    Two.\n"));
   CHECK(Cited(PARA_OF(RUN("one") BR RUN("two")), "[^1]\n\n[^1]: one\\\n    two\n"));
   CHECK(Cited(NOTE_P("Intro.") ITEM("0", "1", "one") ITEM("0", "1", "two"), "[^1]\n\n[^1]: Intro.\n\n    - one\n    - two\n"));
   CHECK(Cited(ITEM("0", "1", "one") ITEM("0", "1", "two"), "[^1]\n\n[^1]: - one\n    - two\n"));
   CHECK(Cited(ITEM("0", "4", "nine") ITEM("1", "4", "deep"), "[^1]\n\n[^1]: 9. nine\n       - deep\n"));
   CHECK(Cited(NOTE_P("Text.") PARA_OF(IN_CODE RUN("code")), "[^1]\n\n[^1]: Text.\n\n    ```\n    code\n    ```\n"));
   CHECK(Cited("<w:tbl>" ROW2("a", "b") "</w:tbl>", "[^1]\n\n[^1]: | a | b |\n    | --- | --- |\n"));
   CHECK(Cited(QUOTED("q") QUOTED("r"), "[^1]\n\n[^1]: > q\n    >\n    > r\n"));
   CHECK(Cited(PARA_OF(IN_H1 RUN("H")) PARA("text"), "[^1]\n\n[^1]: # H\n\n    text\n"));
   CHECK(Cited(PARA("x") RULE_P, "[^1]\n\n[^1]: x\n\n    ---\n"));
   {
      // A raw-HTML table keeps every line inside the definition, the line a nested table's close leaves
      // the rest of its cell on included: at column zero that line would end the definition, and the
      // rest of the note would land in the body.
      cchptr after = "<w:tbl>" CELLS(PARA("a") "<w:tbl>" ROW1("in") "</w:tbl>" PARA("b"));
      cchptr last  = "<w:tbl>" CELLS(PARA("a") "<w:tbl>" ROW1("in") "</w:tbl><w:p/>");
      cchptr wantA = "[^1]\n\n[^1]: <table>\n    <tr><th>a<table>\n    <tr><th>in</th></tr>\n    </table>\n"
                     "    b</th></tr>\n    </table>\n";
      cchptr wantL = "[^1]\n\n[^1]: <table>\n    <tr><th>a<table>\n    <tr><th>in</th></tr>\n    </table>\n"
                     "    </th></tr>\n    </table>\n";

      CHECK(Cited(after, wantA));
      CHECK(Cited(last, wantL));
   }
   // The blank line before the definitions is a plain one even where both sides are quotations: a bare
   // ">" there would carry the body's blockquote into the note.
   CHECK(Noted(QUOTED("q") PARA_OF(IN_QUOTE FREF("2")), FOOT("2", QUOTED("r")), nullptr, "> q\n>\n> [^1]\n\n[^1]: > r\n"));

   CheckGroup("MdEmitter: where a note reference stands");
   {
      // A heading's id is built from its rendered text, and a reference renders as its number, so a link
      // to a bookmark in the heading has to reach "#h1" and not "#h".
      cchptr body = PARA_OF(IN_H1 "<w:bookmarkStart w:id=\"1\" w:name=\"bm\"/>" RUN("H") FREF("2")) TO_BM;

      CHECK(Noted(body, FOOT("2", PARA("N.")), nullptr, "# H[^1]\n\n[go](#h1)\n\n[^1]: N.\n"));
   }
   {
      cchptr cell = "<w:tbl>" CELLS(PARA_OF(RUN("a") FREF("2") RUN("(x)")));

      CHECK(Noted(cell, FOOT("2", PARA("N.")), nullptr, "| a[^1]\\(x) |\n| --- |\n\n[^1]: N.\n"));
   }
   {
      // Inside a raw-HTML table GFM parses no Markdown, so the reference is the number a reader saw.
      cchptr nested = "<w:tbl>" CELLS(PARA_OF(RUN("a") FREF("2")) "<w:tbl>" ROW1("in") "</w:tbl><w:p/>");
      cchptr wanted = "<table>\n<tr><th>a<sup>1</sup><table>\n<tr><th>in</th></tr>\n</table>\n</th></tr>\n</table>\n\n[^1]: N.\n";

      CHECK(Noted(nested, FOOT("2", PARA("N.")), nullptr, wanted));
   }
   // A fence writes its text and nothing else, so a reference inside one is not written -- and the note
   // is still defined, because its number was given when the reference was read.
   CHECK(Noted(PARA_OF(IN_CODE RUN("code") FREF("2")), FOOT("2", PARA("N.")), nullptr, "```\ncode\n```\n\n[^1]: N.\n"));

   CheckGroup("MdEmitter: fields and revisions reach the page as what they showed");
   CHECK(Converts(PARA_OF(RUN("see ") FLD(" HYPERLINK \"http://x/\" ") RUN("here") FLD_END RUN(".")), "see [here](http://x/).\n"));
   CHECK(Converts(PARA_OF(FLD(" HYPERLINK \"u\" ") RUN("a")) PARA_OF(RUN("b") FLD_END RUN(" y")), "[a](u)\n\n[b](u) y\n"));
   CHECK(Converts(PARA("before") PARA_OF(FLD(" TOC \\o ") RUN("entry")) PARA_OF(RUN("entry") FLD_END) PARA("after"), "before\n\nafter\n"));
   CHECK(Converts(PARA_OF("<w:pPr><w:rPr><w:del w:id=\"1\"/></w:rPr></w:pPr>" RUN("one ")) PARA("two"), "one two\n"));
}
