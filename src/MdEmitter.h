/*
 * File: MdEmitter.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: The Markdown emitter: one growable UTF-8 buffer, blank-line discipline and line assembly.
 * To Do: 1) Emit the inline delimiters at M6, once RunCoalescer has merged spans and hoisted whitespace.
 *        2) Distribute a blockquote prefix across every line of a quoted block when M6 detects one.
 *        3) Size the buffer from the part's byte count rather than growing from a fixed first block.
 * Dependencies: CliOptions.h, Ir.h, MdEscape.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "CliOptions.h"
#include "Ir.h"
#include "MdEscape.h"

//== Results

/// Why emission stopped.
enum MD_RESULT : si32 {
   MD_OK = 0,       ///< The document was emitted
   MD_ERROR_MEMORY, ///< The output buffer could not grow
   MD_RESULT_COUNT  ///< Number of values above; not a result
};

/// Constant form of MD_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const MD_RESULT cMD_RESULT;

//== Emitter

/// One document's Markdown, and the policy that shaped it. A worker owns one of these and never shares
/// it (D6), so nothing here takes a lock.
struct al32 MD_EMITTER {
   chptr      out;          ///< The UTF-8 output, unterminated
   chptr      line;         ///< The line being assembled, unescaped and unterminated
   ui64       capacity;     ///< Bytes allocated at out
   ui64       lineCapacity; ///< Bytes allocated at line
   ui64       used;         ///< Bytes written to out
   ui64       lineUsed;     ///< Bytes of line in use
   HARD_BREAK hardBreak;    ///< How a w:br of type textWrapping is spelled
   bool       failed;       ///< Whether a growth failed; sticky once set
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives. al32 says so, and the
// assertion below keeps it said whatever a later field does to the size.
static_assert(alignof(MD_EMITTER) >= 32u, "MdEmitter: MD_EMITTER is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of MD_EMITTER, spelled per GCS r2/t2.
typedef MD_EMITTER       *MD_EMITTERptr;
typedef const MD_EMITTER *cMD_EMITTERptr;
typedef MD_EMITTER *const MD_EMITTERptrc;

//== Entry points

/// Prepares an emitter.
/// @param emitter    Receives the emitter. Every field is written, so it need not be initialised, and
///                   MdClose is safe to call afterwards whatever happens next.
/// @param hardBreak  How a hard line break inside a paragraph is spelled.
void MdOpen(MD_EMITTERptrc emitter, cHARD_BREAK hardBreak);

/// Releases the output buffer, and leaves the emitter safe to close again.
/// @param emitter  An emitter previously passed to MdOpen.
void MdClose(MD_EMITTERptrc emitter);

/// Emits one document's blocks, appending to whatever the emitter already holds.
/// @param emitter   A prepared emitter.
/// @param document  The intermediate representation to emit.
/// @return MD_OK, or MD_ERROR_MEMORY when the buffer could not grow.
/// @note The output contract is UTF-8, no byte-order mark, LF line endings -- tc2's CRLF governs this
///       project's source files and not the documents it writes. A caller must open its output file in
///       binary mode, or the C runtime will turn every LF into a CRLF on Windows.
/// @note Blocks are separated by exactly one blank line and every block ends with one newline, so a
///       document of two paragraphs is "a\n\nb\n" and an empty document is zero bytes.
/// @note A line is assembled raw and escaped in one piece, not span by span. The ampersand rule looks
///       ahead for an entity pattern, and Word fragments a run wherever a revision or spellcheck
///       boundary falls, so escaping per span would make the output depend on where the producer
///       happened to split its runs.
/// @note Each emitted line has its leading and trailing ASCII spaces and tabs removed, and then one
///       backslash inserted if what is left would start a list, a quote, a heading, a setext underline
///       or a thematic break. Four leading spaces would otherwise be an indented code block, and a
///       trailing space or two is Markdown's other spelling of a hard line break.
/// @note A hard break with nothing after it is dropped, and two with nothing between them collapse into
///       one: a Markdown line that is empty ends the paragraph, so neither spelling of a hard break can
///       carry an empty continuation line. Inside a heading a break becomes one space, because an ATX
///       heading is a single line by construction.
/// @note No inline delimiter is emitted at M5. Wrapping a span in delimiters is only safe once adjacent
///       runs with equal formatting have been merged and whitespace hoisted out of the span, and both
///       of those are M6's RunCoalescer.
cMD_RESULT MdEmitDocument(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document);

/// The emitted bytes.
/// @return The buffer, or an empty string when nothing was emitted. Never null, never NUL-terminated.
cchptr MdBytes(cMD_EMITTERptr emitter);

/// How many bytes were emitted.
/// @return The byte count, which is 0 before MdEmitDocument runs and after it fails.
cui64 MdByteCount(cMD_EMITTERptr emitter);
