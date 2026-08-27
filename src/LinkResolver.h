/*
 * File: LinkResolver.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: Reference resolution: relationship ids into destinations, bookmarks into GFM anchors.
 * To Do: 1) Resolve a reference against the part it was read in once M10 walks a second part, whose
 *           relationship ids are scoped separately from the body's.
 *        2) Synthesise a destination from a HYPERLINK field instruction when M10 runs the field machine.
 *        3) Fold beyond the simple one-to-one case mappings, for the few code points whose lower-case
 *           form is more than one character.
 * Dependencies: Ir.h, OpcPackage.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Ir.h"
#include "OpcPackage.h"

//== Limits

/// The longest slug or anchor name this module will build. GitHub imposes no limit and Word caps a
/// bookmark at forty characters; a heading long enough to reach this has a slug no reader would type.
constexpr cui64 LINK_MAX_NAME_BYTES = 512u;

//== Entry points

/// Turns every relationship reference the walk recorded into a destination.
/// @param document   A document DocWalk has filled and RunCoalesce has been over.
/// @param package    The package the part belongs to.
/// @param partIndex  The part the references were read in, whose relationships they are scoped to.
/// @return true when the pass finished, false when it could not allocate.
/// @note This is where correctness rule 1 is kept for content. An r:id means nothing on its own: ids are
///       scoped per part, so the walk records what it read and the lookup happens where the part is
///       known. The part's relationships must already have been loaded.
/// @note What each kind of reference becomes. A hyperlink to an External target becomes that URI, with
///       the w:anchor fragment appended when the element carried both. A hyperlink to a part inside the
///       package becomes nothing -- Markdown has no way to address one, and the honest degradation is to
///       keep the text and drop the brackets. An image becomes the resolved part name, marked
///       IR_SPAN_FLAG_PART for MediaPlan to turn into a file path, or the URI when the target is
///       External. A reference that resolves to nothing at all becomes an empty destination, which every
///       later stage treats as "no link": CONVERSION_REFERENCE 5.4's "dangling refs degrade gracefully",
///       applied to references rather than to numbering.
cbool LinkResolveRefs(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, csi32 partIndex);

/// Turns every bookmark into an anchor a link can reach, and every internal link into that anchor.
/// @param document  A document whose references LinkResolveRefs has already turned into destinations.
/// @return true when the pass finished, false when it could not allocate.
/// @note Mapping row 22's two halves. A bookmark that sits in a heading resolves to that heading's own
///       GFM slug, so the link needs no markup at all; one that sits anywhere else resolves to its own
///       name and the anchor is emitted as an <a id> element where it stands.
/// @note An anchor nothing points at is muted rather than emitted, which is what keeps Word's generated
///       bookmarks -- _GoBack in every saved document, a _Toc target in every heading -- out of the
///       output without this having to know their names. IrDropEmptyBlocks then removes a block that
///       held nothing else, so a paragraph that was only ever a bookmark costs nothing.
/// @note A link naming a bookmark the document does not contain loses its brackets and keeps its text,
///       the same degradation a dangling relationship gets.
cbool LinkResolveAnchors(IR_DOCUMENTptrc document);

/// Builds the GFM heading slug of a run of text.
/// @param text       The heading's text, with no markup in it.
/// @param byteCount  How many bytes it is.
/// @param dest       Receives the slug, NUL-terminated.
/// @param destBytes  Bytes available at dest, terminator included.
/// @return How many bytes the slug occupies, not counting the terminator.
/// @note This has to agree with the renderer, because the heading's own anchor is the renderer's to
///       generate: we write the link and GitHub writes the target. The rule is github-slugger's -- lower
///       case, then every character that is not a letter, a mark, a decimal digit or connector
///       punctuation removed, then each space turned into a hyphen, with a literal hyphen kept. Both
///       halves come from the Unicode character database rather than from a guess, for the reason M6
///       recorded about its punctuation table: a hand-picked subset is wrong in both directions, and here
///       being wrong means a link that scrolls nowhere. An apostrophe is the case that makes it matter --
///       "Don't Panic" is "dont-panic", and a converter that kept the right single quotation mark would
///       write a fragment no heading answers to.
/// @note A decimal digit and not every number, which is the one place a plausible reading of the rule is
///       wrong. github-slugger's removal class settles it inside Latin-1 by itself: it takes out U+00B2,
///       U+00B3, U+00B9 and U+00BC..U+00BE -- the superscripts and the vulgar fractions, every one of
///       them category No -- while leaving U+00AA, U+00B5 and U+00BA, every one of them a letter,
///       standing in the gaps between those ranges. Bengali says it twice over, with the digits
///       U+09E6..U+09EF kept and the currency numerators U+09F4..U+09F9 beside them dropped.
/// @note The padding at the two ends of a heading is dropped before any of that, and the padding inside
///       it is not. An ATX heading's content is its line stripped of leading and trailing whitespace, so
///       "# Intro " is the heading "Intro" and a renderer slugs "intro" where the untrimmed text gives
///       "-intro-" -- an anchor the page does not have. Interior padding becomes one hyphen per space,
///       and a character the keep test drops does not interrupt the run: "A ! B" is "a--b", because the
///       renderer removes the mark and leaves the two spaces adjacent.
/// @note Pure: it touches no document and allocates nothing, which is what makes it the piece the unit
///       tests can hammer directly.
cui64 LinkSlug(cchptr text, cui64 byteCount, chptrc dest, cui64 destBytes);

/// Builds the anchor name an <a id> element and the link that reaches it both use.
/// @param name       The bookmark name as the document spelled it.
/// @param byteCount  How many bytes it is.
/// @param dest       Receives the sanitised name, NUL-terminated.
/// @param destBytes  Bytes available at dest, terminator included.
/// @return How many bytes the name occupies, not counting the terminator.
/// @note Word allows a bookmark name only letters, digits and the underscore, so for every document a
///       word processor wrote this is the identity. It is not the identity for a hand-built part, which
///       is the reason it exists: the name reaches both an HTML attribute and a URL fragment, and a
///       quotation mark in the first or a space in the second would break the document around it.
///       Anything outside the safe set becomes a hyphen, and a name that comes to nothing is refused by
///       returning zero, so the link degrades to plain text rather than pointing at "#".
/// @note Pure: it touches no document and allocates nothing.
cui64 LinkAnchorName(cchptr name, cui64 byteCount, chptrc dest, cui64 destBytes);
