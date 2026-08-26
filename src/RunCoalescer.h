/*
 * File: RunCoalescer.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-26
 * Last Modified: 2026-08-26
 * Description: The coalescing pass: adjacent runs merged on equal formatting, whitespace hoisted out.
 * To Do: 1) Stop merging across a hyperlink or a field-result boundary when M7 and M10 introduce one.
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
/// @note Whitespace here is the ASCII space, the tab and U+00A0. The non-breaking space is in the set
///       because CommonMark 0.30 counts every Zs character as whitespace for flanking, so a closing
///       delimiter behind one may not parse -- and moving it outside the delimiters renders the same in
///       every renderer, which is what mapping row 35 asks for. Note the deliberate asymmetry with
///       IrEndBlock, where U+00A0 is *content*: a paragraph holding one was written to hold something,
///       and that is a different question from where a delimiter may stand.
/// @note A span left holding nothing but whitespace loses its formatting rather than its bytes, which is
///       5.5's "never emit delimiters around empty or whitespace-only content" made structural: after
///       this pass a formatted span always begins and ends with a byte a delimiter may touch, so the
///       emitter needs no test of its own.
/// @note Nothing is hoisted inside a fenced code block. Its content is literal, no delimiter is written
///       around it, and its leading whitespace is the indentation of the code.
/// @note The arena is never rewritten. A merge extends the first span over the second, which is sound
///       only because the walker appends every span's bytes in span order and never leaves a gap -- so
///       the pass checks the two ranges really do meet and declines to merge if they ever do not, rather
///       than trusting an invariant a later milestone could quietly break.
cbool RunCoalesce(IR_DOCUMENTptrc document);
