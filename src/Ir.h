/*
 * File: Ir.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: The intermediate representation: blocks, spans and the arena the walker builds them in.
 * To Do: 1) Add the list, table, image, link and note span and block kinds as M7 through M10 reach them.
 *        2) Give a block a child-block list once a table cell has to hold one at M9.
 *        3) Record the source paragraph index on a block, so a diagnostic can point at the original.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Formatting

/// One bit per piece of run formatting the Markdown mapping can express. This is the *output* model, not
/// WordprocessingML's: StyleModel resolves twelve toggles and several plain properties, and the walker
/// keeps only what a delimiter could be emitted for.
/// @note Nothing reads these at M5, which emits no delimiters at all -- coalescing adjacent runs and
///       hoisting whitespace out of a span are what make a delimiter safe, and both are M6. The bits are
///       filled from the first commit because a span with no formatting on it is a span M6 would have to
///       rebuild rather than extend.
constexpr cui32 IR_FMT_NONE   = 0x00u;
constexpr cui32 IR_FMT_BOLD   = 0x01u; ///< ** **
constexpr cui32 IR_FMT_ITALIC = 0x02u; ///< * *
constexpr cui32 IR_FMT_STRIKE = 0x04u; ///< ~~ ~~
constexpr cui32 IR_FMT_SUPER  = 0x08u; ///< <sup> </sup>
constexpr cui32 IR_FMT_SUB    = 0x10u; ///< <sub> </sub>

//== Blocks and spans

/// What one block of the document is.
enum IR_BLOCK_KIND : ui8 {
   IR_BLOCK_PARAGRAPH = 0, ///< An ordinary paragraph
   IR_BLOCK_HEADING,       ///< A heading, whose level is carried beside the kind
   IR_BLOCK_KIND_COUNT     ///< Number of values above; not a kind
};

/// Constant form of IR_BLOCK_KIND, spelled per GCS r2.
typedef const IR_BLOCK_KIND cIR_BLOCK_KIND;

/// What one span inside a block is.
enum IR_SPAN_KIND : ui8 {
   IR_SPAN_TEXT = 0,  ///< A run of text with one set of formatting
   IR_SPAN_BREAK,     ///< A hard line break inside the block
   IR_SPAN_KIND_COUNT ///< Number of values above; not a kind
};

/// Constant form of IR_SPAN_KIND, spelled per GCS r2.
typedef const IR_SPAN_KIND cIR_SPAN_KIND;

/// One block. Its spans are a contiguous range, because a block is built to completion before the next
/// one starts and nothing ever inserts into the middle of one.
struct IR_BLOCK {
   ui32          spanAt;       ///< Index of the block's first span
   ui32          spanCount;    ///< How many spans it has
   IR_BLOCK_KIND kind;         ///< What the block is
   ui8           headingLevel; ///< 1 to 6 for a heading, 0 otherwise
};

/// One span. Text lives in the document's byte arena rather than in the record, so a span is sixteen
/// bytes and an array of them is worth scanning.
struct IR_SPAN {
   ui32         textAt;    ///< Arena offset of the span's bytes
   ui32         textBytes; ///< How many bytes they are; never NUL-terminated
   ui32         fmt;       ///< The IR_FMT bits in force
   IR_SPAN_KIND kind;      ///< What the span is
};

/// Constant and pointer forms of the records, spelled per GCS r2/t2.
typedef IR_BLOCK       *IR_BLOCKptr;
typedef const IR_BLOCK *cIR_BLOCKptr;
typedef IR_SPAN        *IR_SPANptr;
typedef const IR_SPAN  *cIR_SPANptr;

/// Where one block started, so that ending it can trim it or throw it away again.
struct IR_MARK {
   si32 block;  ///< The block's index, or -1 when it could not be started
   ui32 spanAt; ///< The span count when it started
   ui64 heapAt; ///< The arena's used size when it started
};

/// Constant form of IR_MARK, spelled per GCS r2.
typedef const IR_MARK cIR_MARK;

//== Document

/// One document's intermediate representation. A worker owns one of these and never shares it (D6), so
/// nothing here takes a lock.
struct al32 IR_DOCUMENT {
   IR_BLOCKptr blocks;        ///< Every block, in document order
   IR_SPANptr  spans;         ///< Every span, grouped by block
   chptr       heap;          ///< Every byte of text, addressed by offset
   ui64        blockCapacity; ///< Records allocated at blocks
   ui64        spanCapacity;  ///< Records allocated at spans
   ui64        heapCapacity;  ///< Bytes allocated at heap
   ui64        heapUsed;      ///< Bytes of heap in use
   ui32        blockCount;    ///< Blocks in blocks
   ui32        spanCount;     ///< Spans in spans
   bool        failed;        ///< Whether any append ran out of memory; sticky once set
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives. al32 says so, and the
// assertion below keeps it said whatever a later field does to the size.
static_assert(alignof(IR_DOCUMENT) >= 32u, "Ir: IR_DOCUMENT is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of IR_DOCUMENT, spelled per GCS r2/t2.
typedef IR_DOCUMENT       *IR_DOCUMENTptr;
typedef const IR_DOCUMENT *cIR_DOCUMENTptr;
typedef IR_DOCUMENT *const IR_DOCUMENTptrc;

//== Entry points

/// Prepares an empty document.
/// @param document  Receives the document. Every field is written, so it need not be initialised, and
///                  IrClose is safe to call afterwards whatever happens next.
void IrOpen(IR_DOCUMENTptrc document);

/// Releases everything the document holds, and leaves it safe to close again.
/// @param document  A document previously passed to IrOpen.
void IrClose(IR_DOCUMENTptrc document);

/// Starts a block.
/// @param document      A prepared document.
/// @param kind          What the block is.
/// @param headingLevel  1 to 6 for a heading, 0 otherwise.
/// @return Where the block started. Its block field is -1 when the document could not grow, and the
///         document's failed flag is set; a caller may keep building and check the flag once at the end.
cIR_MARK IrBeginBlock(IR_DOCUMENTptrc document, cIR_BLOCK_KIND kind, cui8 headingLevel);

/// Ends a block, trimming it and dropping it when it holds nothing worth emitting.
/// @param document  A prepared document.
/// @param mark      What IrBeginBlock returned.
/// @return true when the block was kept, false when it was dropped.
/// @note A paragraph whose text is empty or nothing but ASCII whitespace is dropped whole, which is what
///       gives CONVERSION_REFERENCE row 40's "runs of N empty paragraphs collapse" for free: blocks are
///       separated by exactly one blank line, so a block that never existed leaves no gap.
/// @note Leading and trailing break spans are trimmed. A break at the end of a paragraph would emit a
///       hard-break marker with nothing after it, which is a stray backslash at the end of a line.
/// @note A non-breaking space counts as content. CONVERSION_REFERENCE row 35 keeps U+00A0 verbatim, and
///       a paragraph holding one was written to hold something.
cbool IrEndBlock(IR_DOCUMENTptrc document, cIR_MARK mark);

/// Starts a span inside the block being built.
/// @param document  A prepared document.
/// @param kind      What the span is.
/// @param fmt       The IR_FMT bits in force.
/// @return true when the span was started, false when the document could not grow.
/// @note Spans are never merged here. Coalescing adjacent runs with identical formatting is M6's
///       RunCoalescer, and doing it early would hide the fragmentation M6 has to be measured against.
cbool IrAddSpan(IR_DOCUMENTptrc document, cIR_SPAN_KIND kind, cui32 fmt);

/// Appends bytes to the span most recently started.
/// @param document   A prepared document.
/// @param bytes      UTF-8 bytes; they are copied.
/// @param byteCount  How many.
/// @return true when they were appended, false when there is no span or the document could not grow.
/// @note One w:t can reach the walker as several text tokens -- a comment or a processing instruction
///       ends a run of character data -- so appending has to be possible after a span has been started.
cbool IrAppendText(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount);

/// Records where the document currently ends, so that a speculative walk can be undone.
/// @param document  A prepared document.
/// @return The current block count, span count and arena size.
/// @note This is what makes mc:AlternateContent right with a parser that cannot look ahead: the first
///       mc:Choice is walked into the document, and if an mc:Fallback turns up afterwards the Choice's
///       output is rewound and the Fallback walked in its place.
cIR_MARK IrMark(cIR_DOCUMENTptr document);

/// Undoes everything added since a mark.
/// @param document  A prepared document.
/// @param mark      What IrMark returned.
/// @note Only whole blocks and whole spans are unwound. A mark taken in the middle of a block leaves
///       that block's own record alone, because its span range is not written until IrEndBlock.
void IrRewind(IR_DOCUMENTptrc document, cIR_MARK mark);

/// How many blocks the document holds.
cui32 IrBlockCount(cIR_DOCUMENTptr document);

/// One block by index.
/// @return The block, or null for an index outside the document.
cIR_BLOCKptr IrBlockAt(cIR_DOCUMENTptr document, cui32 index);

/// One span by index.
/// @return The span, or null for an index outside the document.
cIR_SPANptr IrSpanAt(cIR_DOCUMENTptr document, cui32 index);

/// The bytes one arena offset names.
/// @return The bytes, or an empty string when the arena is empty. Never null, never NUL-terminated at
///         the span's own end.
cchptr IrText(cIR_DOCUMENTptr document, cui32 at);

/// Whether any append ran out of memory.
cbool IrFailed(cIR_DOCUMENTptr document);
