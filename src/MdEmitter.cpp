/*
 * File: MdEmitter.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: Buffer growth, line assembly, the blank-line discipline and heading and paragraph output.
 * To Do: 1) Emit inline delimiters at M6, sized so a literal backtick run cannot close a code span.
 *        2) Keep a per-line prefix stack when blockquotes and list items nest at M6 and M8.
 *        3) Escape each coalesced span rather than each line once M6 puts markup between them.
 * Dependencies: BuildGuards.h, CliOptions.h, Ir.h, MdEmitter.h, MdEscape.h, typedefs.h,
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
#include "CliOptions.h"
#include "Ir.h"
#include "MdEscape.h"
#include "MdEmitter.h"

//-- Constants

// The first allocation of each buffer. A document of a few paragraphs never needs a second one, and a
// large one reaches its size in a handful of doublings.
constexpr cui64 MD_FIRST_BYTES = 4096u;

// The deepest ATX heading GitHub-Flavored Markdown has.
constexpr cui32 MD_MAX_HEADING = 6u;

//-- Buffers

// Grows one buffer to hold at least the requested number of bytes, doubling so that filling it costs
// amortised constant time.
static cbool MdGrow(MD_EMITTERptrc emitter, chptrptrc block, ui64ptrc capacity, cui64 needed) {
   if(needed <= *capacity) return true;

   ui64 grown = (*capacity ? *capacity : MD_FIRST_BYTES);

   while(grown < needed) grown *= 2u;

   chptr fresh = (chptr)amalloc(grown, 32u);

   if(!fresh) {
      emitter->failed = true;
      return false;
   }
   if(*block) Copy(*block, fresh, *capacity);
   mdealloc(*block);
   *block    = fresh;
   *capacity = grown;
   return true;
}

// Appends raw bytes to the output, which are markup this module wrote itself or text already escaped.
static cbool MdAppend(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount) {
   if(!byteCount) return true;
   if(!MdGrow(emitter, &emitter->out, &emitter->capacity, emitter->used + byteCount)) return false;
   Copy(bytes, emitter->out + emitter->used, byteCount);
   emitter->used += byteCount;
   return true;
}

// Appends one byte to the output.
static cbool MdAppendByte(MD_EMITTERptrc emitter, cchar byte) { return MdAppend(emitter, &byte, 1u); }

// Inserts one byte into the output at an offset, shifting whatever follows it up by one. Only ever used
// to put a backslash at the head of a line that has just been written, so the tail being moved is short.
static cbool MdInsertByte(MD_EMITTERptrc emitter, cui64 at, cchar byte) {
   if(!MdGrow(emitter, &emitter->out, &emitter->capacity, emitter->used + 1u)) return false;
   for(ui64 index = emitter->used; index > at; --index) emitter->out[index] = emitter->out[index - 1u];
   emitter->out[at] = byte;
   emitter->used += 1u;
   return true;
}

// Appends unescaped bytes to the line being assembled.
static cbool MdLineAppend(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount) {
   if(!byteCount) return true;
   if(!MdGrow(emitter, &emitter->line, &emitter->lineCapacity, emitter->lineUsed + byteCount)) return false;
   Copy(bytes, emitter->line + emitter->lineUsed, byteCount);
   emitter->lineUsed += byteCount;
   return true;
}

//-- Lines

// Whether a byte is one of the two Markdown treats as insignificant at the ends of a line.
static cbool MdIsPad(cchar byte) { return byte == ' ' || byte == '\t'; }

// Escapes the assembled line into the output and reports where in the output it started.
// @note The line is assembled raw and escaped in one piece rather than span by span, and that is a
//       correctness matter rather than a tidiness one. The ampersand rule looks ahead for an entity
//       pattern, and Word fragments a run wherever a spellcheck or revision boundary falls; escaping
//       per span would let "A&amp;B" come out differently depending on whether the producer happened
//       to split it after the ampersand. M6's coalescer removes that fragmentation, which is what
//       makes escaping per coalesced span right again once there is markup between the spans.
static cbool MdWriteLine(MD_EMITTERptrc emitter, cMD_CONTEXT context) {
   // Two trailing spaces are Markdown's other spelling of a hard break, so leaving the padding a
   // producer wrote would put breaks the document never had at the end of every line.
   while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;

   cui64 wanted = MdEscapeMeasure(emitter->line, emitter->lineUsed, context);

   if(!wanted) return true;
   if(!MdGrow(emitter, &emitter->out, &emitter->capacity, emitter->used + wanted)) return false;
   emitter->used += MdEscapeWrite(emitter->out + emitter->used, wanted, emitter->line, emitter->lineUsed, context);
   return true;
}

// Writes the assembled line and inserts the one backslash that stops it opening a block it should not.
static cbool MdCloseLine(MD_EMITTERptrc emitter, cbool continuation) {
   cui64 lineAt = emitter->used;

   if(!MdWriteLine(emitter, MD_CONTEXT_INLINE)) return false;

   csi64 at = MdEscapeLineStartAt(emitter->out + lineAt, emitter->used - lineAt, continuation);

   emitter->lineUsed = 0;
   if(at < 0) return true;
   return MdInsertByte(emitter, lineAt + ui64(at), '\\');
}

// Writes the hard-break marker and the newline that ends a continued line.
static cbool MdBreakLine(MD_EMITTERptrc emitter) {
   // A trailing backslash survives an editor that strips trailing whitespace; two trailing spaces do
   // not, which is why the backslash is the default and the two-space form is opt-in.
   if(emitter->hardBreak == HARD_BREAK_SPACES) {
      if(!MdAppend(emitter, "  ", 2u)) return false;
   } else if(!MdAppendByte(emitter, '\\')) {
      return false;
   }
   return MdAppendByte(emitter, '\n');
}

//-- Blocks

// Emits one paragraph, assembling it line by line: a break span ends a line, and every other span adds
// text to the one being built.
static cbool MdEmitParagraph(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   bool lineContent  = false;
   bool pendingBreak = false;
   bool wroteLine    = false;

   emitter->lineUsed = 0;
   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span) continue;
      if(span->kind == IR_SPAN_BREAK) {
         // A break before any content is nothing to break after, and a second break with nothing
         // between it and the first would make an empty line, which ends the paragraph.
         if(lineContent) pendingBreak = true;
         continue;
      }

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      // Four leading spaces would be an indented code block, and the rest is noise, so a line's own
      // leading padding goes. A line only ever starts here or straight after a break.
      if(!lineContent || pendingBreak) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      // A break is only spent once real content follows it, so one with nothing but padding after it
      // is dropped rather than left as a marker on the end of the paragraph.
      if(start >= span->textBytes) continue;
      if(pendingBreak) {
         if(!MdCloseLine(emitter, wroteLine)) return false;
         if(!MdBreakLine(emitter)) return false;
         wroteLine    = true;
         pendingBreak = false;
         lineContent  = false;
      }
      if(!MdLineAppend(emitter, bytes + start, span->textBytes - start)) return false;
      lineContent = true;
   }
   if(!MdCloseLine(emitter, wroteLine)) return false;
   return MdAppendByte(emitter, '\n');
}

// Emits one heading. Its content is not at the start of a line -- the hashes and their space are -- so
// the line-start rules do not apply to it; the closing-sequence rule takes their place.
static cbool MdEmitHeading(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   cui64 lineAt       = emitter->used;
   ui32  level        = (block->headingLevel ? block->headingLevel : 1u);
   bool  content      = false;
   bool  pendingSpace = false;

   emitter->lineUsed = 0;
   if(level > MD_MAX_HEADING) level = MD_MAX_HEADING;
   for(ui32 hash = 0; hash < level; ++hash) {
      if(!MdAppendByte(emitter, '#')) return false;
   }
   if(!MdAppendByte(emitter, ' ')) return false;

   cui64 contentAt = emitter->used;

   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span) continue;
      if(span->kind == IR_SPAN_BREAK) {
         // An ATX heading is one line by construction, so a break inside one becomes a space.
         if(content) pendingSpace = true;
         continue;
      }

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      // A break becomes exactly one space, so the padding on either side of it goes the way a line's
      // own leading and trailing padding does. Two spaces would render as one anyway; what they would
      // really do is put an invisible difference in a file that is compared byte for byte.
      if(!content || pendingSpace) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      if(start >= span->textBytes) continue;
      if(pendingSpace) {
         while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
         if(!MdLineAppend(emitter, " ", 1u)) return false;
         pendingSpace = false;
      }
      if(!MdLineAppend(emitter, bytes + start, span->textBytes - start)) return false;
      content = true;
   }
   if(!MdWriteLine(emitter, MD_CONTEXT_INLINE)) return false;
   emitter->lineUsed = 0;
   // Trimming back to the head of the line rather than to the head of the content is deliberate: a
   // heading whose content came to nothing must not be left with the space after its hashes.
   while(emitter->used > lineAt && MdIsPad(emitter->out[emitter->used - 1u])) emitter->used -= 1u;

   csi64 tail = MdEscapeHeadingTailAt(emitter->out + contentAt, (emitter->used > contentAt ? emitter->used - contentAt : 0u));

   if(tail >= 0 && !MdInsertByte(emitter, contentAt + ui64(tail), '\\')) return false;
   return MdAppendByte(emitter, '\n');
}

//== Entry points

void MdOpen(MD_EMITTERptrc emitter, cHARD_BREAK hardBreak) {
   mzero(emitter, sizeof(MD_EMITTER));
   emitter->hardBreak = hardBreak;
}

void MdClose(MD_EMITTERptrc emitter) {
   cHARD_BREAK hardBreak = emitter->hardBreak;

   mdealloc(emitter->out);
   mdealloc(emitter->line);
   MdOpen(emitter, hardBreak);
}

cMD_RESULT MdEmitDocument(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document) {
   cui32 blocks = IrBlockCount(document);

   for(ui32 index = 0; index < blocks; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;
      // Exactly one blank line between blocks. Each block writes its own closing newline, so the
      // separator is one more of them and the last block leaves the file ending in a single newline.
      // No block can come to nothing here -- IrEndBlock drops one that holds no printable byte -- so
      // the separator can be written before the block rather than unwound again afterwards.
      if(emitter->used && !MdAppendByte(emitter, '\n')) return MD_ERROR_MEMORY;
      if(block->kind == IR_BLOCK_HEADING) {
         if(!MdEmitHeading(emitter, document, block)) return MD_ERROR_MEMORY;
         continue;
      }
      if(!MdEmitParagraph(emitter, document, block)) return MD_ERROR_MEMORY;
   }
   return (emitter->failed ? MD_ERROR_MEMORY : MD_OK);
}

cchptr MdBytes(cMD_EMITTERptr emitter) { return (emitter->out ? emitter->out : ""); }

cui64 MdByteCount(cMD_EMITTERptr emitter) { return emitter->used; }
