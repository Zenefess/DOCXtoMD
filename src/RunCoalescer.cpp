/*
 * File: RunCoalescer.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-26
 * Last Modified: 2026-08-26
 * Description: The merge pass, the whitespace classes it hoists, and the span array it rebuilds.
 * To Do: 1) Reuse the replacement span array between documents once M13 gives a worker several.
 *        2) Fold a zero-width space into the hoisted set if a producer is found putting one in a run.
 * Dependencies: BuildGuards.h, Ir.h, RunCoalescer.h, typedefs.h, memory management.h, windows.h
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
#include "RunCoalescer.h"

//-- Limits

// Hoisting splits one span into at most three -- the whitespace it began with, what is left, and the
// whitespace it ended with -- so the replacement array is sized once and never grown.
constexpr cui64 RUN_SPLIT_MAX = 3u;

//-- Whitespace

// Whether the two bytes at an offset are U+00A0. The non-breaking space is hoisted like an ASCII one
// (CONVERSION_REFERENCE 5.3), and it is two bytes in UTF-8, so it is tested as a pair rather than by
// its trailing byte -- 0xA0 alone is a continuation byte of any number of other characters.
static cbool RunIsNbsp(cchptr bytes, cui64 at, cui64 byteCount) { // Tested as a pair, never by 0xA0 alone
   return at + 1u < byteCount && ui8(bytes[at]) == 0xC2u && ui8(bytes[at + 1u]) == 0xA0u;
}

// How many bytes of hoistable whitespace a run begins with.
static cui64 RunLeadingSpace(cchptr bytes, cui64 byteCount) {
   ui64 at = 0;

   while(at < byteCount) {
      if(bytes[at] == ' ' || bytes[at] == '\t') ++at;
      else if(RunIsNbsp(bytes, at, byteCount)) at += 2u;
      else break;
   }
   return at;
}

// How many bytes of hoistable whitespace a run ends with, never reaching back past what its leading
// whitespace already claimed.
static cui64 RunTrailingSpace(cchptr bytes, cui64 byteCount, cui64 floorAt) {
   ui64 at = byteCount;

   while(at > floorAt) {
      if(bytes[at - 1u] == ' ' || bytes[at - 1u] == '\t') --at;
      else if(at >= floorAt + 2u && RunIsNbsp(bytes, at - 2u, byteCount)) at -= 2u;
      else break;
   }
   return byteCount - at;
}

//-- The merge pass

// Whether two text spans may become one: equal formatting, and bytes that really do meet in the arena.
// The second test is the load-bearing one. Every span the walker starts takes the arena's current end as
// its offset and every append moves that end, so a block's spans are laid out end to end in span order
// -- but this pass would silently corrupt a document if that ever stopped being true, and declining a
// merge costs one delimiter pair rather than a wrong range.
static cbool RunJoinable(cIR_SPANptr first, cIR_SPANptr next) {
   if(first->kind != IR_SPAN_TEXT || next->kind != IR_SPAN_TEXT) return false;
   if(first->fmt != next->fmt) return false;
   return first->textAt + first->textBytes == next->textAt;
}

// Merges each block's adjacent equal-formatting spans in place. Merging only ever shrinks a block, so it
// stays inside the block's own range and no other block's spanAt moves.
static void RunMergeBlocks(IR_DOCUMENTptrc document) {
   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      IR_BLOCKptr block = IrBlockMutable(document, index);
      ui32        kept  = 0;

      if(!block) continue;
      for(ui32 at = 0; at < block->spanCount; ++at) {
         IR_SPANptr span = document->spans + block->spanAt + at;
         // An empty text span is a run that carried a w:rPr and no text, which CONVERSION_REFERENCE 5.5
         // says must contribute nothing. Dropping it here also stops it separating two spans that would
         // otherwise merge, which is the same defect wearing a different hat.
         if(span->kind == IR_SPAN_TEXT && !span->textBytes) continue;

         IR_SPANptr previous = (kept ? document->spans + block->spanAt + kept - 1u : nullptr);

         if(previous && RunJoinable(previous, span)) {
            previous->textBytes += span->textBytes;
            continue;
         }
         document->spans[block->spanAt + kept] = *span;
         ++kept;
      }
      block->spanCount = kept;
   }
}

//-- The hoisting pass

// Everything the rebuild carries between spans.
struct RUN_REBUILD {
   IR_SPANptr spans;   ///< The replacement array
   ui32       used;    ///< Spans written to it
   ui32       blockAt; ///< Where the block being rewritten starts in it
};

typedef RUN_REBUILD *const RUN_REBUILDptrc;

// Appends one span to the replacement array, merging it into the one before it where that is sound. The
// merge is what makes hoisting idempotent: the space taken off the end of a bold run and the plain run
// that follows it are one span again, so nothing downstream sees a seam the source never had.
static void RunEmit(RUN_REBUILDptrc rebuild, cIR_SPAN_KIND kind, cui32 textAt, cui32 textBytes, cui32 fmt) {
   if(kind == IR_SPAN_TEXT && !textBytes) return;

   IR_SPAN span = {textAt, textBytes, fmt, kind};

   if(rebuild->used > rebuild->blockAt) {
      IR_SPANptr previous = rebuild->spans + rebuild->used - 1u;

      if(RunJoinable(previous, &span)) {
         previous->textBytes += textBytes;
         return;
      }
   }
   rebuild->spans[rebuild->used] = span;
   ++rebuild->used;
}

// Rewrites one span into the replacement array, hoisting the whitespace at its ends out of its
// formatting. A span with no formatting, and every span of a fenced block, is copied straight across.
static void RunHoistSpan(RUN_REBUILDptrc rebuild, cIR_DOCUMENTptr document, cIR_SPANptr span, cbool literal) {
   if(span->kind != IR_SPAN_TEXT || span->fmt == IR_FMT_NONE || literal) {
      RunEmit(rebuild, span->kind, span->textAt, span->textBytes, span->fmt);
      return;
   }

   cchptr bytes = IrText(document, span->textAt);
   cui64  lead  = RunLeadingSpace(bytes, span->textBytes);

   // Nothing but whitespace: the bytes stay, the formatting goes. Delimiters around them would be an
   // empty emphasis span, which CONVERSION_REFERENCE 5.5 forbids and no renderer parses.
   if(lead >= span->textBytes) {
      RunEmit(rebuild, IR_SPAN_TEXT, span->textAt, span->textBytes, IR_FMT_NONE);
      return;
   }

   cui64 trail = RunTrailingSpace(bytes, span->textBytes, lead);
   cui32 core  = ui32(span->textBytes - lead - trail);

   RunEmit(rebuild, IR_SPAN_TEXT, span->textAt, ui32(lead), IR_FMT_NONE);
   RunEmit(rebuild, IR_SPAN_TEXT, ui32(span->textAt + lead), core, span->fmt);
   RunEmit(rebuild, IR_SPAN_TEXT, ui32(span->textAt + lead + core), ui32(trail), IR_FMT_NONE);
}

//== Entry points

cbool RunCoalesce(IR_DOCUMENTptrc document) {
   if(IrFailed(document)) return false;
   RunMergeBlocks(document);

   cui64 wanted = ui64(document->spanCount) * RUN_SPLIT_MAX + 1u;

   // The replacement array is indexed by a ui32 like the original, so a document that could not be
   // addressed after splitting is a refusal rather than a wrap. The container caps stop one long before.
   if(wanted > 0xFFFFFFFFu) {
      IrFail(document);
      return false;
   }

   IR_SPANptr fresh = (IR_SPANptr)amalloc(wanted * sizeof(IR_SPAN), 32u);

   if(!fresh) {
      IrFail(document);
      return false;
   }

   RUN_REBUILD rebuild = {fresh, 0, 0};

   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      IR_BLOCKptr block = IrBlockMutable(document, index);

      if(!block) continue;

      cbool literal = (block->kind == IR_BLOCK_CODE);
      cui32 spanAt  = block->spanAt;
      cui32 count   = block->spanCount;

      rebuild.blockAt = rebuild.used;
      for(ui32 at = 0; at < count; ++at) RunHoistSpan(&rebuild, document, document->spans + spanAt + at, literal);
      block->spanAt    = rebuild.blockAt;
      block->spanCount = rebuild.used - rebuild.blockAt;
   }
   IrAdoptSpans(document, fresh, wanted, rebuild.used);
   return true;
}
