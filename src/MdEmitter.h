/*
 * File: MdEmitter.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-22
 * Description: The Markdown emitter: one growable UTF-8 buffer, line assembly and the delimiter rules.
 * To Do: 1) Size the buffer from the part's byte count rather than growing from a fixed first block.
 *        2) Emit an image's wp:extent size as an HTML img element where a document depends on it (row 23).
 *        3) Show a cell's nested list structure under the pipe form, which today keeps every marker and
 *           flattens every level, because indentation inside a cell has no spelling GFM reads.
 *        4) Render a cell's w:shd and w:tcBorders in the raw-HTML fallback, which could carry them.
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
   chptr      line;         ///< The line being assembled, already escaped and unterminated
   ui64       capacity;     ///< Bytes allocated at out
   ui64       lineCapacity; ///< Bytes allocated at line
   ui64       used;         ///< Bytes written to out
   ui64       lineUsed;     ///< Bytes of line in use
   HARD_BREAK hardBreak;    ///< How a w:br of type textWrapping is spelled
   TABLE_MODE tables;       ///< What a table carrying a merge is written as
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
/// @param tables     What a table carrying a merge is written as.
void MdOpen(MD_EMITTERptrc emitter, cHARD_BREAK hardBreak, cTABLE_MODE tables);

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
///       document of two paragraphs is "a\n\nb\n" and an empty document is zero bytes. One separator is
///       not blank: between two consecutive blockquote blocks it is a bare ">", which keeps a quotation
///       a producer broke into several paragraphs inside one blockquote instead of opening a new one.
/// @note A line is assembled span by span in its *output* form -- delimiters and escaped text together
///       -- rather than raw and escaped in one piece, because since M6 there is markup between the
///       spans and escaping the assembled line would escape that markup too. The two rules that depend
///       on seeing more than one span are handled by looking wider rather than by escaping later: the
///       ampersand lookahead is safe within a span because RunCoalescer has already merged every
///       adjacent pair with equal formatting -- including, since it is run again after LinkResolve, the
///       pairs a link's brackets separated until muting removed them -- so a split entity can only be
///       separated by markup that stops it being one; and D12's dollar count is taken over the whole line and passed into each
///       span's escape call, which is what MdEscapeWrite's dollars argument is for.
/// @note One consequence of grouping worth stating rather than discovering: `IrEndBlock` drops an empty
///       paragraph completely, so a blank Normal-styled line between two separate code samples leaves
///       the two runs of code blocks adjacent and they become **one** fence. That is a real change to
///       the document and it is deliberate only in the sense that the alternative -- recording that a
///       block was dropped, so the grouping can see the gap -- is more machinery than the case has
///       earned. `tests/fixtures/code` pins the behaviour, so changing it is a visible diff.
/// @note What each block kind emits: a paragraph is its lines; a heading is one ATX line, its hashes
///       written before the content and a break inside it folded to one space; a blockquote is its
///       lines with "> " in front of each; a horizontal rule is "---"; and a run of consecutive code
///       blocks is one fenced block, its fence longer than the longest backtick run inside it and its
///       content passed through MD_CONTEXT_CODE_BLOCK, where nothing is escaped.
/// @note Each emitted line has its leading and trailing ASCII spaces and tabs removed, and then one
///       backslash inserted if what is left would start a list, a quote, a heading, a setext underline
///       or a thematic break. Four leading spaces would otherwise be an indented code block, and a
///       trailing space or two is Markdown's other spelling of a hard line break.
/// @note A hard break with nothing after it is dropped, and two with nothing between them collapse into
///       one: a Markdown line that is empty ends the paragraph, so neither spelling of a hard break can
///       carry an empty continuation line. Inside a heading a break becomes one space, because an ATX
///       heading is a single line by construction.
/// @note Delimiters nest in one fixed order, outermost first: "<sup>" or "<sub>", then "~~", then the
///       emphasis, then a code span's backticks. Bold and italic together are "***" (mapping row 5), and
///       a code span drops bold and italic (row 11's own ruling on that collision) while keeping the
///       strikethrough and the vertical alignment, which wrap it perfectly well.
/// @note The document handed here must have been through RunCoalesce. Wrapping a span in delimiters is
///       only safe once adjacent runs with equal formatting have been merged and whitespace hoisted out
///       of the span -- "**Hel****lo**" and "**bold **text" are what the two omissions produce -- and
///       this module assumes both, so it emits a delimiter pair around any formatted span it is given.
/// @note It must also have been through LinkResolve and MediaPlan, and then IrDropEmptyBlocks. This
///       module writes a link's destination exactly as it finds it and never looks a reference up, so a
///       document that skipped those passes emits a relationship id where a URL belongs; and every block
///       it is handed is assumed to produce at least one byte, which is what lets the blank line between
///       two blocks be written before the second rather than unwound after it.
/// @note What M7's four span kinds emit: a link is its content between brackets and its destination in
///       parentheses, percent-encoded rather than backslash-escaped; an image is that with a '!' in
///       front and its alt text between the brackets; an anchor is the raw "<a id>" element mapping row
///       22 asks for. A span LinkResolve muted emits nothing at all -- a link with no destination or no
///       content, an anchor nothing points at -- and a link that runs over a hard break is closed at the
///       end of its line and opened again on the next, because Markdown cannot spell one that does.
/// @note Two rules follow from a link needing to see more than the span it stands on. A hard break at the
///       very *edge* of a link leaves one of its two halves with nothing between the brackets, and
///       "[](url)" is a link a reader can neither see nor click, so that bracket is unwound rather than
///       closed. And an exclamation mark immediately in front of a link's '[' is escaped: the pair is an
///       image marker, so "see this!" followed by a link renders as a broken picture with the link text
///       gone. That is CONVERSION_REFERENCE 4.2's pitfall 7, and MdEscape leaves the mark alone on
///       purpose -- it is only dangerous next to a bracket this module itself writes, which is knowledge
///       a run does not have.
/// @note What M9's table emits, and the two forms it comes in. A table whose shape a GFM pipe table can
///       carry is one: a leading "|", one padded cell per grid column, a delimiter row under the first
///       row carrying whatever alignment its own cells stated, and a row per row after it. What a pipe
///       table cannot carry is block content, so a cell is flattened to one line -- its paragraphs
///       joined by "<br>", a hard break inside one the same, a list item keeping its marker as literal
///       text, and a code paragraph becoming a code span. A merge is padded: the cell's content lands
///       in the first column it covers and the rest are empty, which is CONVERSION_REFERENCE row 19's
///       policy A, and a vertical merge's continuation cells are empty because that is what Word draws.
/// @note The other form is a raw <table>, and it fires for a table holding another table always -- a
///       pipe table has no way to say one -- and for a table holding a merge when --tables is
///       html-on-merge. Everything inside it is written as HTML rather than as Markdown, because a
///       CommonMark HTML block runs to the next blank line and passes every byte of itself through
///       unparsed: emphasis becomes <strong> and <em>, a code span <code>, a link an <a href> and a
///       break a <br>, and text takes MD_CONTEXT_HTML_BLOCK, where the only escapes are entities.
/// @note The delimiter row is what makes a pipe table a table at all: GFM reads one only where the
///       delimiter row has exactly as many cells as the header, so the width every row is padded to is
///       the wider of what w:tblGrid declares and what the widest row's cells actually reach.
cMD_RESULT MdEmitDocument(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document);

/// The emitted bytes.
/// @return The buffer, or an empty string when nothing was emitted. Never null, never NUL-terminated.
cchptr MdBytes(cMD_EMITTERptr emitter);

/// How many bytes were emitted.
/// @return The byte count, which is 0 before MdEmitDocument runs and after it fails.
cui64 MdByteCount(cMD_EMITTERptr emitter);
