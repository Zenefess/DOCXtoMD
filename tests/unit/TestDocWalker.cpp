/*
 * File: TestDocWalker.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-22
 * Description: Unit tests for the body walk: wrappers, run content, and the formatting bits on a span.
 * To Do: 1) Drive the field state machine's traces once M10 replaces today's skip-it-whole handling.
 *        2) Drive a table whose cells hold pictures and links, which needs a package to resolve them.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, StyleModel.h, typedefs.h
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
#include "StyleModel.h"

//-- Helpers

// The three style parts the cases below reach for, named rather than repeated: a quote style, a
// code paragraph style and a code character style, each the shortest form that carries its role.
static constexpr cchptr STYLE_QUOTE = "<w:style w:type=\"paragraph\" w:styleId=\"Q\"><w:name w:val=\"Quote\"/></w:style>";
static constexpr cchptr STYLE_CODE  = "<w:style w:type=\"paragraph\" w:styleId=\"SC\"><w:name w:val=\"Source Code\"/></w:style>";
static constexpr cchptr STYLE_SPAN  = "<w:style w:type=\"character\" w:styleId=\"CC\"><w:name w:val=\"Code\"/></w:style>";

// A paragraph style carrying numbering, and a heading style carrying it, for the cases that have to
// show where a w:numPr may come from and what cancels it. Spelled in halves for the column limit.
static constexpr cchptr STYLE_LIST_BASE = "<w:style w:type=\"paragraph\" w:styleId=\"LN\">"; // The opening tag
static constexpr cchptr STYLE_LIST_BODY = "<w:pPr><w:numPr><w:numId w:val=\"4\"/><w:ilvl w:val=\"1\"/></w:numPr></w:pPr></w:style>";
static constexpr cchptr STYLE_NUMH_BASE = "<w:style w:type=\"paragraph\" w:styleId=\"NH\"><w:name w:val=\"heading 1\"/>";
static constexpr cchptr STYLE_NUMH_BODY = "<w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr></w:style>";

// A heading style based on the quote style, for the case that has to show which of the two wins. Named
// in halves so no line reaches the column limit once the formatter has joined what it can.
static constexpr cchptr STYLE_QH_BASE = "<w:style w:type=\"paragraph\" w:styleId=\"QH\">"; // The opening tag
static constexpr cchptr STYLE_QH_BODY = "<w:name w:val=\"heading 3\"/><w:basedOn w:val=\"Q\"/></w:style>";

// The root element every body below is wrapped in, with the two namespaces the cases need bound on it.
static constexpr cchptr WALK_HEAD = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
                                    " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
                                    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
                                    " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
                                    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\""
                                    " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\""
                                    " xmlns:v=\"urn:schemas-microsoft-com:vml\""
                                    " xmlns:o=\"urn:schemas-microsoft-com:office:office\"><w:body>";

// The shortest DrawingML picture that carries an alt text and a relationship, and its VML twin.
// Spelled in halves so no line reaches the column limit once the formatter has joined what it can.
#define DRAWING_OPEN "<w:r><w:drawing><wp:inline><wp:docPr id=\"1\" "
#define DRAWING_BLIP "/><a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip "
#define DRAWING_SHUT "/></pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>"
static constexpr cchptr WALK_TAIL = "</w:body></w:document>";

// The numbering part every case here resolves against. The walk consults it for one bit only -- whether
// a w:numId names a list this document can resolve -- because a reference that resolves to nothing must
// not cancel the horizontal rule of row 25 or the monospace detection of row 12, both of which a real
// list item does cancel. numId 4 and numId 9 resolve; every other identifier is dangling.
#define WALK_NUM(id) "<w:num w:numId=\"" id "\"><w:abstractNumId w:val=\"0\"/></w:num>"

// The pieces M9's table cases are built from. A cell holding one word is what nearly every one
// of them holds, so naming it leaves each case showing only the property it is about.
#define WALK_PARA_A       "<w:p><w:r><w:t>a</w:t></w:r></w:p>"
#define WALK_CELL_A       "<w:tc>" WALK_PARA_A "</w:tc>"
#define WALK_ROW_A        "<w:tr>" WALK_CELL_A "</w:tr>"
#define WALK_JC(v)        "<w:tc><w:p><w:pPr><w:jc w:val=\"" v "\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p></w:tc>"
#define WALK_ROW(t)       "<w:tr><w:tc><w:p><w:r><w:t>" t "</w:t></w:r></w:p></w:tc></w:tr>"
#define WALK_ROW_IN       WALK_ROW("in")
#define WALK_ROW_CHOICE   WALK_ROW("choice")
#define WALK_ROW_FALLBACK WALK_ROW("fallback")
#define WALK_SPAN_2       "<w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr>"
#define WALK_GRID_1       "<w:tblGrid><w:gridCol/></w:tblGrid>"
#define WALK_GRID_2       "<w:tblGrid><w:gridCol/><w:gridCol/></w:tblGrid>"
#define WALK_CELL_F1      "<w:tc><w:p><w:r><w:t>F1</w:t></w:r></w:p></w:tc>"
#define WALK_CELL_F2      "<w:tc><w:p><w:r><w:t>F2</w:t></w:r></w:p></w:tc>"
#define WALK_ROW_FF       "<w:tr>" WALK_CELL_F1 WALK_CELL_F2 "</w:tr>"

static constexpr cchptr WALK_NUMS = "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                                    "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\">"
                                    "<w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>" WALK_NUM("4") WALK_NUM("9") "</w:numbering>";

// Bytes before the terminator.
static cui64 WalkLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Appends a NUL-terminated literal to a buffer.
static void WalkAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr text) {
   cui64 length = WalkLength(text);

   for(ui64 index = 0; index < length && *used + 1u < destBytes; ++index) dest[(*used)++] = text[index];
   dest[*used] = 0;
}

// Writes one unsigned number into a buffer and reports how many bytes it took. Check.h deliberately
// pulls in no I/O -- it needs only typedefs.h -- so a trace cannot reach for snprintf.
static cui64 WalkNumber(chptrc dest, ui32 value) {
   char digits[12];
   ui64 count = 0;
   ui64 used  = 0;

   do {
      digits[count++] = char('0' + (value % 10u));
      value /= 10u;
   } while(value && count < sizeof(digits));
   while(count) dest[used++] = digits[--count];
   return used;
}

// Writes a block's list membership, in whichever of its two forms the block is in. Before
// NumAssignMarkers a block carries the *reference* the walk read and renders as [<level>#<numId>];
// afterwards it carries the marker and renders as [<level>=<marker>] -- "-" for a bullet, the number
// for an ordered item, nothing at all for a marker-less continuation -- with a trailing "!" for an item
// that opens a list the one before it was not part of.
static cui64 WalkList(cIR_BLOCKptr block, chptrc dest) {
   ui64 used = 0;

   if(block->listNumId < 0 && !(block->listFlags & IR_LIST_ITEM)) return 0;
   dest[used++] = '[';
   dest[used++] = char('0' + block->listLevel);
   if(block->listFlags & IR_LIST_ITEM) {
      dest[used++] = '=';
      if(block->listFlags & IR_LIST_PLAIN) used += 0; // A continuation carries no marker to render
      else if(!(block->listFlags & IR_LIST_ORDERED)) dest[used++] = '-';
      else used += WalkNumber(dest + used, block->listNumber);
      if(block->listFlags & IR_LIST_FIRST) dest[used++] = '!';
   } else {
      dest[used++] = '#';
      used += WalkNumber(dest + used, ui32(block->listNumId));
   }
   dest[used++] = ']';
   return used;
}

// A block kind neither this renderer nor its twin in the other suite spells would come out as an
// ordinary paragraph, which is a plausible trace rather than an obviously wrong one -- exactly the
// failure CLAUDE.md warns of in saying the two are independent copies that must both be edited.
static_assert(ui32(IR_BLOCK_KIND_COUNT) == 6u, "TestDocWalker: the trace renderer must spell every block kind; add the new one here.");

// The trace notation the three renderers below share, so a case is one string comparison rather than
// ten assertions. A heading is H<level>{...} and a paragraph is P{...}; inside
// a block, [text] is a text span, | is a hard break, and the letters before a bracket are its
// formatting: b bold, i italic, s strike, ^ superscript, v subscript and c code. The other block
// letters are Q for a blockquote, C for a line of a fenced block and R for a horizontal rule.
// M7's three span kinds are L(dest) for a link start and L) for its end, I(source)[alt] for an
// image, and N(name) for a bookmark anchor; a muted anchor -- one nothing links to -- is N-(name).
// M8's list membership stands in front of the block letter: [<level>#<numId>] is the reference the
// walk read, and [<level>=<marker>] is what NumAssignMarkers made of it.

// Renders one range of blocks, which is the whole document at the top level and one cell's content
// inside a table. A table's own blocks are the blocks of its cells, so the range renderer and the
// table renderer call each other -- and the range renderer skips past a table's whole block range,
// or every cell's content would be rendered twice: once in the table and once at the top level.
static void WalkRange(cIR_DOCUMENTptr document, cui32 from, cui32 to, chptrc dest, cui64 destBytes, ui64ptrc used);

// Renders one table: T, its column count, one character per column of alignment (- l c r), then m for
// a table holding a merge and n for one holding a nested table. Rows are separated by "/" and a row
// that carried w:tblHeader is prefixed with "=". A cell is its blocks between parentheses, prefixed by
// its span when it covers more than one column, by "v" when it starts a vertical merge and by "^" when
// it continues one.
static void WalkTable(cIR_DOCUMENTptr document, cIR_BLOCKptr block, chptrc dest, cui64 destBytes, ui64ptrc used) {
   cIR_TABLEptr table = IrTableAt(document, block->tableAt);
   char         head[IR_MAX_COLUMNS + 16u];
   ui64         at = 0;

   if(!table) {
      WalkAppend(dest, destBytes, used, "T?");
      return;
   }
   head[at++] = 'T';
   at += WalkNumber(head + at, table->columns);
   for(ui32 column = 0; column < table->columns; ++column) {
      cIR_ALIGN align = IrAlignOf(document, table, column);

      head[at++] = (align == IR_ALIGN_LEFT ? 'l' : (align == IR_ALIGN_CENTRE ? 'c' : (align == IR_ALIGN_RIGHT ? 'r' : '-')));
   }
   if(table->flags & IR_TABLE_MERGED) head[at++] = 'm';
   if(table->flags & IR_TABLE_NESTED) head[at++] = 'n';
   head[at++] = '{';
   head[at]   = 0;
   WalkAppend(dest, destBytes, used, head);

   ui32 row   = table->firstRow;
   bool first = true;

   while(row != IR_NO_INDEX) {
      cIR_ROWptr record = IrRowAt(document, row);

      if(!record) break;
      if(!first) WalkAppend(dest, destBytes, used, "/");
      first = false;
      if(record->flags & IR_ROW_HEADER) WalkAppend(dest, destBytes, used, "=");

      ui32 cell = record->firstCell;

      while(cell != IR_NO_INDEX) {
         cIR_CELLptr one = IrCellAt(document, cell);

         if(!one) break;
         WalkAppend(dest, destBytes, used, "(");
         if(one->span > 1u) {
            char  width[12];
            cui64 length = WalkNumber(width, one->span);

            width[length]      = ':';
            width[length + 1u] = 0;
            WalkAppend(dest, destBytes, used, width);
         }
         if(one->flags & IR_CELL_VRESTART) WalkAppend(dest, destBytes, used, "v");
         if(one->flags & IR_CELL_VMERGED) WalkAppend(dest, destBytes, used, "^");
         WalkRange(document, one->blockAt, one->blockAt + one->blockCount, dest, destBytes, used);
         WalkAppend(dest, destBytes, used, ")");
         cell = one->nextCell;
      }
      row = record->nextRow;
   }
   WalkAppend(dest, destBytes, used, "}");
}

static void WalkRange(cIR_DOCUMENTptr document, cui32 from, cui32 to, chptrc dest, cui64 destBytes, ui64ptrc used) {
   for(ui32 index = from; index < to; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;
      if(block->kind == IR_BLOCK_TABLE) {
         WalkTable(document, block, dest, destBytes, used);

         cIR_TABLEptr table = IrTableAt(document, block->tableAt);

         if(table && table->blockEnd > index) index = table->blockEnd - 1u;
         continue;
      }

      char head[24];

      ui64 at = WalkList(block, head);

      if(block->kind == IR_BLOCK_HEADING) {
         head[at++] = 'H';
         head[at++] = char('0' + block->headingLevel);
      } else if(block->kind == IR_BLOCK_QUOTE) head[at++] = 'Q';
      else if(block->kind == IR_BLOCK_CODE) head[at++] = 'C';
      else if(block->kind == IR_BLOCK_RULE) head[at++] = 'R';
      else if(block->kind == IR_BLOCK_PARAGRAPH) head[at++] = 'P';
      else head[at++] = '?';
      head[at++] = '{';
      head[at]   = 0;
      WalkAppend(dest, destBytes, used, head);
      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(span->kind == IR_SPAN_BREAK) {
            WalkAppend(dest, destBytes, used, "|");
            continue;
         }
         if(span->kind == IR_SPAN_LINK_END) {
            WalkAppend(dest, destBytes, used, "L)");
            continue;
         }
         if(span->kind == IR_SPAN_LINK_START || span->kind == IR_SPAN_IMAGE || span->kind == IR_SPAN_ANCHOR) {
            WalkAppend(dest, destBytes, used, (span->kind == IR_SPAN_IMAGE ? "I" : (span->kind == IR_SPAN_ANCHOR ? "N" : "L")));
            if(span->flags & IR_SPAN_FLAG_MUTE) WalkAppend(dest, destBytes, used, "-");
            WalkAppend(dest, destBytes, used, "(");
            for(ui32 byte = 0; byte < span->destBytes && *used + 1u < destBytes; ++byte) {
               dest[(*used)++] = IrDest(document, span->destAt)[byte];
            }
            dest[*used] = 0;
            WalkAppend(dest, destBytes, used, ")");
            if(span->kind != IR_SPAN_IMAGE) continue;
         }
         if(span->fmt & IR_FMT_BOLD) WalkAppend(dest, destBytes, used, "b");
         if(span->fmt & IR_FMT_ITALIC) WalkAppend(dest, destBytes, used, "i");
         if(span->fmt & IR_FMT_STRIKE) WalkAppend(dest, destBytes, used, "s");
         if(span->fmt & IR_FMT_SUPER) WalkAppend(dest, destBytes, used, "^");
         if(span->fmt & IR_FMT_SUB) WalkAppend(dest, destBytes, used, "v");
         if(span->fmt & IR_FMT_CODE) WalkAppend(dest, destBytes, used, "c");
         WalkAppend(dest, destBytes, used, "[");
         for(ui32 byte = 0; byte < span->textBytes && *used + 1u < destBytes; ++byte) {
            dest[(*used)++] = IrText(document, span->textAt)[byte];
         }
         dest[*used] = 0;
         WalkAppend(dest, destBytes, used, "]");
      }
      WalkAppend(dest, destBytes, used, "}");
   }
}

// Renders a whole document, in the notation above.
static void WalkTrace(cIR_DOCUMENTptr document, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   dest[0] = 0;
   WalkRange(document, 0, IrBlockCount(document), dest, destBytes, &used);
}

// Walks one body, with an optional styles part in front of it, and compares the trace with a literal.
static cbool TracedAs(cchptr styleBody, cchptr body, cchptr wanted) {
   char part[8192];
   char trace[2048];
   ui64 used = 0;

   part[0] = 0;
   WalkAppend(part, sizeof(part), &used, WALK_HEAD);
   WalkAppend(part, sizeof(part), &used, body);
   WalkAppend(part, sizeof(part), &used, WALK_TAIL);

   STYLE_MODEL styles;
   IR_DOCUMENT document;

   StyleOpen(&styles);
   if(styleBody) {
      char stylePart[4096];
      ui64 styleUsed = 0;

      stylePart[0] = 0;
      WalkAppend(stylePart, sizeof(stylePart), &styleUsed, "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");
      WalkAppend(stylePart, sizeof(stylePart), &styleUsed, styleBody);
      WalkAppend(stylePart, sizeof(stylePart), &styleUsed, "</w:styles>");
      if(StyleLoadBytes(&styles, (cui8ptr)stylePart, styleUsed) != STYLE_OK) {
         StyleClose(&styles);
         return false;
      }
   }
   NUM_MODEL numbering;
   ui64      numberUsed = 0;

   NumOpen(&numbering);
   while(WALK_NUMS[numberUsed]) ++numberUsed;
   if(NumLoadBytes(&numbering, (cui8ptr)WALK_NUMS, numberUsed, nullptr) != NUM_OK) {
      NumClose(&numbering);
      StyleClose(&styles);
      return false;
   }
   IrOpen(&document);

   cWALK_STATUS status = DocWalkBytes(&document, &styles, &numbering, (cui8ptr)part, used);

   if(status.result != WALK_OK) {
      IrClose(&document);
      NumClose(&numbering);
      StyleClose(&styles);
      return false;
   }
   WalkTrace(&document, trace, sizeof(trace));
   IrClose(&document);
   NumClose(&numbering);
   StyleClose(&styles);

   ui64 index = 0;

   while(trace[index] && trace[index] == wanted[index]) ++index;
   return trace[index] == wanted[index];
}

// Walks one body and reports only why it stopped.
static cWALK_RESULT WalkedTo(cchptr part) {
   STYLE_MODEL styles;
   IR_DOCUMENT document;
   NUM_MODEL   numbering;

   StyleOpen(&styles);
   NumOpen(&numbering);
   IrOpen(&document);

   cWALK_STATUS status = DocWalkBytes(&document, &styles, &numbering, (cui8ptr)part, WalkLength(part));

   IrClose(&document);
   NumClose(&numbering);
   StyleClose(&styles);
   return status.result;
}

//== The suite

void TestDocWalker(void);

void TestDocWalker(void) {
   CheckGroup("DocWalker: paragraphs and run content");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>one</w:t></w:r></w:p>", "P{[one]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:t>b</w:t></w:r></w:p>", "P{[a][b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:t>b</w:t></w:r></w:p>", "P{[ab]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>e</w:t><w:noBreakHyphen/><w:t>mail</w:t></w:r></w:p>", "P{[e-mail]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>hy</w:t><w:softHyphen/><w:t>phen</w:t></w:r></w:p>", "P{[hyphen]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:cr/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br w:type=\"textWrapping\"/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   // A page or column break maps to nothing at all, so the text on either side of it joins up.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br w:type=\"page\"/><w:t>b</w:t></w:r></w:p>", "P{[ab]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br w:type=\"column\"/><w:t>b</w:t></w:r></w:p>", "P{[ab]}"));
   // A soft hyphen written as a literal U+00AD goes the same way as the element, per row 34.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>hy\xC2\xADphen</w:t></w:r></w:p>", "P{[hyphen]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>keep \xC2\xA0 the nbsp</w:t></w:r></w:p>", "P{[keep \xC2\xA0 the nbsp]}"));

   CheckGroup("DocWalker: a line end inside a w:t is interior whitespace");
   // WordprocessingML spells a break w:br. A newline character inside a w:t is whitespace, and emitting
   // it would end the Markdown block the text stands in -- a paragraph would become two.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#10;b</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#13;b</w:t></w:r></w:p>", "P{[a b]}"));
   // A carriage return and a line feed together are one line end, so they become one space.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#13;&#10;b</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#10;&#10;b</w:t></w:r></w:p>", "P{[a  b]}"));
   // A literal newline inside the element goes the same way as one written as a reference.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a\nb</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>a&#10;b</w:t></w:r></w:p>", "P{[A B]}"));

   CheckGroup("DocWalker: empty and whitespace-only blocks are dropped");
   CHECK(TracedAs(nullptr, "<w:p/>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r/></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t/></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p/><w:p/><w:p><w:r><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:br/></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>x</w:t><w:br/></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:br/><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   // A block that is dropped must give its arena back, or the next block's text starts in the wrong place.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t xml:space=\"preserve\"> </w:t></w:r></w:p><w:p><w:r><w:t>after</w:t></w:r></w:p>", "P{[after]}"));

   CheckGroup("DocWalker: headings come from the style model");
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"H1\"><w:name w:val=\"heading 1\"/></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H1{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"2\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H3{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"9\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "P{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"x\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "P{[t]}"));
   // ST_DecimalNumber is an xsd:integer, so leading zeros are legal in one and "003" is three. Reading
   // the value under a cap on its *length* rather than on its magnitude makes a padded level a refusal,
   // and the refusal is silent: the paragraph stops being a heading and becomes body text. M8 fixed the
   // style path and TestStyleModel pins it there; this is the paragraph's own w:outlineLvl.
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"003\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H4{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pStyle w:val=\"Absent\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "P{[t]}"));

   CheckGroup("DocWalker: the formatting bits on a span");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{bi[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:strike/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{s[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:dstrike/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{s[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{^[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vertAlign w:val=\"subscript\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{v[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b w:val=\"0\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:u w:val=\"single\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"B\"><w:name w:val=\"B\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"B\"/></w:pPr><w:r><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"B\"><w:name w:val=\"B\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"B\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs("<w:style w:type=\"character\" w:styleId=\"C\"><w:name w:val=\"C\"/><w:rPr><w:i/></w:rPr></w:style>",
                  "<w:p><w:r><w:rPr><w:rStyle w:val=\"C\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{i[x]}"));

   CheckGroup("DocWalker: a heading's bold is style-borne and is not carried as formatting");
   // CLAUDE.md's mapping row 1: heading text is never additionally bolded. IR_FMT is the only channel
   // the emitter has, so the walker clears the bit rather than leaving M6 to guess where it came from.
   CHECK(TracedAs("<w:style w:styleId=\"H1\"><w:name w:val=\"heading 1\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H1{[t]}"));
   CHECK(TracedAs("<w:style w:styleId=\"H1\"><w:name w:val=\"heading 1\"/></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr>"
                  "<w:t>t</w:t></w:r></w:p>",
                  "H1{[t]}"));
   // Italic and strike are not what the ruling names, so they survive into the heading's spans.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr>"
                  "<w:t>t</w:t></w:r></w:p>",
                  "H1{i[t]}"));
   // An ordinary paragraph is untouched by the rule.
   CHECK(TracedAs("<w:style w:styleId=\"B\"><w:name w:val=\"B\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"B\"/></w:pPr>"
                  "<w:r><w:t>t</w:t></w:r></w:p>",
                  "P{b[t]}"));

   CheckGroup("DocWalker: hidden runs are dropped whole");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vanish/></w:rPr><w:t>x</w:t></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vanish/></w:rPr><w:t>gone</w:t></w:r><w:r><w:t>kept</w:t></w:r></w:p>", "P{[kept]}"));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:vanish/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:t>x</w:t></w:r></w:p>", ""));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:vanish/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:rPr><w:vanish w:val=\"0\"/></w:rPr>"
                  "<w:t>x</w:t></w:r></w:p>",
                  "P{[x]}"));

   CheckGroup("DocWalker: transparent wrappers and dropped revisions");
   CHECK(TracedAs(nullptr, "<w:p><w:ins><w:r><w:t>x</w:t></w:r></w:ins></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:moveTo><w:r><w:t>x</w:t></w:r></w:moveTo></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:del><w:r><w:delText>x</w:delText></w:r></w:del></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:moveFrom><w:r><w:t>x</w:t></w:r></w:moveFrom></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:ins><w:p><w:r><w:t>x</w:t></w:r></w:p></w:ins>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:del><w:p><w:r><w:delText>x</w:delText></w:r></w:p></w:del>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:smartTag><w:r><w:t>x</w:t></w:r></w:smartTag></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:customXml><w:r><w:t>x</w:t></w:r></w:customXml></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:sdt><w:sdtPr/><w:sdtContent><w:p><w:r><w:t>x</w:t></w:r></w:p></w:sdtContent></w:sdt>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:sdt><w:sdtPr/><w:sdtContent><w:r><w:t>x</w:t></w:r></w:sdtContent></w:sdt></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:sdt><w:sdtPr><w:alias w:val=\"a\"/></w:sdtPr></w:sdt>", ""));
   // A bookmark is an anchor a link may target (mapping row 22), and its end marker is nothing at all:
   // the start is where a link lands, and the range it covers has no Markdown equivalent.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:bookmarkStart w:id=\"0\" w:name=\"n\"/><w:r><w:t>x</w:t></w:r>"
                  "<w:bookmarkEnd w:id=\"0\"/></w:p>",
                  "P{N(n)[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:proofErr w:type=\"spellStart\"/><w:r><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:fldSimple w:instr=\" PAGE \"><w:r><w:t>7</w:t></w:r></w:fldSimple></w:p>", "P{[7]}"));

   CheckGroup("DocWalker: field instructions never reach the output");
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
                  "<w:r><w:instrText> TOC \\o </w:instrText></w:r>"
                  "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
                  "<w:r><w:t>result</w:t></w:r>"
                  "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>",
                  "P{[result]}"));

   CheckGroup("DocWalker: mc:AlternateContent takes the fallback");
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:r><w:t>choice</w:t></w:r></mc:Choice>"
                  "<mc:Fallback><w:r><w:t>fallback</w:t></w:r></mc:Fallback></mc:AlternateContent></w:p>",
                  "P{[fallback]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:r><w:t>choice</w:t></w:r></mc:Choice>"
                  "</mc:AlternateContent></w:p>",
                  "P{[choice]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"a\"><w:r><w:t>one</w:t></w:r></mc:Choice>"
                  "<mc:Choice Requires=\"b\"><w:r><w:t>two</w:t></w:r></mc:Choice></mc:AlternateContent></w:p>",
                  "P{[one]}"));
   CHECK(TracedAs(nullptr,
                  "<mc:AlternateContent><mc:Choice Requires=\"wps\"><w:p><w:r><w:t>choice</w:t></w:r></w:p></mc:Choice>"
                  "<mc:Fallback><w:p><w:r><w:t>fallback</w:t></w:r></w:p></mc:Fallback></mc:AlternateContent>",
                  "P{[fallback]}"));

   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Fallback><w:r><w:t>only a fallback</w:t></w:r></mc:Fallback>"
                  "</mc:AlternateContent></w:p>",
                  "P{[only a fallback]}"));
   // A rewound Choice must not have voted. The paragraph's row 12 verdict is walker state rather than
   // IR, so IrRewind does not carry it and the walk restores it by hand -- without which the discarded
   // plain Choice below demotes the all-monospace Fallback that survives from a fence to a code span.
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:r><w:t>plain</w:t></w:r></mc:Choice>"
                  "<mc:Fallback><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>let a = 1;</w:t></w:r>"
                  "</mc:Fallback></mc:AlternateContent></w:p>",
                  "C{c[let a = 1;]}"));
   // And the other direction: a rewound *monospace* Choice must not leave a plain Fallback looking
   // like code either, which is what a restore that only cleared the flags would do.
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent>"
                  "<mc:Choice Requires=\"wps\"><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr>"
                  "<w:t>code</w:t></w:r></mc:Choice>"
                  "<mc:Fallback><w:r><w:t>plain</w:t></w:r></mc:Fallback></mc:AlternateContent></w:p>",
                  "P{[plain]}"));

   CheckGroup("DocWalker: caps uppercases the text, and hidden runs go either way");
   // Mapping row 37: caps uppercases, smallCaps leaves the text as typed.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>chapter one</w:t></w:r></w:p>", "P{[CHAPTER ONE]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:smallCaps/></w:rPr><w:t>chapter one</w:t></w:r></w:p>", "P{[chapter one]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>caf\xC3\xA9 na\xC3\xAFve</w:t></w:r></w:p>", "P{[CAF\xC3\x89 NA\xC3\x8FVE]}"));
   // Two exclusions, both because 0x20 is not their distance: the sharp s grows to two letters when it
   // is uppercased, and y-diaeresis's uppercase form is nowhere near it.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>stra\xC3\x9F"
                  "e</w:t></w:r></w:p>",
                  "P{[STRA\xC3\x9F"
                  "E]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>1 \xC3\xB7 2</w:t></w:r></w:p>", "P{[1 \xC3\xB7 2]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>hy</w:t><w:softHyphen/>"
                  "<w:t>phen</w:t></w:r></w:p>",
                  "P{[HYPHEN]}"));
   // caps is a toggle, so two specifications of it across the two style chains cancel the way any
   // other pair does -- while a run naming it itself is direct formatting, which is final.
   CHECK(TracedAs("<w:style w:styleId=\"C\"><w:name w:val=\"C\"/><w:rPr><w:caps/></w:rPr></w:style>"
                  "<w:style w:type=\"character\" w:styleId=\"K\"><w:name w:val=\"K\"/>"
                  "<w:rPr><w:caps/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"C\"/></w:pPr><w:r><w:rPr><w:rStyle w:val=\"K\"/></w:rPr>"
                  "<w:t>quiet</w:t></w:r></w:p>",
                  "P{[quiet]}"));
   CHECK(TracedAs("<w:style w:styleId=\"C\"><w:name w:val=\"C\"/><w:rPr><w:caps/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"C\"/></w:pPr><w:r><w:rPr><w:caps/></w:rPr>"
                  "<w:t>loud</w:t></w:r></w:p>",
                  "P{[LOUD]}"));
   // w:webHidden is not a toggle -- 17.7.3 does not list it -- but a run it hides is dropped all the same.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:webHidden/></w:rPr><w:t>gone</w:t></w:r>"
                  "<w:r><w:t>kept</w:t></w:r></w:p>",
                  "P{[kept]}"));
   CHECK(TracedAs("<w:style w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:webHidden/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:rPr><w:webHidden w:val=\"0\"/></w:rPr>"
                  "<w:t>shown</w:t></w:r></w:p>",
                  "P{[shown]}"));
   // Nearest-wins, not XOR: two specifications of true stay true where two toggles would cancel.
   CHECK(TracedAs("<w:style w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:webHidden/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:rPr><w:webHidden/></w:rPr>"
                  "<w:t>gone</w:t></w:r></w:p>",
                  ""));

   CheckGroup("DocWalker: run containers whose text is content");
   CHECK(TracedAs(nullptr, "<w:p><w:dir w:val=\"rtl\"><w:r><w:t>x</w:t></w:r></w:dir></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:bdo w:val=\"ltr\"><w:r><w:t>x</w:t></w:r></w:bdo></w:p>", "P{[x]}"));
   // A ruby annotation is printed above its base text, which Markdown has nowhere to put; the base is
   // the sentence, so it is what survives.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:ruby><w:rubyPr/><w:rt><w:r><w:t>anno</w:t></w:r></w:rt>"
                  "<w:rubyBase><w:r><w:t>base</w:t></w:r></w:rubyBase></w:ruby></w:p>",
                  "P{[base]}"));

   // M9's tables, in the notation the header of this file describes: T, the column count, one
   // character of alignment per column, then m for a merge and n for a nested table; rows separated
   // by "/", a w:tblHeader row prefixed by "=", and a cell its blocks between parentheses.
   CheckGroup("DocWalker: tables");
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>x</w:t></w:r></w:p></w:tc></w:tr></w:tbl>", "T1-{(P{[x]})}"));
   // A skip does not eat the siblings that follow it, which a table has to satisfy like every other
   // element the walk consumes whole.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>in a cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
                  "<w:p><w:r><w:t>after</w:t></w:r></w:p>",
                  "T1-{(P{[in a cell]})}P{[after]}"));

   CheckGroup("DocWalker: a table's shape");
   // The grid is the column count the emitter pads to; a row holding fewer cells is not padded here,
   // because padding is what the emitter does and the walk records what the document said.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tblGrid><w:gridCol/><w:gridCol/><w:gridCol/></w:tblGrid>"
                  "<w:tr><w:tc><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc>"
                  "<w:tc><w:p><w:r><w:t>c</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T3---{(P{[a]})(P{[b]})(P{[c]})}"));
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tblGrid><w:gridCol/><w:gridCol/><w:gridCol/></w:tblGrid>"
                  "<w:tr><w:tc><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T3---{(P{[a]})(P{[b]})}"));
   // A row reaching past the grid widens the table rather than losing its cell, which is row 19's
   // "never silently drop columns" read the only way a pipe table can honour it.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tblGrid><w:gridCol/></w:tblGrid>"
                  "<w:tr><w:tc><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc><w:tc><w:p><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T2--{(P{[a]})(P{[b]})}"));
   // A merge is recorded as a fact and never as a verdict: whether it becomes raw HTML is --tables.
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:tc>" WALK_SPAN_2 WALK_PARA_A "</w:tc></w:tr></w:tbl>", "T2--m{(2:P{[a]})}"));
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:tc><w:tcPr><w:vMerge w:val=\"restart\"/></w:tcPr><w:p><w:r><w:t>a</w:t></w:r></w:p></w:tc></w:tr>"
                  "<w:tr><w:tc><w:tcPr><w:vMerge/></w:tcPr><w:p/></w:tc></w:tr></w:tbl>",
                  "T1-m{(vP{[a]})/(^)}"));
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:trPr><w:tblHeader/></w:trPr>" WALK_CELL_A "</w:tr></w:tbl>", "T1-{=(P{[a]})}"));
   // A cell with no w:p, and a row with no w:tc. The schema forbids both; each is still a column and a
   // row a reader sees, so neither costs the table its shape.
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:tc/></w:tr></w:tbl>", "T1-{()}"));
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr/></w:tbl>", "T1-{}"));
   // A table with no rows at all is unwound whole, so it costs no block and no blank line.
   CHECK(TracedAs(nullptr, "<w:tbl><w:tblGrid><w:gridCol/></w:tblGrid></w:tbl>", ""));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:t>before</w:t></w:r></w:p><w:tbl><w:tblGrid><w:gridCol/></w:tblGrid></w:tbl>"
                  "<w:p><w:r><w:t>after</w:t></w:r></w:p>",
                  "P{[before]}P{[after]}"));

   CheckGroup("DocWalker: a table's alignment");
   // Only the first row's cells can reach a delimiter row, so only the first row's w:jc is kept.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:tc><w:p><w:pPr><w:jc w:val=\"center\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p></w:tc></w:tr>"
                  "<w:tr><w:tc><w:p><w:pPr><w:jc w:val=\"right\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T1c{(P{[a]})/(P{[b]})}"));
   // start and end are the bidirectional spellings of left and right, which this build has no
   // bidirectional layout to reverse; both and distribute are alignments GFM cannot spell at all.
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr>" WALK_JC("start") "</w:tr></w:tbl>", "T1l{(P{[a]})}"));
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr>" WALK_JC("end") "</w:tr></w:tbl>", "T1r{(P{[a]})}"));
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr>" WALK_JC("both") "</w:tr></w:tbl>", "T1-{(P{[a]})}"));
   // A cell spanning two columns aligns both, which is the only reading when one cell speaks for two.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tblGrid><w:gridCol/><w:gridCol/></w:tblGrid><w:tr>"
                  "<w:tc><w:tcPr><w:gridSpan w:val=\"2\"/></w:tcPr><w:p><w:pPr><w:jc w:val=\"right\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p></w:tc>"
                  "</w:tr></w:tbl>",
                  "T2rrm{(2:P{[a]})}"));

   CheckGroup("DocWalker: a table's wrappers and revisions");
   CHECK(TracedAs(nullptr, "<w:tbl><w:sdt><w:sdtContent>" WALK_ROW_A "</w:sdtContent></w:sdt></w:tbl>", "T1-{(P{[a]})}"));
   CHECK(TracedAs(nullptr, "<w:tbl><w:ins>" WALK_ROW_A "</w:ins></w:tbl>", "T1-{(P{[a]})}"));
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:customXml>" WALK_CELL_A "</w:customXml></w:tr></w:tbl>", "T1-{(P{[a]})}"));
   // Accept-all revisions, correctness rule 8: a row a tracked change deleted is not there at all.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:trPr><w:del/></w:trPr><w:tc><w:p><w:r><w:t>gone</w:t></w:r></w:p></w:tc></w:tr>"
                  "<w:tr><w:tc><w:p><w:r><w:t>kept</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T1-{(P{[kept]})}"));

   CheckGroup("DocWalker: a table nests, and a rewind heals its chain");
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:tc><w:tbl>" WALK_ROW_IN "</w:tbl><w:p/></w:tc></w:tr></w:tbl>", "T1-n{(T1-{(P{[in]})})}"));
   // An mc:AlternateContent may wrap a whole table, and the discarded mc:Choice's rows and cells go
   // with its blocks -- or the next table would inherit records nothing points at.
   CHECK(TracedAs(nullptr,
                  "<mc:AlternateContent><mc:Choice Requires=\"x\"><w:tbl>" WALK_ROW_CHOICE "</w:tbl></mc:Choice>"
                  "<mc:Fallback><w:tbl>" WALK_ROW_FALLBACK "</w:tbl></mc:Fallback></mc:AlternateContent>",
                  "T1-{(P{[fallback]})}"));
   // And it may wrap a w:tr inside one, which is the case the chain tails on the walk context exist
   // for: the row after the discarded one has to link behind the row that really precedes it.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>first</w:t></w:r></w:p></w:tc></w:tr>"
                  "<mc:AlternateContent><mc:Choice Requires=\"x\"><w:tr><w:tc><w:p><w:r><w:t>choice</w:t></w:r></w:p></w:tc></w:tr></mc:Choice>"
                  "<mc:Fallback><w:tr><w:tc><w:p><w:r><w:t>fallback</w:t></w:r></w:p></w:tc></w:tr></mc:Fallback></mc:AlternateContent>"
                  "<w:tr><w:tc><w:p><w:r><w:t>last</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T1-{(P{[first]})/(P{[fallback]})/(P{[last]})}"));
   // The same inside one row, for its cells.
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>first</w:t></w:r></w:p></w:tc>"
                  "<mc:AlternateContent><mc:Choice Requires=\"x\"><w:tc><w:p><w:r><w:t>choice</w:t></w:r></w:p></w:tc></mc:Choice>"
                  "<mc:Fallback><w:tc><w:p><w:r><w:t>fallback</w:t></w:r></w:p></w:tc></mc:Fallback></mc:AlternateContent>"
                  "<w:tc><w:p><w:r><w:t>last</w:t></w:r></w:p></w:tc></w:tr></w:tbl>",
                  "T3---{(P{[first]})(P{[fallback]})(P{[last]})}"));

   CheckGroup("DocWalker: a table says nothing a discarded branch declared");
   // Everything a table says about its own shape is derived from the records that survived, so a cell
   // an mc:Fallback replaced leaves behind no column, no alignment and no merge. Accumulated during
   // the walk instead, the three-cell Choice below widened this table to three columns and aligned its
   // first one right, and the gridSpan in the second put it into raw HTML --tables never asked for.
   {
      cchptr widened = "<w:tbl>" WALK_GRID_2 "<mc:AlternateContent>"
                       "<mc:Choice Requires=\"zz\"><w:tr>" WALK_JC("right") WALK_CELL_A WALK_CELL_A "</w:tr></mc:Choice><mc:Fallback>" WALK_ROW_FF
                                                                                                    "</mc:Fallback></mc:AlternateContent></w:tbl>";
      cchptr merged = "<w:tbl>" WALK_GRID_2 "<w:tr><mc:AlternateContent>"
                      "<mc:Choice Requires=\"zz\"><w:tc>" WALK_SPAN_2 WALK_PARA_A "</w:tc></mc:Choice>"
                      "<mc:Fallback>" WALK_CELL_F1 "</mc:Fallback></mc:AlternateContent>" WALK_CELL_F2 "</w:tr></w:tbl>";

      CHECK(TracedAs(nullptr, widened, "T2--{(P{[F1]})(P{[F2]})}"));
      CHECK(TracedAs(nullptr, merged, "T2--{(P{[F1]})(P{[F2]})}"));
   }
   // A nested table that came to nothing must not force its parent into raw HTML it does not need, so
   // the parent is marked only once the nested one knows it survived.
   {
      cchptr hollow = "<w:tbl>" WALK_GRID_1 "<w:tr><w:tc>" WALK_PARA_A "<w:tbl>" WALK_GRID_1 "</w:tbl><w:p/></w:tc></w:tr></w:tbl>";
      cchptr dead   = "<w:tbl>" WALK_GRID_1 "<w:tr><w:trPr><w:del/></w:trPr>" WALK_CELL_A "</w:tr></w:tbl>";
      cchptr gone   = "<w:tbl>" WALK_GRID_1 "<w:tr><w:tc>";

      CHECK(TracedAs(nullptr, hollow, "T1-{(P{[a]})}"));
      char joined[512];
      ui64 used = 0;

      joined[0] = 0;
      WalkAppend(joined, sizeof(joined), &used, gone);
      WalkAppend(joined, sizeof(joined), &used, dead);
      WalkAppend(joined, sizeof(joined), &used, WALK_PARA_A "</w:tc></w:tr></w:tbl>");
      CHECK(TracedAs(nullptr, joined, "T1-{(P{[a]})}"));
   }

   CheckGroup("DocWalker: a table nested past the cap is dropped");
   {
      char deep[4096];
      ui64 used = 0;

      deep[0] = 0;
      for(ui32 level = 0; level < IR_MAX_TABLE_DEPTH + 1u; ++level) WalkAppend(deep, sizeof(deep), &used, "<w:tbl><w:tr><w:tc>");
      WalkAppend(deep, sizeof(deep), &used, "<w:p><w:r><w:t>deep</w:t></w:r></w:p>");
      for(ui32 level = 0; level < IR_MAX_TABLE_DEPTH + 1u; ++level) WalkAppend(deep, sizeof(deep), &used, "</w:tc></w:tr></w:tbl>");

      char wanted[4096];
      ui64 at = 0;

      wanted[0] = 0;
      // Every table but the last opens; the one past the cap is skipped whole, so the cell it would
      // have stood in comes to nothing and the text inside it goes with it.
      for(ui32 level = 0; level < IR_MAX_TABLE_DEPTH; ++level) {
         cbool outer = (level + 1u < IR_MAX_TABLE_DEPTH);

         WalkAppend(wanted, sizeof(wanted), &at, (outer ? "T1-n{(" : "T1-{("));
      }
      for(ui32 level = 0; level < IR_MAX_TABLE_DEPTH; ++level) WalkAppend(wanted, sizeof(wanted), &at, ")}");
      CHECK(TracedAs(nullptr, deep, wanted));
   }

   CheckGroup("DocWalker: what is skipped whole");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:drawing><w:t>x</w:t></w:drawing></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:sym w:font=\"Symbol\" w:char=\"F0B7\"/></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:sectPr><w:pgSz w:w=\"1\"/></w:sectPr>", ""));
   CHECK(TracedAs(nullptr, "<w:unheardOf><w:p><w:r><w:t>x</w:t></w:r></w:p></w:unheardOf>", ""));

   CheckGroup("DocWalker: a skip does not eat the siblings that follow it");
   CHECK(TracedAs(nullptr,
                  "<w:unheardOf><w:p><w:r><w:t>inside</w:t></w:r></w:p></w:unheardOf>"
                  "<w:p><w:r><w:t>after</w:t></w:r></w:p>",
                  "P{[after]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:unheardOf><w:r><w:t>inside</w:t></w:r></w:unheardOf>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "P{[after]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:unheardOf><w:t>inside</w:t></w:unheardOf><w:t>after</w:t></w:r></w:p>", "P{[after]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:unheardOf><w:b/></w:unheardOf></w:rPr><w:t>x</w:t></w:r></w:p>", "P{[x]}"));

   CheckGroup("DocWalker: a pretty-printed part leaks no indentation");
   // Whitespace between elements is character data too. Only what stands inside a w:t is content, and
   // a part a producer indented for a human must convert to the same bytes as one it did not.
   CHECK(TracedAs(nullptr,
                  "\n  <w:p>\n    <w:pPr>\n      <w:pStyle w:val=\"None\"/>\n    </w:pPr>\n"
                  "    <w:r>\n      <w:t>one</w:t>\n    </w:r>\n"
                  "    <w:r>\n      <w:t xml:space=\"preserve\"> two</w:t>\n    </w:r>\n  </w:p>\n",
                  "P{[one][ two]}"));

   CheckGroup("DocWalker: complex-script bold and italic fold into one bit each");
   // Word writes w:b and w:bCs together whenever anything is bolded, so a run carrying only the
   // complex-script twin is complex-script text that really is bold.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:bCs/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:iCs/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{i[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b/><w:bCs/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));

   CheckGroup("DocWalker: refusals");
   CHECK(WalkedTo("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                  "<w:body/></w:document>") == WALK_OK);
   CHECK(WalkedTo("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == WALK_ERROR_ROOT);
   CHECK(WalkedTo("<w:other xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == WALK_ERROR_ROOT);
   CHECK(WalkedTo("") == WALK_ERROR_XML);
   CHECK(WalkedTo("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                  "<w:body><w:p>") == WALK_ERROR_XML);
   // ISO 29500 Strict spells the namespace differently and must walk exactly the same code.
   CHECK(WalkedTo("<w:document xmlns:w=\"http://purl.oclc.org/ooxml/wordprocessingml/main\">"
                  "<w:body><w:p><w:r><w:t>x</w:t></w:r></w:p></w:body></w:document>") == WALK_OK);

   CheckGroup("DocWalker: the result sentences track their enumeration");

   WALK_STATUS root = {WALK_ERROR_ROOT, XML_OK, OPC_OK};
   WALK_STATUS none = {WALK_OK, XML_OK, OPC_OK};
   ui64        at   = 0;
   cchptr      said = DocWalkResultText(nullptr, root);
   cchptr      want = "the main document part\'s root element is not w:document";

   while(said[at] && said[at] == want[at]) ++at;
   CHECK(said[at] == want[at]);
   CHECK(DocWalkResultText(nullptr, none)[0] != 0);

   CheckGroup("DocWalker: the block kinds it classifies");
   // The walker never merges: the fragmentation is what RunCoalescer is measured against, so preserving
   // it here is the property, not an omission.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Hel</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>lo</w:t></w:r></w:p>",
                  "P{b[Hel]b[lo]}"));
   // A quote style and a code style each give their own block kind, and a heading beats both.
   CHECK(TracedAs(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "Q{[a]}"));
   CHECK(TracedAs(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "C{[a]}"));
   {
      char styled[512];
      ui64 used = 0;

      styled[0] = 0;
      WalkAppend(styled, sizeof(styled), &used, STYLE_QUOTE);
      WalkAppend(styled, sizeof(styled), &used, STYLE_QH_BASE);
      WalkAppend(styled, sizeof(styled), &used, STYLE_QH_BODY);
      CHECK(TracedAs(styled, "<w:p><w:pPr><w:pStyle w:val=\"QH\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "H3{[a]}"));
   }
   // A style-borne role does not let a w:outlineLvl on the paragraph turn a quotation into a heading.
   CHECK(TracedAs(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "Q{[a]}"));
   // Row 12's second detection is over the font alone, so one run that is not monospace settles it.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Menlo\"/></w:rPr><w:t>b</w:t></w:r></w:p>",
                  "C{c[a]c[b]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a</w:t></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "P{c[a][b]}"));
   // A run that produces no text votes on nothing, so a w:br between two monospace runs is still code.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a</w:t>"
                  "<w:br/><w:t>b</w:t></w:r></w:p>",
                  "C{c[a]|c[b]}"));
   // A run that contributes no visible character votes on nothing either, and the two ways a run can do
   // that are the two DocAppendText removes: a CR or an LF inside a w:t folds to one space, and a soft
   // hyphen is dropped outright. Word gives a hyphenation point from a later session its own w:r.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>let a</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\">&#10;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>= 1;</w:t></w:r></w:p>",
                  "C{c[let a][ ]c[= 1;]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>let b</w:t></w:r>"
                  "<w:r><w:t>&#xAD;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t> = 2;</w:t></w:r></w:p>",
                  "C{c[let b][]c[ = 2;]}"));
   // A carriage return and a tab abstain for the same reason, and each needs its own case: none of the
   // others reaches the byte, so deleting either from the set leaves every suite green.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>let c</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\">&#13;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>= 3;</w:t></w:r></w:p>",
                  "C{c[let c][ ]c[= 3;]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>let d</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\">&#9;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>= 4;</w:t></w:r></w:p>",
                  "C{c[let d][\t]c[= 4;]}"));
   // The set is the tab and the Zs category, the same class RunCoalescer hoists -- a body-font U+2002 or
   // U+3000 between two code runs is the fragmentation rule 4 absorbs, not a vote against the fence.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>int</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\">&#8194;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>x;</w:t></w:r></w:p>",
                  "C{c[int][\xE2\x80\x82]c[x;]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>int</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\">&#12288;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>y;</w:t></w:r></w:p>",
                  "C{c[int][\xE3\x80\x80]c[y;]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>int</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\">&#8239;</w:t></w:r>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>z;</w:t></w:r></w:p>",
                  "C{c[int][\xE2\x80\xAF]c[z;]}"));
   // A non-breaking space is content, per mapping row 35, so a run of one is not in that set. That is a
   // deliberate exclusion from the Zs class above, and the one place the two questions differ.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a</w:t></w:r>"
                  "<w:r><w:t>\xC2\xA0</w:t></w:r></w:p>",
                  "P{c[a][\xC2\xA0]}"));
   // A paragraph with nothing in it is not a code block by the font heuristic: there is no run to look at.
   CHECK(TracedAs(nullptr, "<w:p/>", ""));
   // But an empty paragraph wearing a code *style* is a blank line of the fence, so it keeps its block.
   CHECK(TracedAs(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>", "C{}"));

   CheckGroup("DocWalker: a hyperlink becomes a link span pair");
   // CLAUDE.md's mapping rows 21 and 22. The walk records the reference as written and nothing more:
   // ids are scoped to the part they were read in, so LinkResolve does the lookup where the part is
   // known, and a w:anchor is a bookmark name that the same pass turns into a slug or an anchor.
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink r:id=\"rId5\"><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{L(rId5)[x]L)}"));
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink w:anchor=\"top\"><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{L(#top)[x]L)}"));
   // Both at once is a link into another document, so the relationship decides and the anchor becomes
   // the fragment of whatever it resolves to. A relationship id cannot hold a '#', so the seam is safe.
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink r:id=\"rId5\" w:anchor=\"top\"><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{L(rId5#top)[x]L)}"));
   // A hyperlink naming nothing is a container and nothing else -- its text is still content.
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{[x]}"));
   // Links do not nest in Markdown, so the outer one is the one a reader was given.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:hyperlink r:id=\"rId5\"><w:hyperlink r:id=\"rId6\">"
                  "<w:r><w:t>x</w:t></w:r></w:hyperlink></w:hyperlink></w:p>",
                  "P{L(rId5)[x]L)}"));
   // Formatting inside a hyperlink is content of the link, which is CONVERSION_REFERENCE 5.6.
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink r:id=\"rId5\"><w:r><w:rPr><w:b/></w:rPr><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{L(rId5)b[x]L)}"));

   CheckGroup("DocWalker: a picture becomes an image span");
   // CONVERSION_REFERENCE 2.6: the alt text is the description first, the title behind it and the
   // object's own name last, and the source is r:embed before r:link.
   CHECK(TracedAs(nullptr, "<w:p>" DRAWING_OPEN "descr=\"A cat\"" DRAWING_BLIP "r:embed=\"rId2\"" DRAWING_SHUT "</w:p>", "P{I(rId2)[A cat]}"));
   CHECK(TracedAs(nullptr, "<w:p>" DRAWING_OPEN "name=\"Picture 1\"" DRAWING_BLIP "r:embed=\"rId2\"" DRAWING_SHUT "</w:p>", "P{I(rId2)[Picture 1]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p>" DRAWING_OPEN "descr=\"A cat\" title=\"T\" name=\"N\"" // Every one at once
                  DRAWING_BLIP "r:embed=\"rId2\"" DRAWING_SHUT "</w:p>",
                  "P{I(rId2)[A cat]}"));
   CHECK(TracedAs(nullptr, "<w:p>" DRAWING_OPEN "descr=\"A cat\"" DRAWING_BLIP "r:link=\"rId3\"" DRAWING_SHUT "</w:p>", "P{I(rId3)[A cat]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p>" DRAWING_OPEN "descr=\"A cat\"" DRAWING_BLIP // Both references at once
                  "r:embed=\"rId2\" r:link=\"rId3\"" DRAWING_SHUT "</w:p>",
                  "P{I(rId2)[A cat]}"));
   // A line end in an attribute cannot survive into a link label, so it folds to one space.
   CHECK(TracedAs(nullptr, "<w:p>" DRAWING_OPEN "descr=\"a&#10;b\"" DRAWING_BLIP "r:embed=\"rId2\"" DRAWING_SHUT "</w:p>", "P{I(rId2)[a b]}"));
   // Legacy VML, which is what an older file and every mc:Fallback carries.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:pict><v:shape alt=\"A logo\">" // The shape carries the alt
                  "<v:imagedata r:id=\"rId4\"/></v:shape></w:pict></w:r></w:p>",
                  "P{I(rId4)[A logo]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:pict><v:shape>" // The image data carries it instead
                  "<v:imagedata r:id=\"rId4\" o:title=\"A logo\"/></v:shape></w:pict></w:r></w:p>",
                  "P{I(rId4)[A logo]}"));
   // A container holding no picture reference at all -- a chart, a diagram, a drawn shape -- has no
   // bitmap the document could show, so it is rewound to nothing and the text either side rejoins.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:t>a</w:t></w:r>" DRAWING_OPEN "descr=\"A chart\"/></wp:inline></w:drawing></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "P{[a][b]}"));
   // A picture ends the text span beside it. Both halves of a run that draws a picture mid-way are
   // ordinary w:t, and IrAppendText always appends to the *last* span -- so without the split the text
   // after the picture lands inside its alt text and comes out between the image's own brackets.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:t>a</w:t><w:drawing><wp:inline><wp:docPr id=\"1\" descr=\"A cat\"/>"
                  "<a:graphic><a:graphicData><pic:pic><pic:blipFill><a:blip r:embed=\"rId2\"/>"
                  "</pic:blipFill></pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing>"
                  "<w:t>b</w:t></w:r></w:p>",
                  "P{[a]I(rId2)[A cat][b]}"));
   // A blip counts only under a pic:blipFill, which is the DrawingML *picture* vocabulary. The same
   // element under an a:blipFill is the bitmap a drawn shape is painted with, and taking it would emit
   // a shape's wallpaper as the figure the paragraph shows -- and contradict the rule above, since a
   // drawn shape is exactly what "comes to nothing" names.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:drawing><wp:inline><wp:docPr id=\"1\" descr=\"A shape\"/>"
                  "<a:graphic><a:graphicData><a:sp><a:spPr><a:blipFill><a:blip r:embed=\"rId2\"/></a:blipFill>"
                  "</a:spPr></a:sp></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "P{[a][b]}"));
   // A blip that is not the fill's own child is not the picture either: pic:blipFill holds one blip,
   // and anything deeper belongs to an effect, a duotone or a vocabulary this build has not heard of.
   CHECK(TracedAs(nullptr,
                  "<w:p>" DRAWING_OPEN "descr=\"A cat\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill>"
                  "<a:duotone><a:blip r:embed=\"rId2\"/></a:duotone></pic:blipFill></pic:pic>"
                  "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>",
                  ""));
   // CONVERSION_REFERENCE 5.8's double-emit: both branches of an mc:AlternateContent describe the same
   // picture, so reading both and keeping the first reference emits it exactly once.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:drawing><wp:inline>"
                  "<wp:docPr id=\"1\" descr=\"S\"/><a:graphic><a:graphicData><pic:pic><pic:blipFill>"
                  "<a:blip r:embed=\"rId2\"/></pic:blipFill></pic:pic></a:graphicData></a:graphic>"
                  "</wp:inline></w:drawing></mc:Choice><mc:Fallback><w:pict><v:shape alt=\"F\">"
                  "<v:imagedata r:id=\"rId4\"/></v:shape></w:pict></mc:Fallback></mc:AlternateContent></w:r></w:p>",
                  "P{I(rId2)[S]}"));

   CheckGroup("DocWalker: a bookmark becomes an anchor");
   // A bookmark inside a paragraph marks the point it stood at; one between paragraphs, which is legal
   // and what a producer writes when a bookmark wraps whole blocks, attaches to the block that follows.
   CHECK(TracedAs(nullptr, "<w:p><w:bookmarkStart w:id=\"0\" w:name=\"a\"/><w:r><w:t>x</w:t></w:r></w:p>", "P{N(a)[x]}"));
   CHECK(TracedAs(nullptr, "<w:bookmarkStart w:id=\"0\" w:name=\"a\"/><w:p><w:r><w:t>x</w:t></w:r></w:p>", "P{N(a)[x]}"));
   CHECK(TracedAs(nullptr,
                  "<w:bookmarkStart w:id=\"0\" w:name=\"a\"/><w:bookmarkStart w:id=\"1\" w:name=\"b\"/>"
                  "<w:p><w:r><w:t>x</w:t></w:r></w:p>",
                  "P{N(a)N(b)[x]}"));
   // A bookmark keeps a block alive, because it is a link target and something has to carry it. The
   // resolution pass mutes the ones nothing points at, and the emptied blocks go then.
   CHECK(TracedAs(nullptr, "<w:p><w:bookmarkStart w:id=\"0\" w:name=\"a\"/></w:p>", "P{N(a)}"));
   // A bookmark with no name at all is a range marker and nothing else.
   CHECK(TracedAs(nullptr, "<w:p><w:bookmarkStart w:id=\"0\"/><w:r><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   // A bookmark keeps a block alive without putting anything on the page, so mapping row 25's rule --
   // a lone bottom border on a paragraph that "came to nothing" -- still sees a rule here.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr>"
                  "<w:bookmarkStart w:id=\"0\" w:name=\"a\"/></w:p>",
                  "P{N(a)}R{}"));

   CheckGroup("DocWalker: a w:numPr is read as the reference it is");
   // The walk records what the paragraph wrote and resolves nothing: whether the identifier names a list
   // is NumAssignMarkers's question, asked after the walk because a counter cannot be rewound.
   {
      cchptr flat   = "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>";
      cchptr deep   = "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"2\"/><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                      "<w:r><w:t>a</w:t></w:r></w:p>";
      cchptr deeper = "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"40\"/><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                      "<w:r><w:t>a</w:t></w:r></w:p>";

      CHECK(TracedAs(nullptr, flat, "[0#4]P{[a]}"));
      CHECK(TracedAs(nullptr, deep, "[2#4]P{[a]}"));
      // 0 to 8 is every level the schema has, and a deeper one is clamped rather than refused.
      CHECK(TracedAs(nullptr, deeper, "[8#4]P{[a]}"));
   }
   // A w:numPr with no w:numId names no list, so nothing is recorded at all.
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"1\"/></w:numPr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "P{[a]}"));
   // An empty numbered paragraph still gets its block: a marker on a line of its own is what Word draws,
   // and IrEndBlock would otherwise unwind it like any other empty paragraph.
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr></w:p>", "[0#4]P{}"));
   // A padded w:ilvl and a padded w:numId are the same xsd:integer question as w:outlineLvl above: a
   // cap on the value's length refuses a legal value, and every refusal here is silent. A padded
   // identifier stops the paragraph being an item at all; a padded level loses the depth it named.
   {
      cchptr padded  = "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"002\"/><w:numId w:val=\"004\"/></w:numPr></w:pPr>"
                       "<w:r><w:t>a</w:t></w:r></w:p>";
      cchptr hundred = "<w:p><w:pPr><w:numPr><w:ilvl w:val=\"100\"/><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                       "<w:r><w:t>a</w:t></w:r></w:p>";
      cchptr wide    = "<w:p><w:pPr><w:numPr><w:numId w:val=\"000000000004\"/></w:numPr></w:pPr>"
                       "<w:r><w:t>a</w:t></w:r></w:p>";
      cchptr past    = "<w:p><w:pPr><w:numPr><w:numId w:val=\"2147483648\"/></w:numPr></w:pPr>"
                       "<w:r><w:t>a</w:t></w:r></w:p>";

      CHECK(TracedAs(nullptr, padded, "[2#4]P{[a]}"));
      // Three digits is past what the old cap allowed at all, so a level that deep was discarded and
      // the paragraph fell back to the style chain. It is clamped now, which is what the call site's
      // own comment and StyleModel's twin have both said all along.
      CHECK(TracedAs(nullptr, hundred, "[8#4]P{[a]}"));
      // Twelve characters, past the old ten-character cap: where a cap on length and a cap on
      // magnitude visibly part company.
      CHECK(TracedAs(nullptr, wide, "[0#4]P{[a]}"));
      // The overflow test is the only bound left and it still holds: 2^31 does not fit an si32, so the
      // value is refused and the reference stays unspecified rather than wrapping to a negative one.
      CHECK(TracedAs(nullptr, past, "P{[a]}"));
   }
   CHECK(TracedAs(nullptr, "<w:p></w:p>", ""));

   CheckGroup("DocWalker: where numbering comes from and what cancels it");
   {
      char styled[512];
      ui64 used = 0;

      styled[0] = 0;
      WalkAppend(styled, sizeof(styled), &used, STYLE_LIST_BASE);
      WalkAppend(styled, sizeof(styled), &used, STYLE_LIST_BODY);
      // A style's own w:pPr carries numbering, which is how LibreOffice's ListNumber works and how the
      // built-in ListParagraph pattern is written.
      CHECK(TracedAs(styled, "<w:p><w:pPr><w:pStyle w:val=\"LN\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "[1#4]P{[a]}"));
      // The paragraph's own w:numPr wins outright, and each half of it falls through on its own: a
      // producer that changes the level without changing the list writes only the w:ilvl.
      CHECK(TracedAs(styled,
                     "<w:p><w:pPr><w:pStyle w:val=\"LN\"/><w:numPr><w:ilvl w:val=\"3\"/></w:numPr></w:pPr>"
                     "<w:r><w:t>a</w:t></w:r></w:p>",
                     "[3#4]P{[a]}"));
      CHECK(TracedAs(styled,
                     "<w:p><w:pPr><w:pStyle w:val=\"LN\"/><w:numPr><w:numId w:val=\"9\"/></w:numPr></w:pPr>"
                     "<w:r><w:t>a</w:t></w:r></w:p>",
                     "[1#9]P{[a]}"));
      // w:numId 0 is a specification of "no numbering" and cancels what the style chain supplied, which
      // is the whole reason 0 is reserved -- reading it as an absence would silently un-cancel it.
      CHECK(TracedAs(styled,
                     "<w:p><w:pPr><w:pStyle w:val=\"LN\"/><w:numPr><w:numId w:val=\"0\"/></w:numPr></w:pPr>"
                     "<w:r><w:t>a</w:t></w:r></w:p>",
                     "P{[a]}"));
   }
   {
      cchptr adrift = "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr>"
                      "<w:numPr><w:numId w:val=\"7\"/></w:numPr></w:pPr></w:p>";
      cchptr living = "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr>"
                      "<w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr></w:p>";

      // A w:numId that names no w:num cancels nothing, because the paragraph is an item of nothing.
      // Row 25's rule and row 12's font detection are suppressed by the list *marker* Word draws beside
      // the border, and a reference that resolves to nothing draws none. Decided on the reference alone,
      // a broken numbering graph deleted the horizontal rule from the document outright -- a defect in a
      // reference losing output that has nothing to do with it, which is the opposite of what
      // CONVERSION_REFERENCE 5.4 asks a dangling one to do. numId 7 names nothing; numId 4 is a list.
      CHECK(TracedAs(nullptr, adrift, "R{}"));
      CHECK(TracedAs(nullptr, living, "[0#4]P{}"));
   }
   {
      char styled[512];
      ui64 used = 0;

      styled[0] = 0;
      WalkAppend(styled, sizeof(styled), &used, STYLE_NUMH_BASE);
      WalkAppend(styled, sizeof(styled), &used, STYLE_NUMH_BODY);
      // CONVERSION_REFERENCE 5.4: a heading carrying numbering is a heading and nothing else. Word's
      // Multilevel List linked to headings puts a w:numPr on every Heading N style, so without this
      // every heading in such a document would become a list item and its structure would invert.
      CHECK(TracedAs(styled, "<w:p><w:pPr><w:pStyle w:val=\"NH\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "H1{[a]}"));
   }
   // A quotation and a line of code both keep their marker: a paragraph may legitimately be an item of a
   // list and be one of those, so only a heading cancels the numbering.
   CHECK(TracedAs(STYLE_QUOTE,
                  "<w:p><w:pPr><w:pStyle w:val=\"Q\"/><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                  "<w:r><w:t>a</w:t></w:r></w:p>",
                  "[0#4]Q{[a]}"));
   CHECK(TracedAs(STYLE_CODE,
                  "<w:p><w:pPr><w:pStyle w:val=\"SC\"/><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                  "<w:r><w:t>a</w:t></w:r></w:p>",
                  "[0#4]C{[a]}"));
   // Row 12's monospace *guess* does not overrule a statement: a list of code lines set in Consolas is
   // still a list, where the same paragraph without the w:numPr would be a fence.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                  "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a</w:t></w:r></w:p>",
                  "[0#4]P{c[a]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "C{c[a]}"));
   // Row 25's rule is about a paragraph that came to nothing, and one wearing a list marker did not.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr>"
                  "<w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>",
                  "[0#4]P{}"));

   CheckGroup("DocWalker: the horizontal rule of mapping row 25");
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "R{}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:between w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "R{}"));
   // A border element with no w:val at all is taken as present: its presence is the signal.
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:bottom/></w:pBdr></w:pPr></w:p>", "R{}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"none\"/></w:pBdr></w:pPr></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"nil\"/></w:pBdr></w:pPr></w:p>", ""));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:pBdr><w:top w:val=\"single\"/><w:bottom w:val=\"single\"/>"
                  "</w:pBdr></w:pPr></w:p>",
                  ""));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr>"
                  "<w:r><w:t>a</w:t></w:r></w:p>",
                  "P{[a]}"));
   // The sides are tested by name, so an element this build has never heard of inside a w:pBdr is
   // ignored rather than counted as another border. That is the OOXML compatibility model.
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/><w:glow w:rad=\"1\"/></w:pBdr></w:pPr></w:p>", "R{}"));
   // A side that really is one still votes.
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/><w:bar w:val=\"single\"/></w:pBdr></w:pPr></w:p>", ""));
   // A paragraph that came to nothing is one whether it held no runs or only whitespace.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr>"
                  "<w:r><w:t xml:space=\"preserve\">  </w:t></w:r></w:p>",
                  "R{}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:t>a</w:t></w:r></w:p>"
                  "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>"
                  "<w:p><w:r><w:t>b</w:t></w:r></w:p>",
                  "P{[a]}R{}P{[b]}"));
}
