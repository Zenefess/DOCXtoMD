/*
 * File: Ir.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-27
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

void IrOpen(IR_DOCUMENTptrc document) { mzero(document, sizeof(IR_DOCUMENT)); }

void IrClose(IR_DOCUMENTptrc document) {
   mdealloc(document->blocks);
   mdealloc(document->spans);
   mdealloc(document->heap);
   mdealloc(document->dest);
   IrOpen(document);
}

cIR_MARK IrBeginBlock(IR_DOCUMENTptrc document, cIR_BLOCK_KIND kind, cui8 headingLevel) {
   IR_MARK mark = {-1, document->spanCount, document->heapUsed, document->destUsed};

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
      document->destUsed  = mark.destAt;
      return true;
   }

   bool content = IrRangeHasContent(document, first, last);

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

cIR_MARK IrMark(cIR_DOCUMENTptr document) {
   IR_MARK mark = {si32(document->blockCount), document->spanCount, document->heapUsed, document->destUsed};

   return mark;
}

void IrRewind(IR_DOCUMENTptrc document, cIR_MARK mark) {
   if(mark.block < 0) return;
   if(ui32(mark.block) < document->blockCount) document->blockCount = ui32(mark.block);
   if(mark.spanAt < document->spanCount) document->spanCount = mark.spanAt;
   if(mark.heapAt < document->heapUsed) document->heapUsed = mark.heapAt;
   if(mark.destAt < document->destUsed) document->destUsed = mark.destAt;
}

cui32 IrBlockCount(cIR_DOCUMENTptr document) { return document->blockCount; }

cui32 IrSpanCount(cIR_DOCUMENTptr document) { return document->spanCount; }

cbool IrHasInk(cIR_DOCUMENTptr document, cui32 first, cui32 last) {
   cui32 stop = (last < document->spanCount ? last : document->spanCount);

   return (first < stop ? IrRangeHasInk(document, first, stop) : false);
}

cIR_BLOCKptr IrBlockAt(cIR_DOCUMENTptr document, cui32 index) { return (index < document->blockCount ? document->blocks + index : nullptr); }

cIR_SPANptr IrSpanAt(cIR_DOCUMENTptr document, cui32 index) { return (index < document->spanCount ? document->spans + index : nullptr); }

IR_BLOCKptr IrBlockMutable(IR_DOCUMENTptrc document, cui32 index) { return (index < document->blockCount ? document->blocks + index : nullptr); }

IR_SPANptr IrSpanMutable(IR_DOCUMENTptrc document, cui32 index) { return (index < document->spanCount ? document->spans + index : nullptr); }

void IrDropEmptyBlocks(IR_DOCUMENTptrc document) {
   ui32 kept = 0;

   for(ui32 index = 0; index < document->blockCount; ++index) {
      cIR_BLOCKptr block = document->blocks + index;
      // A rule carries no spans by construction and a code paragraph may legitimately be blank, so both
      // are exempt here exactly as they are in IrEndBlock -- the two tests have to agree, or a block
      // that survived being ended would be thrown away on the second look.
      cbool exempt = (block->kind == IR_BLOCK_RULE || block->kind == IR_BLOCK_CODE);

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
