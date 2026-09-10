/*
 * File: RunCoalescer.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-26
 * Last Modified: 2026-09-09
 * Description: The coalescing pass: adjacent runs merged on equal formatting, whitespace hoisted out.
 * To Do: 1) Stop merging across a field-result boundary when M10 introduces one.
 *        2) Coalesce a table cell's own blocks once M9 gives a block children.
 *        3) Benchmark an AVX2 scan for the first and last non-space byte of a span before adopting one.
 * Dependencies: Ir.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Ir.h"

//== Entry points

/// Merges and trims every block's spans, leaving an intermediate representation a delimiter may be
/// wrapped around safely.
/// @param document  A document DocWalk has filled. Its span array is replaced.
/// @return true when the pass finished, false when it could not allocate; the document's failed flag is
///         set either way, so a caller that already checks IrFailed need not check this as well.
/// @note Two things happen here, in this order, and the order is the whole point. First adjacent text
///       spans carrying equal formatting are merged (CONVERSION_REFERENCE 5.1): Word splits a logical
///       run at every spellcheck and revision boundary, so "Hello" arrives as "Hel" and "lo" and a
///       delimiter per run would emit "**Hel****lo**", which is not emphasis at all. Then leading and
///       trailing whitespace is hoisted out of each formatted span (5.3), because CommonMark's flanking
///       rules refuse "**bold **text" -- a closing delimiter may not be preceded by whitespace, nor an
///       opening one followed by it. Hoisting before merging would be wrong: "**a **" beside "**b**"
///       has to become "**a b**", and hoisting first would make it "**a** **b**".
/// @note Whitespace here is the tab and the whole Unicode Zs category -- U+0020, U+00A0, U+1680,
///       U+2000..U+200A, U+202F, U+205F and U+3000 -- because CommonMark 0.30 counts every Zs as
///       whitespace for flanking, so a closing delimiter behind any of them may not parse, and moving it
///       outside the delimiters renders the same in every renderer (mapping row 35). None of them is
///       exotic: U+2002 is one Insert-Symbol away in Word and U+3000 is what a CJK keyboard's space bar
///       produces. U+200B is deliberately absent -- it is Cf rather than Zs, and CommonMark does not
///       count it. Note the deliberate asymmetry with IrEndBlock, where U+00A0 is *content*: a paragraph
///       holding one was written to hold something, and that is a different question from where a
///       delimiter may stand.
/// @note A span left holding nothing but whitespace loses its formatting rather than its bytes, which is
///       5.5's "never emit delimiters around empty or whitespace-only content" made structural: after
///       this pass a formatted span always begins and ends with a byte a delimiter may touch, so the
///       emitter needs no test of its own.
/// @note Nothing is hoisted inside a fenced code block. Its content is literal, no delimiter is written
///       around it, and its leading whitespace is the indentation of the code.
/// @note A merge never crosses a link's brackets or an image, and no rule here says so.
///       CONVERSION_REFERENCE 5.1 asks that runs be coalesced *within* a hyperlink, and M7's link
///       markers are spans, so the two text spans on either side of one are not adjacent in the output.
///       An **anchor** is the exception and is transparent: it emits an element of its own between the
///       two runs without putting anything between their text, and Word writes a bookmark in the middle
///       of a word often enough that stopping there would emit "**Hel****lo**".
/// @note A **muted** span is transparent too, and it is why this pass is run twice. A muted span emits
///       nothing at all, so the text on either side of one *is* adjacent in the output -- but muting is
///       LinkResolve's, and LinkResolve runs after this pass has already refused the merge on the
///       strength of brackets that will not survive. Convert therefore calls this again once the muting
///       is done. Left unmerged, a bold run on either side of a muted link emitted "**A****B**" -- the
///       fragmentation defect correctness rule 4 exists to prevent, arriving through the back door --
///       and an entity split across the pair went unescaped, because MdEscape's lookahead is span-local
///       and cannot see the "amp;" beginning the span after the one it is writing.
/// @note The arena is never rewritten. A merge extends the first span over the second, which is sound
///       only because the walker appends every span's bytes in span order and never leaves a gap -- so
///       the pass checks the two ranges really do meet and declines to merge if they ever do not, rather
///       than trusting an invariant a later milestone could quietly break.
cbool RunCoalesce(IR_DOCUMENTptrc document);
