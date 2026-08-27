/*
 * File: Ir.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: The intermediate representation's arena: growth, span appends and empty-block trimming.
 * To Do: 1) Size the first allocation from the part's own byte count, once the walker knows it.
 *        2) Release the arena back to the allocator between documents when M13 reuses a worker.
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

//== Entry points

void IrOpen(IR_DOCUMENTptrc document) { mzero(document, sizeof(IR_DOCUMENT)); }

void IrClose(IR_DOCUMENTptrc document) {
   mdealloc(document->blocks);
   mdealloc(document->spans);
   mdealloc(document->heap);
   IrOpen(document);
}

cIR_MARK IrBeginBlock(IR_DOCUMENTptrc document, cIR_BLOCK_KIND kind, cui8 headingLevel) {
   IR_MARK mark = {-1, document->spanCount, document->heapUsed};

   if(!IrReserve((ptrptrc)&document->blocks, &document->blockCapacity, ui64(document->blockCount) + 1u, sizeof(IR_BLOCK))) {
      document->failed = true;
      return mark;
   }

   IR_BLOCKptr block = document->blocks + document->blockCount;

   block->spanAt       = document->spanCount;
   block->spanCount    = 0;
   block->kind         = kind;
   block->headingLevel = headingLevel;
   mark.block          = si32(document->blockCount);
   ++document->blockCount;
   return mark;
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
   if(block->kind == IR_BLOCK_RULE) {
      block->spanAt       = mark.spanAt;
      block->spanCount    = 0;
      document->spanCount = mark.spanAt;
      document->heapUsed  = mark.heapAt;
      return true;
   }

   bool content = false;

   for(ui32 index = first; index < last && !content; ++index) {
      cIR_SPANptr span = document->spans + index;

      if(span->kind != IR_SPAN_TEXT) continue;
      for(ui32 at = 0; at < span->textBytes; ++at) {
         if(!IrIsBlank(document->heap[span->textAt + at])) content = true;
      }
   }
   // An empty code paragraph is a blank line inside a fence, which is content of a kind an ordinary
   // paragraph has no equivalent for -- so it is kept here and the emitter trims one only where it
   // falls at the edge of a fence, which is where it would be a blank line before or after the code.
   if(!content && block->kind == IR_BLOCK_CODE) content = true;
   if(!content) {
      // Nothing worth emitting: unwind the block completely, arena and all, so that the next block's
      // text starts where this one's would have and an empty paragraph costs nothing at all.
      document->blockCount = ui32(mark.block);
      document->spanCount  = mark.spanAt;
      document->heapUsed   = mark.heapAt;
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
   span->fmt       = fmt;
   span->kind      = kind;
   ++document->spanCount;
   return true;
}

cbool IrAppendText(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount) {
   if(!document->spanCount) return false;
   if(!byteCount) return true;
   if(document->heapUsed + byteCount > IR_MAX_HEAP_BYTES) {
      document->failed = true;
      return false;
   }
   if(!IrReserve((ptrptrc)&document->heap, &document->heapCapacity, document->heapUsed + byteCount, 1u)) {
      document->failed = true;
      return false;
   }

   IR_SPANptr span = document->spans + document->spanCount - 1u;

   Copy(bytes, document->heap + document->heapUsed, byteCount);
   document->heapUsed += byteCount;
   span->textBytes += ui32(byteCount);
   return true;
}

cIR_MARK IrMark(cIR_DOCUMENTptr document) {
   IR_MARK mark = {si32(document->blockCount), document->spanCount, document->heapUsed};

   return mark;
}

void IrRewind(IR_DOCUMENTptrc document, cIR_MARK mark) {
   if(mark.block < 0) return;
   if(ui32(mark.block) < document->blockCount) document->blockCount = ui32(mark.block);
   if(mark.spanAt < document->spanCount) document->spanCount = mark.spanAt;
   if(mark.heapAt < document->heapUsed) document->heapUsed = mark.heapAt;
}

cui32 IrBlockCount(cIR_DOCUMENTptr document) { return document->blockCount; }

cIR_BLOCKptr IrBlockAt(cIR_DOCUMENTptr document, cui32 index) { return (index < document->blockCount ? document->blocks + index : nullptr); }

cIR_SPANptr IrSpanAt(cIR_DOCUMENTptr document, cui32 index) { return (index < document->spanCount ? document->spans + index : nullptr); }

IR_BLOCKptr IrBlockMutable(IR_DOCUMENTptrc document, cui32 index) { return (index < document->blockCount ? document->blocks + index : nullptr); }

cchptr IrText(cIR_DOCUMENTptr document, cui32 at) { return (document->heap ? document->heap + at : ""); }

cbool IrFailed(cIR_DOCUMENTptr document) { return document->failed; }

void IrFail(IR_DOCUMENTptrc document) { document->failed = true; }

void IrAdoptSpans(IR_DOCUMENTptrc document, IR_SPANptr spans, cui64 capacity, cui32 count) {
   mdealloc(document->spans);
   document->spans        = spans;
   document->spanCapacity = capacity;
   document->spanCount    = count;
}
