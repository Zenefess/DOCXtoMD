/*
 * File: DocWalker.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: The document walk: WordprocessingML body content into the intermediate representation.
 * To Do: 1) Walk w:tbl into table blocks at M9, and w:hyperlink into link spans at M7.
 *        2) Run the field state machine over w:fldChar and w:instrText at M10, which today are skipped.
 *        3) Save and restore the paragraph classification around a nested paragraph when M9 walks a cell.
 *        4) Honour a deleted paragraph mark by joining the paragraph with the next one (M10).
 * Dependencies: Ir.h, OpcPackage.h, StyleModel.h, XmlPull.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Ir.h"
#include "OpcPackage.h"
#include "StyleModel.h"
#include "XmlPull.h"

//== Results

/// Why the walk stopped. Every value but WALK_OK means no usable document was produced.
enum WALK_RESULT : si32 {
   WALK_OK = 0,       ///< The part was walked from end to end
   WALK_ERROR_MEMORY, ///< An allocation failed while building the representation
   WALK_ERROR_PART,   ///< The part could not be inflated or is not text; the package records why
   WALK_ERROR_XML,    ///< The part is not well-formed XML; the status carries which rule it broke
   WALK_ERROR_ROOT,   ///< The part's root element is not w:document, so it is not a body part at all
   WALK_RESULT_COUNT  ///< Number of values above; not a result
};

/// Constant form of WALK_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const WALK_RESULT cWALK_RESULT;

/// What one walk produced, and why it stopped.
struct WALK_STATUS {
   WALK_RESULT result; ///< Why the walk stopped
   XML_RESULT  xml;    ///< Which XML rule the part broke, when the result is WALK_ERROR_XML
   OPC_RESULT  opc;    ///< How the package refused the part, when the result is WALK_ERROR_PART
};

/// Constant form of WALK_STATUS, spelled per GCS r2.
typedef const WALK_STATUS cWALK_STATUS;

//== Entry points

/// Walks one WordprocessingML body part into a prepared intermediate representation.
/// @param document   A document IrOpen has prepared. Blocks are appended to whatever it already holds.
/// @param package    The package the part belongs to.
/// @param styles     The style model, which may be empty; a document with no styles part is legal.
/// @param partIndex  The main document part, resolved through _rels/.rels rather than named.
/// @return Why the walk stopped.
/// @note What the walk keeps and what it drops, all of it CONVERSION_REFERENCE 2.1 and 2.2: w:ins and
///       w:moveTo are transparent and w:del and w:moveFrom are dropped whole, which is the accept-all
///       policy of correctness rule 8; w:sdt, w:smartTag and w:customXml are transparent at every level;
///       mc:AlternateContent takes its mc:Fallback when it has one, because this build understands no
///       extension namespace and so understands no mc:Choice.
/// @note What M5 does not walk yet, and skips whole rather than descending into: w:tbl, w:drawing,
///       w:pict, the field elements, the note and comment references, w:sym and m:oMath. Each arrives
///       with the milestone that can emit it, except m:oMath and w:sym, which have none yet and are the
///       two places text is lost rather than merely unformatted -- both are DocWalker.cpp's To Do item
///       4. An element this build has never heard of is skipped the same way, which is the OOXML
///       compatibility model.
/// @note What is descended into although its own meaning waits for a later milestone, because dropping
///       it would lose text: w:hyperlink, w:fldSimple, the bidirectional containers w:dir and w:bdo, and
///       a w:ruby's w:rubyBase -- its w:rt annotation is printed above the base text, which Markdown has
///       nowhere to put.
/// @note A lone w:pBdr bottom -- or w:between -- on a paragraph that came to nothing is Word's
///       autoformatted horizontal rule, and the ruled mapping row 25 turns it into "---". "Lone" is
///       enforced: a paragraph wearing a box has borders on its other sides and is not a rule, and a
///       paragraph with a bottom border *and* text is an underlined paragraph and is not one either.
/// @note Three block kinds beyond a paragraph and a heading come out of this walk. A style chain whose
///       role is a quote gives IR_BLOCK_QUOTE and one whose role is code gives IR_BLOCK_CODE (mapping
///       rows 13 and 12); so does a paragraph whose every text-bearing run is set in a monospace family,
///       which is row 12's second detection and is settled here because the font is a run property the
///       intermediate representation does not carry. A heading beats both.
/// @note A run whose effective w:vanish or w:webHidden is on is dropped with its text. Word hides field
///       instructions that way, so keeping them would put raw field codes in the output.
/// @note A run whose effective w:caps is on has its text uppercased, which is mapping row 37 -- caps is
///       a transform on the bytes rather than a delimiter, so it belongs here and not to M6's emitter.
///       w:smallCaps leaves the text as typed, which the same row says.
cWALK_STATUS DocWalk(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cSTYLE_MODELptr styles, csi32 partIndex);

/// Walks one WordprocessingML body part out of bytes that are already known to be well-formed UTF-8.
/// @param document   A document IrOpen has prepared.
/// @param styles     The style model, which may be empty.
/// @param bytes      The part's UTF-8 bytes; they are not owned and need not outlive the call.
/// @param byteCount  How many there are.
/// @return Why the walk stopped.
/// @note DocWalk is this plus the package read in front of it. The split exists so the walk can be
///       driven from a string literal by the unit suite, which opens no file and builds no package.
cWALK_STATUS DocWalkBytes(IR_DOCUMENTptrc document, cSTYLE_MODELptr styles, cui8ptr bytes, cui64 byteCount);

/// The user-facing sentence for a walk status, ready to hand to DiagErrorText.
/// @param package  The package the walk ran over; a null pointer still yields a usable sentence.
/// @param status   What DocWalk returned.
/// @return A NUL-terminated ASCII sentence with no trailing punctuation, naming the part it is about
///         when one is known. It is valid until the next call on the same package.
/// @note A container or encoding refusal keeps the package's own sentence, which says which rule the
///       bytes broke -- a bad CRC-32, a decompression cap, ill-formed UTF-8. Folding all of those into
///       "the part could not be read" would throw away the only half of the message worth reading.
cchptr DocWalkResultText(OPC_PACKAGEptrc package, cWALK_STATUS status);
