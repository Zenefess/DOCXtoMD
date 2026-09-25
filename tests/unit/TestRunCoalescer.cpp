/*
 * File: TestRunCoalescer.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-26
 * Last Modified: 2026-09-23
 * Description: Unit tests for adjacent-run merging, whitespace hoisting and the order of the two.
 * To Do: 1) Drive a document straight from IrAddSpan once a case needs a shape no body part produces.
 *        2) Drive a merge that crosses a table cell's own edge, which no body part can produce either:
 *           a cell's blocks are blocks, so the pass sees a boundary it can never be asked to cross.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, LinkResolver.h, RunCoalescer.h, StyleModel.h,
 *               typedefs.h
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
#include "RunCoalescer.h"
#include "StyleModel.h"

//-- Helpers

// The three style parts the cases below reach for, named rather than repeated: a quote style, a
// code paragraph style and a code character style, each the shortest form that carries its role.
static constexpr cchptr STYLE_QUOTE = "<w:style w:type=\"paragraph\" w:styleId=\"Q\"><w:name w:val=\"Quote\"/></w:style>";
static constexpr cchptr STYLE_CODE  = "<w:style w:type=\"paragraph\" w:styleId=\"SC\"><w:name w:val=\"Source Code\"/></w:style>";
static constexpr cchptr STYLE_SPAN  = "<w:style w:type=\"character\" w:styleId=\"CC\"><w:name w:val=\"Code\"/></w:style>";

// The root element every body below is wrapped in, kept out of the helper so no line reaches the column
// limit once the formatter has joined what it can.
static constexpr cchptr COALESCE_HEAD = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
                                        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
                                        " xmlns:v=\"urn:schemas-microsoft-com:vml\"><w:body>";
static constexpr cchptr COALESCE_TAIL = "</w:body></w:document>";

// The wrapper a styles part goes inside, for the cases that need one.
static constexpr cchptr STYLES_HEAD = "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
static constexpr cchptr STYLES_TAIL = "</w:styles>";

// The numbering part every case here resolves against. The walk consults it for one bit only -- whether
// a w:numId names a list this document can resolve -- because a reference that resolves to nothing must
// not cancel the horizontal rule of row 25 or the monospace detection of row 12, both of which a real
// list item does cancel. numId 4 and numId 9 resolve; every other identifier is dangling.
#define WALK_NUM(id) "<w:num w:numId=\"" id "\"><w:abstractNumId w:val=\"0\"/></w:num>"

static constexpr cchptr WALK_NUMS = "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                                    "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\">"
                                    "<w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>" WALK_NUM("4") WALK_NUM("9") "</w:numbering>";

// Bytes before the terminator.
static cui64 CoalesceLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Appends a NUL-terminated literal to a buffer.
static void CoalesceAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr text) {
   cui64 length = CoalesceLength(text);

   for(ui64 index = 0; index < length && *used + 1u < destBytes; ++index) dest[(*used)++] = text[index];
   dest[*used] = 0;
}

// Writes one unsigned number into a buffer and reports how many bytes it took. Check.h deliberately
// pulls in no I/O -- it needs only typedefs.h -- so a trace cannot reach for snprintf.
static cui64 CoalesceNumber(chptrc dest, ui32 value) {
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
static cui64 CoalesceList(cIR_BLOCKptr block, chptrc dest) {
   ui64 used = 0;

   if(block->listNumId < 0 && !(block->listFlags & IR_LIST_ITEM)) return 0;
   dest[used++] = '[';
   dest[used++] = char('0' + block->listLevel);
   if(block->listFlags & IR_LIST_ITEM) {
      dest[used++] = '=';
      if(block->listFlags & IR_LIST_PLAIN) used += 0; // A continuation carries no marker to render
      else if(!(block->listFlags & IR_LIST_ORDERED)) dest[used++] = '-';
      else used += CoalesceNumber(dest + used, block->listNumber);
      if(block->listFlags & IR_LIST_FIRST) dest[used++] = '!';
   } else {
      dest[used++] = '#';
      used += CoalesceNumber(dest + used, ui32(block->listNumId));
   }
   dest[used++] = ']';
   return used;
}

// A block kind neither this renderer nor its twin in the other suite spells would come out as an
// ordinary paragraph, which is a plausible trace rather than an obviously wrong one -- exactly the
// failure CLAUDE.md warns of in saying the two are independent copies that must both be edited.
static_assert(ui32(IR_BLOCK_KIND_COUNT) == 6u, "TestRunCoalescer: the trace renderer must spell every block kind; add the new one here.");
static_assert(ui32(IR_SPAN_KIND_COUNT) == 7u, "TestRunCoalescer: the trace renderer must spell every span kind; add the new one here.");

// The trace notation the renderers below share, which is TestDocWalker's: c is a code span and the
// block letters are P, H<level>, Q, C and R. M7's span kinds render the same way here: L(dest) and L)
// for a link, I(source)[alt] for an image, N(name) for a bookmark anchor. M8's list membership stands
// in front of the block letter in the same two forms TestDocWalker spells: [<level>#<numId>] before
// the counter pass, [<level>=<marker>] after it. M10's note references are F(id) for a footnote and
// E(id) for an endnote, F-(id) or E-(id) once LinkResolveNotes has muted one, and a block a note holds
// is prefixed f<w:id>: or e<w:id>:.

// Writes which note a block belongs to, as f<w:id>: for a footnote and e<w:id>: for an endnote, in front
// of everything else the block renders as. A block of the body writes nothing.
static void CoalesceNote(cIR_DOCUMENTptr document, cIR_BLOCKptr block, chptrc dest, cui64 destBytes, ui64ptrc used) {
   cIR_NOTEptr note = IrNoteAt(document, block->note);
   char        head[16];
   ui64        at = 0;

   if(!note) return;
   head[at++] = (note->kind == IR_NOTE_END ? 'e' : 'f');
   if(note->id < 0) head[at++] = '-';
   at += CoalesceNumber(head + at, ui32(note->id < 0 ? -note->id : note->id));
   head[at++] = ':';
   head[at]   = 0;
   CoalesceAppend(dest, destBytes, used, head);
}

// Renders one range of blocks, which is the whole document at the top level and one cell's content
// inside a table. A table's own blocks are the blocks of its cells, so the range renderer and the
// table renderer call each other -- and the range renderer skips past a table's whole block range,
// or every cell's content would be rendered twice: once in the table and once at the top level.
static void CoalesceRange(cIR_DOCUMENTptr document, cui32 from, cui32 to, chptrc dest, cui64 destBytes, ui64ptrc used);

// Renders one table: T, its column count, one character per column of alignment (- l c r), then m for
// a table holding a merge and n for one holding a nested table. Rows are separated by "/" and a row
// that carried w:tblHeader is prefixed with "=". A cell is its blocks between parentheses, prefixed by
// its span when it covers more than one column, by "v" when it starts a vertical merge and by "^" when
// it continues one.
static void CoalesceTable(cIR_DOCUMENTptr document, cIR_BLOCKptr block, chptrc dest, cui64 destBytes, ui64ptrc used) {
   cIR_TABLEptr table = IrTableAt(document, block->tableAt);
   char         head[IR_MAX_COLUMNS + 16u];
   ui64         at = 0;

   if(!table) {
      CoalesceAppend(dest, destBytes, used, "T?");
      return;
   }
   head[at++] = 'T';
   at += CoalesceNumber(head + at, table->columns);
   for(ui32 column = 0; column < table->columns; ++column) {
      cIR_ALIGN align = IrAlignOf(document, table, column);

      head[at++] = (align == IR_ALIGN_LEFT ? 'l' : (align == IR_ALIGN_CENTRE ? 'c' : (align == IR_ALIGN_RIGHT ? 'r' : '-')));
   }
   if(table->flags & IR_TABLE_MERGED) head[at++] = 'm';
   if(table->flags & IR_TABLE_NESTED) head[at++] = 'n';
   head[at++] = '{';
   head[at]   = 0;
   CoalesceAppend(dest, destBytes, used, head);

   ui32 row   = table->firstRow;
   bool first = true;

   while(row != IR_NO_INDEX) {
      cIR_ROWptr record = IrRowAt(document, row);

      if(!record) break;
      if(!first) CoalesceAppend(dest, destBytes, used, "/");
      first = false;
      if(record->flags & IR_ROW_HEADER) CoalesceAppend(dest, destBytes, used, "=");

      ui32 cell = record->firstCell;

      while(cell != IR_NO_INDEX) {
         cIR_CELLptr one = IrCellAt(document, cell);

         if(!one) break;
         CoalesceAppend(dest, destBytes, used, "(");
         if(one->span > 1u) {
            char  width[12];
            cui64 length = CoalesceNumber(width, one->span);

            width[length]      = ':';
            width[length + 1u] = 0;
            CoalesceAppend(dest, destBytes, used, width);
         }
         if(one->flags & IR_CELL_VRESTART) CoalesceAppend(dest, destBytes, used, "v");
         if(one->flags & IR_CELL_VMERGED) CoalesceAppend(dest, destBytes, used, "^");
         CoalesceRange(document, one->blockAt, one->blockAt + one->blockCount, dest, destBytes, used);
         CoalesceAppend(dest, destBytes, used, ")");
         cell = one->nextCell;
      }
      row = record->nextRow;
   }
   CoalesceAppend(dest, destBytes, used, "}");
}

static void CoalesceRange(cIR_DOCUMENTptr document, cui32 from, cui32 to, chptrc dest, cui64 destBytes, ui64ptrc used) {
   for(ui32 index = from; index < to; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;
      if(block->kind == IR_BLOCK_TABLE) {
         CoalesceNote(document, block, dest, destBytes, used);
         CoalesceTable(document, block, dest, destBytes, used);

         cIR_TABLEptr table = IrTableAt(document, block->tableAt);

         if(table && table->blockEnd > index) index = table->blockEnd - 1u;
         continue;
      }

      char head[24];
      CoalesceNote(document, block, dest, destBytes, used);

      ui64 at = CoalesceList(block, head);

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
      CoalesceAppend(dest, destBytes, used, head);
      for(ui32 span = 0; span < block->spanCount; ++span) {
         cIR_SPANptr one = IrSpanAt(document, block->spanAt + span);

         if(one->kind == IR_SPAN_BREAK) {
            CoalesceAppend(dest, destBytes, used, "|");
            continue;
         }
         if(one->kind == IR_SPAN_LINK_END) {
            CoalesceAppend(dest, destBytes, used, "L)");
            continue;
         }
         if(one->kind == IR_SPAN_NOTE) {
            CoalesceAppend(dest, destBytes, used, (one->flags & IR_SPAN_FLAG_END ? "E" : "F"));
            if(one->flags & IR_SPAN_FLAG_MUTE) CoalesceAppend(dest, destBytes, used, "-");
            CoalesceAppend(dest, destBytes, used, "(");
            for(ui32 byte = 0; byte < one->destBytes && *used + 1u < destBytes; ++byte) dest[(*used)++] = IrDest(document, one->destAt)[byte];
            dest[*used] = 0;
            CoalesceAppend(dest, destBytes, used, ")");
            continue;
         }
         if(one->kind == IR_SPAN_LINK_START || one->kind == IR_SPAN_IMAGE || one->kind == IR_SPAN_ANCHOR) {
            CoalesceAppend(dest, destBytes, used, (one->kind == IR_SPAN_IMAGE ? "I" : (one->kind == IR_SPAN_ANCHOR ? "N" : "L")));
            if(one->flags & IR_SPAN_FLAG_MUTE) CoalesceAppend(dest, destBytes, used, "-");
            CoalesceAppend(dest, destBytes, used, "(");
            for(ui32 byte = 0; byte < one->destBytes && *used + 1u < destBytes; ++byte) {
               dest[(*used)++] = IrDest(document, one->destAt)[byte];
            }
            dest[*used] = 0;
            CoalesceAppend(dest, destBytes, used, ")");
            if(one->kind != IR_SPAN_IMAGE) continue;
         }
         if(one->fmt & IR_FMT_BOLD) CoalesceAppend(dest, destBytes, used, "b");
         if(one->fmt & IR_FMT_ITALIC) CoalesceAppend(dest, destBytes, used, "i");
         if(one->fmt & IR_FMT_STRIKE) CoalesceAppend(dest, destBytes, used, "s");
         if(one->fmt & IR_FMT_SUPER) CoalesceAppend(dest, destBytes, used, "^");
         if(one->fmt & IR_FMT_SUB) CoalesceAppend(dest, destBytes, used, "v");
         if(one->fmt & IR_FMT_CODE) CoalesceAppend(dest, destBytes, used, "c");
         CoalesceAppend(dest, destBytes, used, "[");
         for(ui32 byte = 0; byte < one->textBytes && *used + 1u < destBytes; ++byte) {
            dest[(*used)++] = IrText(document, one->textAt)[byte];
         }
         dest[*used] = 0;
         CoalesceAppend(dest, destBytes, used, "]");
      }
      CoalesceAppend(dest, destBytes, used, "}");
   }
}

// Renders a whole coalesced document, in the notation above.
static void CoalesceTrace(cIR_DOCUMENTptr document, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   dest[0] = 0;
   CoalesceRange(document, 0, IrBlockCount(document), dest, destBytes, &used);
}

// Walks one body, coalesces it, and compares the trace with a literal. With resolved set the note
// references are then labelled -- there are no notes, so every one of them is muted -- and the body is
// coalesced a second time, which is the order Convert.cpp runs the two passes in.
static cbool CoalescesWith(cchptr styleBody, cchptr body, cchptr wanted, cbool resolved) {
   char part[8192];
   char trace[2048];
   ui64 used = 0;

   part[0] = 0;
   CoalesceAppend(part, sizeof(part), &used, COALESCE_HEAD);
   CoalesceAppend(part, sizeof(part), &used, body);
   CoalesceAppend(part, sizeof(part), &used, COALESCE_TAIL);

   STYLE_MODEL styles;
   IR_DOCUMENT document;

   StyleOpen(&styles);
   if(styleBody) {
      char stylePart[4096];
      ui64 styleUsed = 0;

      stylePart[0] = 0;
      CoalesceAppend(stylePart, sizeof(stylePart), &styleUsed, STYLES_HEAD);
      CoalesceAppend(stylePart, sizeof(stylePart), &styleUsed, styleBody);
      CoalesceAppend(stylePart, sizeof(stylePart), &styleUsed, STYLES_TAIL);
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

   bool ready = status.result == WALK_OK && RunCoalesce(&document);

   if(ready && resolved) ready = LinkResolveNotes(&document) && RunCoalesce(&document);
   if(!ready) {
      IrClose(&document);
      NumClose(&numbering);
      StyleClose(&styles);
      return false;
   }
   CoalesceTrace(&document, trace, sizeof(trace));
   IrClose(&document);
   NumClose(&numbering);
   StyleClose(&styles);

   ui64 index = 0;

   while(trace[index] && trace[index] == wanted[index]) ++index;
   return trace[index] == wanted[index];
}

// Walks one body with an optional styles part, and compares the coalesced trace with a literal.
static cbool CoalescesAs(cchptr styleBody, cchptr body, cchptr wanted) { return CoalescesWith(styleBody, body, wanted, false); }

// The same with no styles part, which is what most cases want.
static cbool Coalesces(cchptr body, cchptr wanted) { return CoalescesAs(nullptr, body, wanted); }

//== The suite

void TestRunCoalescer(void);

void TestRunCoalescer(void) {
   CheckGroup("RunCoalescer: merging adjacent runs of equal formatting");
   // CONVERSION_REFERENCE 5.1: Word splits a logical run mid-word at every spellcheck and revision
   // boundary, and a delimiter per run would emit "**Hel****lo**", which is not emphasis at all.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Hel</w:t></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>lo</w:t></w:r></w:p>",
                   "P{b[Hello]}"));
   // A proofErr, a bookmark and an accepted insertion all fall between runs and none of them is a
   // barrier: after the accept-all pass the first and the last are not there at all, and the bookmark
   // is a span the merge sees straight through. It comes out after the merged text rather than in the
   // middle of it, because the merge extends the span in front of it -- a link resolves an anchor to
   // the block it stands in, so which end of the paragraph it settles at costs nothing.
   CHECK(Coalesces("<w:p><w:proofErr w:type=\"spellStart\"/><w:r><w:t>a</w:t></w:r>"
                   "<w:bookmarkStart w:id=\"1\" w:name=\"x\"/><w:r><w:t>b</w:t></w:r>"
                   "<w:ins w:id=\"2\" w:author=\"A\"><w:r><w:t>c</w:t></w:r></w:ins></w:p>",
                   "P{[abc]N(x)}"));
   // Unequal formatting is not merged, which is the other half of the rule.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>b</w:t></w:r></w:p>", "P{b[a]i[b]}"));
   // A hard break is a barrier: the two sides are different lines and cannot be one span.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "P{b[a]|b[b]}"));
   // A run carrying properties and no text contributes nothing, and must not separate two that merge.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t/></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>b</w:t></w:r></w:p>",
                   "P{b[ab]}"));
   // The complex-script twins share a bit, so three runs that render identically become one span.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>A</w:t></w:r><w:r><w:rPr><w:b/><w:bCs/></w:rPr><w:t>B</w:t></w:r>"
                   "<w:r><w:rPr><w:bCs/></w:rPr><w:t>C</w:t></w:r></w:p>",
                   "P{b[ABC]}"));

   CheckGroup("RunCoalescer: what a merge sees through and what stops it");
   // An anchor is the one span kind a merge reads straight past, and CONVERSION_REFERENCE 5.1 is why:
   // Word writes _GoBack in the middle of a paragraph, between two fragments of one word, and a merge
   // that stopped there would emit "**Hel****lo**" exactly as the run fragmentation itself does.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Hel</w:t></w:r>"
                   "<w:bookmarkStart w:id=\"1\" w:name=\"_GoBack\"/>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>lo</w:t></w:r></w:p>",
                   "P{b[Hello]N(_GoBack)}"));
   // Several of them in a row is the shape that matters, and not only because a producer writes it:
   // the anchors pile up behind the span every later run merges into, so a pass that looked for its
   // merge target by stepping back over them would be quadratic in the paragraph's own length.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:bookmarkStart w:id=\"1\" w:name=\"p\"/>"
                   "<w:r><w:t>b</w:t></w:r><w:bookmarkStart w:id=\"2\" w:name=\"q\"/>"
                   "<w:r><w:t>c</w:t></w:r><w:bookmarkStart w:id=\"3\" w:name=\"r\"/>"
                   "<w:r><w:t>d</w:t></w:r></w:p>",
                   "P{[abcd]N(p)N(q)N(r)}"));
   // An image is a barrier and an anchor behind one does not reopen the merge across it.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r>"
                   "<w:r><w:pict><v:shape alt=\"i\"><v:imagedata r:id=\"rId2\"/></v:shape></w:pict></w:r>"
                   "<w:bookmarkStart w:id=\"1\" w:name=\"p\"/><w:r><w:t>b</w:t></w:r></w:p>",
                   "P{[a]I(rId2)[i]N(p)[b]}"));
   // A link is the opposite case and must stop one: text on either side of a bracket is not adjacent
   // in the output, and joining it would carry bytes across a boundary the reader can see.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r>"
                   "<w:hyperlink r:id=\"rId5\"><w:r><w:t>b</w:t></w:r></w:hyperlink>"
                   "<w:r><w:t>c</w:t></w:r></w:p>",
                   "P{[a]L(rId5)[b]L)[c]}"));
   // Runs *inside* one link still merge, which is the half of 5.1 the brackets do not touch.
   CHECK(Coalesces("<w:p><w:hyperlink r:id=\"rId5\"><w:r><w:t>a</w:t></w:r>"
                   "<w:r><w:t>b</w:t></w:r></w:hyperlink></w:p>",
                   "P{L(rId5)[ab]L)}"));
   // Whitespace still hoists out of a formatted span inside a link, so the delimiter can parse.
   CHECK(Coalesces("<w:p><w:hyperlink r:id=\"rId5\"><w:r><w:rPr><w:b/></w:rPr>"
                   "<w:t xml:space=\"preserve\">a </w:t></w:r><w:r><w:t>b</w:t></w:r></w:hyperlink></w:p>",
                   "P{L(rId5)b[a][ b]L)}"));

   // A field needs no barrier of its own. A plain field's cached result is ordinary text, which Word
   // splits from the text around it as readily as it splits any run; a HYPERLINK field's result is
   // bounded by the same link markers a w:hyperlink's is.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">page </w:t></w:r>"
                   "<w:r><w:fldChar w:fldCharType=\"begin\"/></w:r><w:r><w:instrText> PAGE </w:instrText></w:r>"
                   "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>7</w:t></w:r>"
                   "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>",
                   "P{b[page 7]}"));
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
                   "<w:r><w:instrText> HYPERLINK \"u\" </w:instrText></w:r><w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
                   "<w:r><w:t>b</w:t></w:r><w:r><w:fldChar w:fldCharType=\"end\"/></w:r><w:r><w:t>c</w:t></w:r></w:p>",
                   "P{[a]L(u)[b]L)[c]}"));

   // M10's note reference is a marker like a link's brackets: the text either side of one is not
   // adjacent in the output, so it stops a merge -- until LinkResolveNotes mutes one whose note the
   // document does not hold, after which it emits nothing and the two sides meet.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>b</w:t></w:r></w:p>",
                   "P{b[a]F(1)b[b]}"));
   CHECK(CoalescesWith(nullptr,
                       "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r><w:r><w:footnoteReference w:id=\"1\"/></w:r>"
                       "<w:r><w:rPr><w:b/></w:rPr><w:t>b</w:t></w:r></w:p>",
                       "P{b[ab]F-(1)}", true));

   CheckGroup("RunCoalescer: hoisting whitespace out of a formatted span");
   // CONVERSION_REFERENCE 5.3: "**bold **text" does not parse, so the space moves outside the span.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">bold </w:t></w:r>"
                   "<w:r><w:t>text</w:t></w:r></w:p>",
                   "P{b[bold][ text]}"));
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\"> b</w:t></w:r></w:p>", "P{[a ]b[b]}"));
   // A span that is nothing but whitespace keeps its bytes and loses its formatting, which is 5.5.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t xml:space=\"preserve\"> </w:t></w:r>"
                   "<w:r><w:t>b</w:t></w:r></w:p>",
                   "P{[a b]}"));
   // The non-breaking space hoists like an ASCII one: CommonMark counts every Zs for flanking, so a
   // closing delimiter behind one may not parse. Note the asymmetry with IrEndBlock, where it is content.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">n&#160;</w:t></w:r>"
                   "<w:r><w:t>a</w:t></w:r></w:p>",
                   "P{b[n][\xC2\xA0"
                   "a]}"));
   // A tab hoists too, and both ends of one span hoist at once.
   CHECK(Coalesces("<w:p><w:r><w:t>x</w:t></w:r><w:r><w:rPr><w:b/></w:rPr>"
                   "<w:t xml:space=\"preserve\">&#9;b&#9;</w:t></w:r><w:r><w:t>y</w:t></w:r></w:p>",
                   "P{[x\t]b[b][\ty]}"));
   // An unformatted span is never hoisted from: there is no delimiter for its whitespace to escape.
   CHECK(Coalesces("<w:p><w:r><w:t xml:space=\"preserve\"> a </w:t></w:r></w:p>", "P{[ a ]}"));
   // The set is every Zs, not the ASCII pair and U+00A0 alone, because CommonMark's flanking rules do
   // not distinguish them: a delimiter written hard against an EN SPACE parses no better than one
   // against an ordinary space. U+2002 is one Insert-Symbol away in Word and U+3000 is what a CJK
   // keyboard's space bar produces, so neither is exotic.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr>"
                   "<w:t xml:space=\"preserve\">&#8194;bold</w:t></w:r></w:p>",
                   "P{[a\xE2\x80\x82]b[bold]}"));
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">bold&#8194;</w:t></w:r>"
                   "<w:r><w:t>c</w:t></w:r></w:p>",
                   "P{b[bold][\xE2\x80\x82"
                   "c]}"));
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:i/></w:rPr>"
                   "<w:t xml:space=\"preserve\">&#12288;em&#8239;</w:t></w:r></w:p>",
                   "P{[a\xE3\x80\x80]i[em][\xE2\x80\xAF]}"));
   // U+200B is deliberately not in the set: it is Cf rather than Zs, and CommonMark does not count it.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>b&#8203;</w:t></w:r></w:p>", "P{b[b\xE2\x80\x8B]}"));

   CheckGroup("RunCoalescer: merging happens before hoisting");
   // This is the whole reason the two passes are ordered. Merged first, the pair is one bold span with
   // no whitespace at either end; hoisted first, it would come apart into "**one** **two**".
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">one </w:t></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>two</w:t></w:r></w:p>",
                   "P{b[one two]}"));
   // And the whitespace hoisted out of two neighbours becomes one span again rather than two.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">x </w:t></w:r>"
                   "<w:r><w:rPr><w:i/></w:rPr><w:t xml:space=\"preserve\"> y</w:t></w:r></w:p>",
                   "P{b[x][  ]i[y]}"));

   CheckGroup("RunCoalescer: the block kinds it must leave alone");
   // Nothing is hoisted inside a fenced block: its whitespace is the indentation of the code. The span
   // has to *carry* formatting for this to assert anything -- an unformatted one returns before the
   // guard is read, so the case would pass with the guard deleted.
   CHECK(CoalescesAs(STYLE_CODE,
                     "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"
                     "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr>"
                     "<w:t xml:space=\"preserve\">   indented   </w:t></w:r></w:p>",
                     "C{c[   indented   ]}"));
   CHECK(CoalescesAs(STYLE_CODE,
                     "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"
                     "<w:r><w:t xml:space=\"preserve\">   indented   </w:t></w:r></w:p>",
                     "C{[   indented   ]}"));
   // A rule carries no spans at all, and an empty code paragraph is kept although it holds none either.
   CHECK(Coalesces("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "R{}"));
   CHECK(CoalescesAs(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>", "C{}"));
   // Two breaks with nothing between them are one everywhere but in a fence, where each is a blank line:
   // the emitter already writes them that way, and dropping the second here is what keeps a run of them
   // from costing a record apiece in the rebuild. A marker between them is something between them.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t><w:br/><w:br/><w:br/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   CHECK(Coalesces("<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:t>a</w:t><w:br/><w:br/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t><w:br/></w:r>"
                   "<w:hyperlink w:anchor=\"x\"><w:r><w:br/><w:t>b</w:t></w:r></w:hyperlink></w:p>",
                   "P{[a]|L(#x)|[b]L)}"));
   CHECK(CoalescesAs(STYLE_CODE,
                     "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"
                     "<w:r><w:t>a</w:t><w:br/><w:br/><w:t>b</w:t></w:r></w:p>",
                     "C{[a]||[b]}"));
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr>"
                   "<w:t>a</w:t><w:br/><w:br/><w:t>b</w:t></w:r></w:p>",
                   "C{c[a]||c[b]}"));
   // A quote is an ordinary paragraph as far as this pass is concerned, prefix and all.
   CHECK(CoalescesAs(STYLE_QUOTE,
                     "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr>"
                     "<w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">q </w:t></w:r>"
                     "<w:r><w:t>r</w:t></w:r></w:p>",
                     "Q{b[q][ r]}"));

   CheckGroup("RunCoalescer: a list item is an ordinary paragraph to this pass");
   // Its leading whitespace is *not* the indentation of code -- the indentation is the emitter's, built
   // from the marker -- so a list item hoists exactly as a paragraph does and a fence still does not.
   CHECK(CoalescesAs(nullptr,
                     "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                     "<w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">bold </w:t></w:r>"
                     "<w:r><w:t>rest</w:t></w:r></w:p>",
                     "[0#4]P{b[bold][ rest]}"));
   // Two runs inside one item merge, and the per-block reset keeps a merge out of the next one.
   CHECK(CoalescesAs(nullptr,
                     "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                     "<w:r><w:t>Hel</w:t></w:r><w:r><w:t>lo</w:t></w:r></w:p>"
                     "<w:p><w:pPr><w:numPr><w:numId w:val=\"4\"/></w:numPr></w:pPr>"
                     "<w:r><w:t>there</w:t></w:r></w:p>",
                     "[0#4]P{[Hello]}[0#4]P{[there]}"));

   CheckGroup("RunCoalescer: code spans and the two halves of row 11");
   // Row 11 drops bold and italic from a code run, and the bits are cleared in the *walker* so that two
   // runs which come out as the same code span merge here. Left set, their two backtick delimiters meet
   // and a renderer reads the pair as one span with backticks in it. Nothing else asserts the rule: it
   // is invisible at emission, so its only observable effect is this merge.
   CHECK(CoalescesAs(STYLE_SPAN,
                     "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/><w:b/></w:rPr><w:t>co</w:t></w:r>"
                     "<w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>de</w:t></w:r></w:p>",
                     "P{c[code]}"));
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/><w:i/></w:rPr><w:t>co</w:t></w:r>"
                   "<w:r><w:rPr><w:rFonts w:ascii=\"Menlo\"/></w:rPr><w:t>de</w:t></w:r>"
                   "<w:r><w:t> and prose</w:t></w:r></w:p>",
                   "P{c[code][ and prose]}"));
   // A run that is code because of its character style and one that is code because of its font carry
   // the same bit, so two of them side by side are one code span rather than two.
   CHECK(CoalescesAs(STYLE_SPAN,
                     "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>co</w:t></w:r>"
                     "<w:r><w:rPr><w:rFonts w:ascii=\"Menlo\"/></w:rPr><w:t>de</w:t></w:r></w:p>",
                     "P{c[code]}"));
   // Whitespace hoists out of a code span too: CommonMark strips one pad space of its own, so leaving
   // it inside would change the content depending on what stood at the other end.
   CHECK(CoalescesAs(STYLE_SPAN,
                     "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t xml:space=\"preserve\">run </w:t></w:r>"
                     "<w:r><w:t>after</w:t></w:r></w:p>",
                     "P{c[run][ after]}"));
   // Every run monospace makes the paragraph a fenced block, and one run that is not leaves it a
   // paragraph holding a code span -- which is CONVERSION_REFERENCE row 12 against row 11.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a();</w:t></w:r></w:p>", "C{c[a();]}"));
   CHECK(Coalesces("<w:p><w:r><w:t xml:space=\"preserve\">x </w:t></w:r>"
                   "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a();</w:t></w:r></w:p>",
                   "P{[x ]c[a();]}"));

   CheckGroup("RunCoalescer: a cell's blocks are ordinary blocks to this pass");
   // A cell's content is walked where it stands, so its paragraphs are blocks in the same flat array
   // and this pass never learns that a table exists. What that buys is the whole of M9's coalescing:
   // Word fragments a run inside a cell exactly as it does outside one, and the merge is the same.
   CHECK(Coalesces("<w:tbl><w:tr><w:tc><w:p>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>Hel</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>lo</w:t></w:r>"
                   "</w:p></w:tc></w:tr></w:tbl>",
                   "T1-{(P{b[Hello]})}"));
   // And the hoist: a trailing space inside a bold run in a cell moves outside the delimiters, or
   // the closing "**" could not parse where it stands.
   CHECK(Coalesces("<w:tbl><w:tr><w:tc><w:p>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">bold </w:t></w:r><w:r><w:t>after</w:t></w:r>"
                   "</w:p></w:tc></w:tr></w:tbl>",
                   "T1-{(P{b[bold][ after]})}"));
   // A nested table's blocks are the same again, one level further in.
   CHECK(Coalesces("<w:tbl><w:tr><w:tc><w:tbl><w:tr><w:tc><w:p>"
                   "<w:r><w:rPr><w:i/></w:rPr><w:t>in</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>ner</w:t></w:r>"
                   "</w:p></w:tc></w:tr></w:tbl></w:tc></w:tr></w:tbl>",
                   "T1-n{(T1-{(P{i[inner]})})}"));
}
