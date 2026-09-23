/*
 * File: Ir.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-23
 * Description: The intermediate representation's arena: growth, span appends and empty-block trimming.
 * To Do: 1) Size the first allocation from the part's own byte count, once the walker knows it.
 *        2) Release the arena back to the allocator between documents when M13 reuses a worker.
 *        3) Compact the arena of the destinations IrSetDest leaves behind, if a document is ever found
 *           whose link count makes the waste worth a pass over every offset.
 * Dependencies: BuildGuards.h, Ir.h, typedefs.h, memory management.h, windows.h
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

//-- Limits

// The arena is addressed by a ui32 offset, so it may not grow past what one can name. The container
// layer's own caps stop a document long before this, which is why reaching it is a refusal and not a
// resize: a part that produced four gigabytes of text is not a document anyone meant to convert.
constexpr cui64 IR_MAX_HEAP_BYTES = 0xFFFFFFFFu;

//-- Growable storage

// Grows a block to hold at least the requested number of elements, doubling so that filling one costs
// amortised constant time. Indices survive a move, which is why every reference here is one.
static cbool IrReserve(ptrptrc block, ui64ptrc capacity, cui64 needed, cui64 unit) {
   if(needed <= *capacity) return true;

   ui64 grown = (*capacity ? *capacity : 64u);

   while(grown < needed) grown *= 2u;

   ptr fresh = amalloc(grown * unit, 32u);

   if(!fresh) return false;
   if(*block) Copy(*block, fresh, *capacity * unit);
   mdealloc(*block);
   *block    = fresh;
   *capacity = grown;
   return true;
}

// Whether a byte is ASCII whitespace, which is what decides that a paragraph holds nothing. A
// non-breaking space is deliberately not in the set: it is content, per CONVERSION_REFERENCE row 35.
static cbool IrIsBlank(cchar byte) { return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n'; }

// Whether a range of spans holds anything a reader would see. An anchor does not count here: it is a
// link target rather than something on the page, and the two questions come apart exactly once --
// mapping row 25's horizontal rule, which asks whether a paragraph "came to nothing" and must not be
// answered by a bookmark. IrRangeHasContent below is this plus the anchors.
static cbool IrRangeHasInk(cIR_DOCUMENTptr document, cui32 first, cui32 last) {
   for(ui32 index = first; index < last; ++index) {
      cIR_SPANptr span = document->spans + index;

      if(span->kind == IR_SPAN_IMAGE) return true;
      // A note reference puts its label on the page, so it is ink -- until LinkResolveNotes mutes one
      // whose note the document does not hold, after which it emits nothing and is nothing.
      if(span->kind == IR_SPAN_NOTE && !(span->flags & IR_SPAN_FLAG_MUTE)) return true;
      if(span->kind != IR_SPAN_TEXT) continue;
      for(ui32 at = 0; at < span->textBytes; ++at) {
         if(!IrIsBlank(document->heap[span->textAt + at])) return true;
      }
   }
   return false;
}

// Whether a range of spans holds anything worth keeping a block for. That is the ink above, plus any
// anchor something still points at: a paragraph holding one bookmark is a link target and has to
// survive to carry it. A link's own markers count for nothing, because a link with no text between
// them is nothing at all (CONVERSION_REFERENCE 5.6). Asked twice on a block's life: once by
// IrEndBlock, where no anchor has been muted yet, and once by IrDropEmptyBlocks, where the muted ones
// no longer count.
static cbool IrRangeHasContent(cIR_DOCUMENTptr document, cui32 first, cui32 last) {
   if(IrRangeHasInk(document, first, last)) return true;
   for(ui32 index = first; index < last; ++index) {
      cIR_SPANptr span = document->spans + index;

      if(span->kind == IR_SPAN_ANCHOR && !(span->flags & IR_SPAN_FLAG_MUTE)) return true;
   }
   return false;
}

// Appends bytes to the end of one arena and reports where they landed, or -1 when they will not fit.
//
// The source may be inside the very arena it is being appended to: LinkResolve copies a heading's
// slug onto the destination of the anchor that reaches it, and both live here. Growing the arena
// frees the block that pointer is in, so a source that lies inside it is remembered as an offset and
// re-derived afterwards. AddressSanitizer found the one caller that did this; the defence is here
// rather than there because every later caller would have to remember the same thing.
static csi64 IrStore(IR_DOCUMENTptrc document, chptrptrc arena, ui64ptrc capacity, ui64ptrc used, cchptr bytes, cui64 byteCount) {
   if(*used + byteCount > IR_MAX_HEAP_BYTES) {
      document->failed = true;
      return -1;
   }

   cbool inside = (*arena && bytes >= *arena && bytes < *arena + *used);
   cui64 source = (inside ? ui64(bytes - *arena) : 0);

   if(!IrReserve((ptrptrc)arena, capacity, *used + byteCount, 1u)) {
      document->failed = true;
      return -1;
   }

   csi64 at = si64(*used);

   // The two ranges cannot overlap: an interior source ends at or before the end in use, and the
   // destination begins there.
   if(byteCount) Copy((inside ? *arena + source : bytes), *arena + *used, byteCount);
   *used += byteCount;
   return at;
}

// The same, into the arena a span's text lives in.
static csi64 IrStoreText(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount) {
   return IrStore(document, &document->heap, &document->heapCapacity, &document->heapUsed, bytes, byteCount);
}

// The same, into the arena a span's destination lives in. Unlike its twin this is also an entry point:
// a pass that assembles a destination out of pieces needs somewhere to put one that is not a span's.
csi64 IrStoreDest(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount) {
   return IrStore(document, &document->dest, &document->destCapacity, &document->destUsed, bytes, byteCount);
}

//== Entry points

void IrOpen(IR_DOCUMENTptrc document) {
   mzero(document, sizeof(IR_DOCUMENT));
   document->note = -1;
}

void IrClose(IR_DOCUMENTptrc document) {
   mdealloc(document->blocks);
   mdealloc(document->spans);
   mdealloc(document->tables);
   mdealloc(document->rows);
   mdealloc(document->cells);
   mdealloc(document->notes);
   mdealloc(document->heap);
   mdealloc(document->dest);
   mdealloc(document->align);
   IrOpen(document);
}

// Where the document currently ends, in every one of the eight counts a rewind has to put back.
static cIR_MARK IrHere(cIR_DOCUMENTptr document, csi32 block) {
   IR_MARK mark;

   mark.block   = block;
   mark.spanAt  = document->spanCount;
   mark.tableAt = document->tableCount;
   mark.rowAt   = document->rowCount;
   mark.cellAt  = document->cellCount;
   mark.heapAt  = document->heapUsed;
   mark.destAt  = document->destUsed;
   mark.alignAt = document->alignUsed;
   return mark;
}

cIR_MARK IrBeginBlock(IR_DOCUMENTptrc document, cIR_BLOCK_KIND kind, cui8 headingLevel) {
   IR_MARK mark = IrHere(document, -1);

   if(!IrReserve((ptrptrc)&document->blocks, &document->blockCapacity, ui64(document->blockCount) + 1u, sizeof(IR_BLOCK))) {
      document->failed = true;
      return mark;
   }

   IR_BLOCKptr block = document->blocks + document->blockCount;

   // Every field is written here and nothing zeroes a record: IrReserve hands back whatever amalloc
   // did, so a field this misses is indeterminate for every block in the document -- which neither
   // sanitizer nor MSVC's /RTCu reaches, both of them being about the stack.
   block->spanAt       = document->spanCount;
   block->spanCount    = 0;
   block->listNumId    = -1;
   block->listNumber   = 0;
   block->tableAt      = -1;
   block->note         = document->note;
   block->kind         = kind;
   block->headingLevel = headingLevel;
   block->listLevel    = 0;
   block->listFlags    = IR_LIST_NONE;
   mark.block          = si32(document->blockCount);
   ++document->blockCount;
   return mark;
}

csi32 IrBeginTable(IR_DOCUMENTptrc document, cIR_MARK mark) {
   if(mark.block < 0 || ui32(mark.block) >= document->blockCount) return -1;
   if(!IrReserve((ptrptrc)&document->tables, &document->tableCapacity, ui64(document->tableCount) + 1u, sizeof(IR_TABLE))) {
      document->failed = true;
      return -1;
   }

   IR_TABLEptr table = document->tables + document->tableCount;

   table->firstRow                      = IR_NO_INDEX;
   table->blockAt                       = ui32(mark.block);
   table->blockEnd                      = ui32(mark.block) + 1u;
   table->columns                       = 1u;
   table->alignAt                       = IR_ALIGN_ABSENT;
   table->flags                         = IR_TABLE_NONE;
   document->blocks[mark.block].tableAt = si32(document->tableCount);
   ++document->tableCount;
   return si32(document->tableCount) - 1;
}

// Fills one table's align arena from the first row it has, once the column count is settled. It is
// stored only where some column really is aligned, which is the ordinary table's case both ways round:
// a producer that set none pays nothing, and one that set some pays a byte a column.
static void IrStoreAligns(IR_DOCUMENTptrc document, IR_TABLEptrc table) {
   cIR_ROWptr row = IrRowAt(document, table->firstRow);
   bool       any = false;

   if(!row) return;
   for(ui32 at = row->firstCell; at != IR_NO_INDEX;) {
      cIR_CELLptr cell = IrCellAt(document, at);

      if(!cell) break;
      if(cell->align != ui8(IR_ALIGN_NONE) && cell->column < table->columns) any = true;
      at = cell->nextCell;
   }
   if(!any) return;
   if(!IrReserve((ptrptrc)&document->align, &document->alignCapacity, document->alignUsed + table->columns, 1u)) {
      document->failed = true;
      return;
   }
   table->alignAt = ui32(document->alignUsed);
   for(ui32 column = 0; column < table->columns; ++column) document->align[table->alignAt + column] = ui8(IR_ALIGN_NONE);
   for(ui32 at = row->firstCell; at != IR_NO_INDEX;) {
      cIR_CELLptr cell = IrCellAt(document, at);

      if(!cell) break;
      // A cell spanning several columns aligns all of them, which is the only reading available when
      // one cell speaks for two.
      for(ui32 column = cell->column; column < cell->column + cell->span && column < table->columns; ++column) {
         document->align[table->alignAt + column] = cell->align;
      }
      at = cell->nextCell;
   }
   document->alignUsed += table->columns;
}

void IrEndTable(IR_DOCUMENTptrc document, csi32 table, csi32 lastRow, cui32 grid) {
   if(table < 0 || ui32(table) >= document->tableCount) return;

   IR_TABLEptr owner = document->tables + table;

   owner->blockEnd = document->blockCount;
   // The terminator is written from the caller's own tail rather than from whatever was appended last,
   // which is the whole of what a rewind inside a table costs: a discarded row is simply never named.
   if(lastRow >= 0 && ui32(lastRow) < document->rowCount) document->rows[lastRow].nextRow = IR_NO_INDEX;
   else if(lastRow < 0) owner->firstRow = IR_NO_INDEX;

   // Everything else the table says about itself is derived from the chains just terminated, and never
   // accumulated as the walk went: a cell an mc:Fallback replaced must leave behind no column, no
   // alignment and no merge. See the note on this function in Ir.h.
   ui32 columns = (grid > IR_MAX_COLUMNS ? IR_MAX_COLUMNS : grid);

   for(ui32 index = owner->firstRow; index != IR_NO_INDEX;) {
      cIR_ROWptr row = IrRowAt(document, index);

      if(!row) break;
      for(ui32 at = row->firstCell; at != IR_NO_INDEX;) {
         cIR_CELLptr cell  = IrCellAt(document, at);
         ui32        reach = 0;

         if(!cell) break;
         reach = cell->column + cell->span;
         if(reach > columns) columns = (reach > IR_MAX_COLUMNS ? IR_MAX_COLUMNS : reach);
         if(cell->span > 1u || (cell->flags & (IR_CELL_VRESTART | IR_CELL_VMERGED))) owner->flags |= IR_TABLE_MERGED;
         for(ui32 step = 0; step < cell->blockCount; ++step) {
            cIR_BLOCKptr held = IrBlockAt(document, cell->blockAt + step);

            if(held && held->kind == IR_BLOCK_TABLE) {
               owner->flags |= IR_TABLE_NESTED;
               break;
            }
         }
         at = cell->nextCell;
      }
      index = row->nextRow;
   }
   owner->columns = (columns < 1u ? 1u : columns);
   IrStoreAligns(document, owner);
}

csi32 IrBeginRow(IR_DOCUMENTptrc document, csi32 table, csi32 after, cbool header) {
   if(table < 0 || ui32(table) >= document->tableCount) return -1;
   if(!IrReserve((ptrptrc)&document->rows, &document->rowCapacity, ui64(document->rowCount) + 1u, sizeof(IR_ROW))) {
      document->failed = true;
      return -1;
   }

   cui32     fresh = document->rowCount;
   IR_ROWptr row   = document->rows + fresh;

   row->firstCell = IR_NO_INDEX;
   row->nextRow   = IR_NO_INDEX;
   row->flags     = (header ? IR_ROW_HEADER : IR_ROW_NONE);
   if(after >= 0 && ui32(after) < fresh) document->rows[after].nextRow = fresh;
   else document->tables[table].firstRow = fresh;
   ++document->rowCount;
   return si32(fresh);
}

void IrEndRow(IR_DOCUMENTptrc document, csi32 row, csi32 lastCell) {
   if(row < 0 || ui32(row) >= document->rowCount) return;
   if(lastCell >= 0 && ui32(lastCell) < document->cellCount) document->cells[lastCell].nextCell = IR_NO_INDEX;
   else if(lastCell < 0) document->rows[row].firstCell = IR_NO_INDEX;
}

csi32 IrBeginCell(IR_DOCUMENTptrc document, csi32 row, csi32 after, cui32 span, cui8 flags) {
   if(row < 0 || ui32(row) >= document->rowCount) return -1;
   if(!IrReserve((ptrptrc)&document->cells, &document->cellCapacity, ui64(document->cellCount) + 1u, sizeof(IR_CELL))) {
      document->failed = true;
      return -1;
   }

   cui32      fresh  = document->cellCount;
   cui32      wanted = (span < 1u ? 1u : (span > IR_MAX_COLUMNS ? IR_MAX_COLUMNS : span));
   cbool      linked = (after >= 0 && ui32(after) < fresh);
   cui32      column = (linked ? document->cells[after].column + document->cells[after].span : 0u);
   IR_CELLptr cell   = document->cells + fresh;

   // The column is left where the arithmetic puts it rather than clamped, so that two cells of one row
   // can never claim the same one. A cell past IR_MAX_COLUMNS is simply outside the grid the emitter
   // writes, which is the one place this build stops honouring "never drop a column" -- and a table
   // 256 columns wide has stopped being readable on any page long before that.
   cell->blockAt    = document->blockCount;
   cell->blockCount = 0;
   cell->column     = column;
   cell->span       = wanted;
   cell->nextCell   = IR_NO_INDEX;
   cell->flags      = flags;
   cell->align      = ui8(IR_ALIGN_NONE);
   if(linked) document->cells[after].nextCell = fresh;
   else document->rows[row].firstCell = fresh;
   ++document->cellCount;
   return si32(fresh);
}

void IrEndCell(IR_DOCUMENTptrc document, csi32 cell, cui32 blockAt, cIR_ALIGN align) {
   if(cell < 0 || ui32(cell) >= document->cellCount) return;

   IR_CELLptr owner = document->cells + cell;

   owner->blockAt    = blockAt;
   owner->blockCount = (document->blockCount > blockAt ? document->blockCount - blockAt : 0);
   owner->align      = ui8(align);
}

csi32 IrBeginNote(IR_DOCUMENTptrc document, cIR_NOTE_KIND kind, csi32 id, csi32 part) {
   if(!IrReserve((ptrptrc)&document->notes, &document->noteCapacity, ui64(document->noteCount) + 1u, sizeof(IR_NOTE))) {
      document->failed = true;
      return -1;
   }

   IR_NOTEptr note = document->notes + document->noteCount;

   note->id       = id;
   note->part     = part;
   note->number   = 0;
   note->kind     = kind;
   document->note = si32(document->noteCount);
   ++document->noteCount;
   return document->note;
}

void IrEndNote(IR_DOCUMENTptrc document) { document->note = -1; }

cui32 IrNoteCount(cIR_DOCUMENTptr document) { return document->noteCount; }

cIR_NOTEptr IrNoteAt(cIR_DOCUMENTptr document, csi32 index) {
   if(index < 0 || ui32(index) >= document->noteCount) return nullptr;
   return document->notes + index;
}

IR_NOTEptr IrNoteMutable(IR_DOCUMENTptrc document, csi32 index) {
   if(index < 0 || ui32(index) >= document->noteCount) return nullptr;
   return document->notes + index;
}

cIR_TABLEptr IrTableAt(cIR_DOCUMENTptr document, csi32 index) {
   if(index < 0 || ui32(index) >= document->tableCount) return nullptr;
   return document->tables + index;
}

cIR_ROWptr IrRowAt(cIR_DOCUMENTptr document, cui32 index) {
   if(index >= document->rowCount) return nullptr;
   return document->rows + index;
}

cIR_CELLptr IrCellAt(cIR_DOCUMENTptr document, cui32 index) {
   if(index >= document->cellCount) return nullptr;
   return document->cells + index;
}

cIR_ALIGN IrAlignOf(cIR_DOCUMENTptr document, cIR_TABLEptr table, cui32 column) {
   if(!table || table->alignAt == IR_ALIGN_ABSENT || column >= table->columns) return IR_ALIGN_NONE;

   cui8 stored = document->align[table->alignAt + column];

   return (stored < ui8(IR_ALIGN_COUNT) ? IR_ALIGN(stored) : IR_ALIGN_NONE);
}

void IrSetListRef(IR_DOCUMENTptrc document, cIR_MARK mark, csi32 numId, cui32 level) {
   if(mark.block < 0 || ui32(mark.block) >= document->blockCount) return;

   IR_BLOCKptr block = document->blocks + mark.block;

   block->listNumId = numId;
   block->listLevel = ui8(level < 9u ? level : 8u);
}

cbool IrEndBlock(IR_DOCUMENTptrc document, cIR_MARK mark) {
   if(mark.block < 0) return false;

   IR_BLOCKptr block = document->blocks + mark.block;
   ui32        first = mark.spanAt;
   ui32        last  = document->spanCount; // One past the block's last span

   // A break with nothing before or after it renders as a stray hard-break marker, so both ends are
   // trimmed before the block is judged -- except inside a fence, where a break is a real newline and
   // no marker is written for it, so the reason to trim one does not arise and trimming loses a line.
   if(block->kind != IR_BLOCK_CODE) {
      while(first < last && document->spans[first].kind == IR_SPAN_BREAK) ++first;
      while(last > first && document->spans[last - 1u].kind == IR_SPAN_BREAK) --last;
   }

   // A horizontal rule is an empty paragraph by construction (CONVERSION_REFERENCE row 25), so the
   // emptiness test below would throw away every one. It keeps no spans either: a rule emits none.
   // A table is the same shape for a different reason: its content is the blocks of its cells, so it
   // has no spans of its own and never will. A table that turned out to hold no rows is unwound by the
   // walker, which is the only place that is known.
   if(block->kind == IR_BLOCK_RULE || block->kind == IR_BLOCK_TABLE) {
      block->spanAt       = mark.spanAt;
      block->spanCount    = 0;
      document->spanCount = mark.spanAt;
      document->heapUsed  = mark.heapAt;
      document->destUsed  = mark.destAt;
      return true;
   }

   bool content = IrRangeHasContent(document, first, last);

   // An empty code paragraph is a blank line inside a fence, which is content of a kind an ordinary
   // paragraph has no equivalent for -- so it is kept here and the emitter trims one only where it
   // falls at the edge of a fence, which is where it would be a blank line before or after the code.
   // An empty *list item* is kept on the same reasoning and the same terms: it is a marker on a line of
   // its own, which CommonMark spells and Word draws, and the emitter trims one off either edge of a
   // list. Dropping it here instead would take the number of every item after it down by one.
   if(!content && (block->kind == IR_BLOCK_CODE || block->listNumId >= 0)) content = true;
   if(!content) {
      // Nothing worth emitting: unwind the block completely, arena and all, so that the next block's
      // text starts where this one's would have and an empty paragraph costs nothing at all.
      document->blockCount = ui32(mark.block);
      document->spanCount  = mark.spanAt;
      document->heapUsed   = mark.heapAt;
      document->destUsed   = mark.destAt;
      return false;
   }

   // Trimmed spans at the front are dropped by moving the block's start; at the back, by shortening it.
   // The arena keeps the trimmed bytes, which is a few bytes per block and not worth compacting for.
   block->spanAt       = first;
   block->spanCount    = last - first;
   document->spanCount = last;
   return true;
}

cbool IrAddSpan(IR_DOCUMENTptrc document, cIR_SPAN_KIND kind, cui32 fmt) {
   if(!IrReserve((ptrptrc)&document->spans, &document->spanCapacity, ui64(document->spanCount) + 1u, sizeof(IR_SPAN))) {
      document->failed = true;
      return false;
   }

   IR_SPANptr span = document->spans + document->spanCount;

   span->textAt    = ui32(document->heapUsed);
   span->textBytes = 0;
   span->destAt    = 0;
   span->destBytes = 0;
   span->fmt       = fmt;
   span->kind      = kind;
   span->flags     = IR_SPAN_FLAG_NONE;
   ++document->spanCount;
   return true;
}

cbool IrAppendText(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount) {
   if(!document->spanCount) return false;
   if(!byteCount) return true;

   csi64 at = IrStoreText(document, bytes, byteCount);

   if(at < 0) return false;

   IR_SPANptr span = document->spans + document->spanCount - 1u;

   // The bytes always land where the span's text already ends, because nothing else writes to the text
   // arena between one append and the next -- a destination goes to the other arena precisely so that
   // this stays true. Extending the range rather than repointing it is what keeps a run arriving as
   // several text tokens one span.
   span->textBytes += ui32(byteCount);
   return true;
}

cbool IrAppendDest(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount) {
   if(!document->spanCount) return false;
   if(!byteCount) return true;

   cbool first = (document->spans[document->spanCount - 1u].destBytes == 0);
   csi64 at    = IrStoreDest(document, bytes, byteCount);

   if(at < 0) return false;

   IR_SPANptr span = document->spans + document->spanCount - 1u;

   if(first) span->destAt = ui32(at);
   span->destBytes += ui32(byteCount);
   return true;
}

cbool IrSetDest(IR_DOCUMENTptrc document, cui32 index, cchptr bytes, cui64 byteCount) {
   if(index >= document->spanCount) return false;

   csi64 at = IrStoreDest(document, bytes, byteCount);

   if(at < 0) return false;

   IR_SPANptr span = document->spans + index;

   span->destAt    = ui32(at);
   span->destBytes = ui32(byteCount);
   return true;
}

cIR_MARK IrMark(cIR_DOCUMENTptr document) { return IrHere(document, si32(document->blockCount)); }

void IrRewind(IR_DOCUMENTptrc document, cIR_MARK mark) {
   if(mark.block < 0) return;
   if(ui32(mark.block) < document->blockCount) document->blockCount = ui32(mark.block);
   if(mark.spanAt < document->spanCount) document->spanCount = mark.spanAt;
   // A table's three record arrays go back with the blocks, or a discarded mc:Choice would leave rows
   // and cells behind that nothing names and the next table would read as its own.
   if(mark.tableAt < document->tableCount) document->tableCount = mark.tableAt;
   if(mark.rowAt < document->rowCount) document->rowCount = mark.rowAt;
   if(mark.cellAt < document->cellCount) document->cellCount = mark.cellAt;
   if(mark.heapAt < document->heapUsed) document->heapUsed = mark.heapAt;
   if(mark.destAt < document->destUsed) document->destUsed = mark.destAt;
   if(mark.alignAt < document->alignUsed) document->alignUsed = mark.alignAt;
}

cui32 IrBlockCount(cIR_DOCUMENTptr document) { return document->blockCount; }

cui32 IrSpanCount(cIR_DOCUMENTptr document) { return document->spanCount; }

cbool IrHasInk(cIR_DOCUMENTptr document, cui32 first, cui32 last) {
   cui32 stop = (last < document->spanCount ? last : document->spanCount);

   return (first < stop ? IrRangeHasInk(document, first, stop) : false);
}

cbool IrHasContent(cIR_DOCUMENTptr document, cui32 first, cui32 last) {
   cui32 stop = (last < document->spanCount ? last : document->spanCount);

   return (first < stop ? IrRangeHasContent(document, first, stop) : false);
}

cIR_BLOCKptr IrBlockAt(cIR_DOCUMENTptr document, cui32 index) { return (index < document->blockCount ? document->blocks + index : nullptr); }

cIR_SPANptr IrSpanAt(cIR_DOCUMENTptr document, cui32 index) { return (index < document->spanCount ? document->spans + index : nullptr); }

IR_BLOCKptr IrBlockMutable(IR_DOCUMENTptrc document, cui32 index) { return (index < document->blockCount ? document->blocks + index : nullptr); }

IR_SPANptr IrSpanMutable(IR_DOCUMENTptrc document, cui32 index) { return (index < document->spanCount ? document->spans + index : nullptr); }

// Shifts every record of one table, and of every table nested inside it, down by one delta. It is one
// delta because nothing inside a table is ever dropped: the only blocks that go are outside every
// table, so the whole of a table moves together. The cursor walks the table array once over the whole
// document, which is sound because tables are created in document order and a nested table's own block
// lies inside its parent's range -- so the tables belonging to one range are always a run of them.
static void IrShiftTables(IR_DOCUMENTptrc document, ui32ptrc cursor, cui32 end, cui32 delta) {
   while(*cursor < document->tableCount && document->tables[*cursor].blockAt < end) {
      IR_TABLEptr table = document->tables + *cursor;
      ui32        row   = table->firstRow;

      while(row < document->rowCount) {
         ui32 cell = document->rows[row].firstCell;

         while(cell < document->cellCount) {
            document->cells[cell].blockAt -= delta;
            cell = document->cells[cell].nextCell;
         }
         row = document->rows[row].nextRow;
      }
      table->blockAt -= delta;
      table->blockEnd -= delta;
      *cursor += 1u;
   }
}

void IrDropEmptyBlocks(IR_DOCUMENTptrc document) {
   ui32 kept   = 0;
   ui32 cursor = 0; // How far through the table array the shift above has reached

   for(ui32 index = 0; index < document->blockCount; ++index) {
      cIR_BLOCKptr block = document->blocks + index;

      // A table and everything in it is kept whole and moved as a unit -- see the header's note on why
      // an empty block inside a cell stays where it is rather than being compacted out from under the
      // index the cell names it by.
      if(block->kind == IR_BLOCK_TABLE && block->tableAt >= 0) {
         cIR_TABLEptr table = IrTableAt(document, ui32(block->tableAt));
         cui32        end   = (table && table->blockEnd > index ? table->blockEnd : index + 1u);
         cui32        stop  = (end < document->blockCount ? end : document->blockCount);
         cui32        delta = index - kept;

         IrShiftTables(document, &cursor, end, delta);
         for(ui32 at = index; at < stop; ++at) {
            document->blocks[kept] = document->blocks[at];
            ++kept;
         }
         index = stop - 1u;
         continue;
      }
      // A rule carries no spans by construction, a code paragraph may legitimately be blank, and a list
      // item is a marker whether or not it holds text, so all three are exempt here exactly as they are
      // in IrEndBlock -- the two tests have to agree, or a block that survived being ended would be
      // thrown away on the second look. Only a *real* item is exempt by the time this asks: the walk
      // records a list reference only for a w:numId the numbering part resolves, and NumAssignMarkers
      // clears any other before this runs, so an empty paragraph whose w:numId named nothing is dropped
      // like any other.
      cbool exempt = (block->kind == IR_BLOCK_RULE || block->kind == IR_BLOCK_CODE || block->listNumId >= 0);

      if(!exempt && !IrRangeHasContent(document, block->spanAt, block->spanAt + block->spanCount)) continue;
      document->blocks[kept] = *block;
      ++kept;
   }
   document->blockCount = kept;
}

cchptr IrText(cIR_DOCUMENTptr document, cui32 at) { return (document->heap ? document->heap + at : ""); }

cchptr IrDest(cIR_DOCUMENTptr document, cui32 at) { return (document->dest ? document->dest + at : ""); }

cbool IrFailed(cIR_DOCUMENTptr document) { return document->failed; }

void IrFail(IR_DOCUMENTptrc document) { document->failed = true; }

void IrAdoptSpans(IR_DOCUMENTptrc document, IR_SPANptr spans, cui64 capacity, cui32 count) {
   mdealloc(document->spans);
   document->spans        = spans;
   document->spanCapacity = capacity;
   document->spanCount    = count;
}
