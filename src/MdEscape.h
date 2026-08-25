/*
 * File: MdEscape.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: The context-aware Markdown escaping writer: one rule set per place text can be emitted.
 * To Do: 1) Escape a leading pipe once tables give a line one, which is M9's business.
 *        2) Revisit the link-destination rule against real hyperlink targets when M7 emits one.
 *        3) Benchmark an AVX2 scan for the next byte needing an escape before adopting one (bd1/bd2).
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Contexts

/// Where text is being emitted, which is what decides how it is escaped. Escaping is context-dependent:
/// one "escape everything" pass litters the output with backslashes, and no escaping corrupts documents.
/// @note CONVERSION_REFERENCE 4.1 lists a lineStart context alongside these. It is not one here, and the
///       reason is its pitfall 6: the ordered-list and setext patterns can only be judged once a whole
///       line has been assembled out of however many runs, so line starts are a post-pass over the
///       finished line -- MdEscapeLineStartAt -- rather than a mode a run can be written in.
enum MD_CONTEXT : si32 {
   MD_CONTEXT_INLINE = 0, ///< Ordinary text inside a paragraph or a heading
   MD_CONTEXT_TABLE_CELL, ///< Inside a GFM pipe-table cell, where a pipe would end it
   MD_CONTEXT_LINK_TEXT,  ///< Between the brackets of a link
   MD_CONTEXT_LINK_DEST,  ///< Between the parentheses of a link, where percent-encoding replaces escapes
   MD_CONTEXT_ALT_TEXT,   ///< Between the brackets of an image
   MD_CONTEXT_CODE_SPAN,  ///< Inside a code span, where nothing at all is escaped
   MD_CONTEXT_CODE_BLOCK, ///< Inside a fenced block, where nothing at all is escaped
   MD_CONTEXT_HTML,       ///< Inside a raw-HTML fallback: the inline rules, plus & and < as entities
   MD_CONTEXT_COUNT       ///< Number of values above; not a context
};

/// Constant form of MD_CONTEXT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const MD_CONTEXT cMD_CONTEXT;

//== Entry points

/// Measures the escaped form of a run of text.
/// @param text       The bytes to escape; a null pointer measures nothing.
/// @param byteCount  How many there are.
/// @param context    Where they are going.
/// @return How many bytes the escaped form occupies.
cui64 MdEscapeMeasure(cchptr text, cui64 byteCount, cMD_CONTEXT context);

/// Writes the escaped form of a run of text.
/// @param dest       Receives the escaped bytes, unterminated.
/// @param destBytes  Bytes available at dest. Writing stops when they run out.
/// @param text       The bytes to escape; a null pointer writes nothing.
/// @param byteCount  How many there are.
/// @param context    Where they are going.
/// @return How many bytes were written.
/// @note The escape set is CONVERSION_REFERENCE 4.1 and 4.2. In the inline family, the backslash,
///       asterisk, underscore, backtick, both brackets and the tilde are escaped unconditionally, per
///       pitfall 4 -- a literal asterisk next to an emitted one merges into a delimiter otherwise. A
///       less-than sign is escaped only where it could open a tag or an autolink, and an ampersand only
///       where an entity reference would be recognised, because escaping either everywhere is noise.
/// @note The exclamation mark is deliberately not escaped. Pitfall 7 is about an image marker forming in
///       front of an emitted link, and the bracket that would complete it is already escaped here; the
///       adjacency that pitfall really names belongs to whichever milestone emits the link.
/// @note Nothing whatever is escaped in the two code contexts. A backtick inside a code span cannot be
///       escaped at all, so a collision is handled by lengthening the delimiter, which is the emitter's
///       job and not this module's.
/// @note MD_CONTEXT_HTML is the inline set *plus* two entities, not instead of it. GitHub-Flavored
///       Markdown passes a raw tag through unparsed but still parses the text between the tags as
///       ordinary inline content, so "<sup>*n*</sup>" italicises the n. The ampersand and the
///       less-than sign become entities unconditionally there, because inside a fallback there is no
///       room for the guesswork the inline rules do about which of them could start markup.
/// @note Four contexts have no caller before M7: the two link contexts, the alt text and the table
///       cell. They are written now because correctness rule 6 forbids an emitter concatenating raw
///       text and because the rules are pure and testable without one -- but they are provisional,
///       and the milestone that first emits through one is expected to re-cut it against real input.
cui64 MdEscapeWrite(chptrc dest, cui64 destBytes, cchptr text, cui64 byteCount, cMD_CONTEXT context);

/// Reports where a finished line needs one backslash to stop it starting a block it should not.
/// @param line         The line's bytes, already escaped for MD_CONTEXT_INLINE and already trimmed.
/// @param byteCount    How many there are.
/// @param continuation Whether another line of the same block stands above this one, which is what a
///                     setext underline needs in order to be one.
/// @return The offset a single backslash must be inserted before, or -1 when the line is safe.
/// @note This is CONVERSION_REFERENCE 4.1's lineStart row and 4.2's pitfalls 5 and 6. It runs on the
///       assembled line and not on each run, because "digits then a dot then a space" is a property of
///       the line and a run may hold only part of it.
/// @note Only the *first* offending construct is reported, which is all that is needed: a line that no
///       longer begins a list, a quote, a heading, a setext underline or a thematic break cannot begin
///       one further along, because every one of those patterns is anchored to the start of the line.
/// @note A setext underline promotes the line above it, so it can only be one on a continuation line:
///       blocks are separated by a blank line, and the first line of a block has nothing above it to
///       promote. A thematic break has no such condition and is escaped wherever it appears.
csi64 MdEscapeLineStartAt(cchptr line, cui64 byteCount, cbool continuation);

/// Reports where a heading's finished content needs one backslash to stop its tail being eaten.
/// @param content    The heading's content, already escaped and trimmed, without the leading hashes.
/// @param byteCount  How many bytes it holds.
/// @return The offset a single backslash must be inserted before, or -1 when the content is safe.
/// @note An ATX heading may end in a run of hashes preceded by a space, and a renderer strips it as a
///       closing sequence: "# Sharp #" renders as "Sharp". Escaping the first hash of that run keeps it.
csi64 MdEscapeHeadingTailAt(cchptr content, cui64 byteCount);
