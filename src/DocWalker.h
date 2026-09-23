/*
 * File: DocWalker.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-23
 * Description: The document walk: WordprocessingML body and note content into the intermediate representation.
 * To Do: 1) Extract a text box's w:txbxContent in place (row 38), which today is inside a picture
 *           container and so is scanned for a blip and otherwise dropped.
 *        2) Read an unclosed field's end from a pre-scan, if a producer is ever found writing one: a begin
 *           with no end leaves everything after its instruction unread, and streaming cannot undo that.
 *        3) Carry an INCLUDEPICTURE's own URL where its cached result holds no picture.
 *        4) Drop the custom mark a w:customMarkFollows reference is followed by in its run, which today is
 *           emitted as text beside the "[^n]" that already stands for it.
 * Dependencies: Ir.h, NumberingModel.h, OpcPackage.h, StyleModel.h, XmlPull.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Ir.h"
#include "NumberingModel.h"
#include "OpcPackage.h"
#include "StyleModel.h"
#include "XmlPull.h"

//== Results

/// Why the walk stopped. Every value but WALK_OK means no usable document was produced.
enum WALK_RESULT : si32 {
   WALK_OK = 0,           ///< The part was walked from end to end
   WALK_ERROR_MEMORY,     ///< An allocation failed while building the representation
   WALK_ERROR_PART,       ///< The part could not be inflated or is not text; the package records why
   WALK_ERROR_XML,        ///< The part is not well-formed XML; the status carries which rule it broke
   WALK_ERROR_ROOT,       ///< The part's root element is not w:document, so it is not a body part at all
   WALK_ERROR_NOTES_ROOT, ///< A notes part's root element is not the w:footnotes or w:endnotes it must be
   WALK_RESULT_COUNT      ///< Number of values above; not a result
};

/// Constant form of WALK_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const WALK_RESULT cWALK_RESULT;

/// What one walk produced, and why it stopped.
struct WALK_STATUS {
   WALK_RESULT result; ///< Why the walk stopped
   XML_RESULT  xml;    ///< Which XML rule the part broke, when the result is WALK_ERROR_XML
   OPC_RESULT  opc;    ///< How the package refused the part, when the result is WALK_ERROR_PART
   si32        part;   ///< The part the walk read, which a sentence about its bytes names; -1 for none
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
/// @note What M8 adds. A w:pPr's w:numPr is read into the block as the *reference* it is -- the w:numId
///       and the w:ilvl the paragraph wrote, resolved against the style chain but against nothing else.
///       Whether that identifier names a list at all is NumAssignMarkers's question, and it is asked
///       after the walk because a counter cannot be rewound: this walk speculatively enters an
///       mc:Choice and unwinds it again, and a number spent there would be gone.
///       A heading carrying numbering is a heading and nothing else (CONVERSION_REFERENCE 5.4), which
///       is the only kind that cancels it -- a quotation and a line of code both keep their marker,
///       because a paragraph may legitimately be an item of a list *and* be one of those. Row 12's
///       monospace guess does not apply to a paragraph that carries one, and neither does row 25's
///       horizontal rule: a paragraph wearing a list marker did not come to nothing.
/// @note What this build does not walk, and skips whole rather than descending into: the comment
///       references and ranges, w:sym and m:oMath. Comments are dropped by policy (mapping row 30);
///       m:oMath and w:sym have no milestone yet and are the two places text is lost rather than merely
///       unformatted -- both are DocWalker.cpp's To Do item 3. An element this build has never heard of
///       is skipped the same way, which is the OOXML compatibility model.
/// @note What M10 adds. Fields run through a begin/separate/end state machine with a stack, which lives
///       on the walk rather than on a paragraph because a field's result may span several (correctness
///       rule 7): everything between begin and separate is instruction and never content; a HYPERLINK's
///       result, and a REF's that carries \h, becomes a link, closed at the end of each block and opened
///       again at the start of the next; a TOC vanishes result and all, and so does a w:sdt whose
///       w:docPartGallery says it is one; every other field is the result it was showing. w:fldSimple is
///       the same machine in one element. A field's structure is read even inside a hidden run, because
///       Word hides the runs of the PAGEREF inside each TOC entry, w:fldChar and all.
/// @note What else M10 adds, all of it accept-all (correctness rule 8): a paragraph whose mark a tracked
///       change deleted runs on into the next paragraph, which gives the two its classification; a cell
///       a w:cellDel removed is dropped with its content, as a deleted row already was. And a
///       w:footnoteReference or w:endnoteReference becomes a note reference span carrying the w:id as
///       written, which DocWalkNotes and LinkResolveNotes turn into a note and its label.
/// @note What M7 adds. A w:hyperlink becomes a link span pair around its content, carrying the reference
///       as written -- an r:id, a '#' and a w:anchor, or both joined by the '#' that will separate them
///       in the output -- because ids are scoped per part and the lookup belongs where the part is known.
///       A w:drawing, a w:pict, a w:object and an mc:AlternateContent standing in for one become a single
///       image span: the markers inside identify the picture, so both markup families are looked for at
///       once and the first alt text and the first relationship win, which is what emits a picture with
///       two vocabularies exactly once. A container holding no picture reference -- a chart, a diagram,
///       a drawn shape -- comes to nothing, because none of them has a bitmap the document could show.
///       An a:blip counts only as the direct child of a pic:blipFill, which is the DrawingML *picture*
///       vocabulary: the same element under an a:blipFill is the bitmap a drawn shape, a chart wall or a
///       table cell is painted with, and taking it emits a shape's wallpaper as the figure the paragraph
///       shows -- which is also the opposite of the rule in the sentence before this one.
///       A w:bookmarkStart becomes an anchor span where it stood, or is held for the next block when it
///       stood between two; LinkResolve mutes every anchor nothing points at, which is what keeps
///       Word's own _GoBack out of the output without this having to know its name.
/// @note What is descended into although its own meaning is layout this mapping has no spelling for,
///       because dropping it would lose text: the bidirectional containers w:dir and w:bdo, and a w:ruby's
///       w:rubyBase -- its w:rt annotation is printed above the base text, which Markdown has nowhere
///       to put.
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
cWALK_STATUS DocWalk(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, csi32 partIndex);

/// Walks one WordprocessingML body part out of bytes that are already known to be well-formed UTF-8.
/// @param document   A document IrOpen has prepared.
/// @param styles     The style model, which may be empty.
/// @param bytes      The part's UTF-8 bytes; they are not owned and need not outlive the call.
/// @param byteCount  How many there are.
/// @return Why the walk stopped.
/// @note DocWalk is this plus the package read in front of it. The split exists so the walk can be
///       driven from a string literal by the unit suite, which opens no file and builds no package.
cWALK_STATUS DocWalkBytes(IR_DOCUMENTptrc document, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, cui8ptr bytes, cui64 byteCount);

/// Walks the notes of one story, appending the body of every note something already references.
/// @param document   A document DocWalk has filled; each note's blocks are appended after what it holds.
/// @param package    The package the part belongs to.
/// @param styles     The style model, which may be empty.
/// @param numbering  The numbering model, which may be empty.
/// @param partIndex  The notes part, resolved through the main part's footnotes or endnotes relationship
///                   rather than named, or -1 when the document has none.
/// @param kind       Which story the part holds.
/// @return Why the walk stopped. A document that references no note of this story has its part left
///         unread, so a part it does not need cannot refuse it; one that references a note and has no part
///         is WALK_OK too, and the references dangle.
/// @note Only a note something references is read. A notes part holds Word's separators and every note
///       whose reference a user deleted, and GitHub drops a definition nothing references -- so reading
///       one would put its pictures on disk and its list items in the counters for text nobody sees.
/// @note Each note walked becomes an IR_NOTE, and its blocks are ordinary blocks after the body's, each
///       carrying the note's index: a note body may hold anything a body can, and every pass above the
///       walk reads it for the price of reading one flat array. The part's index goes on the note, because
///       a note's relationship ids are scoped to its own part -- rId3 in footnotes.xml is not rId3 in
///       document.xml -- and LinkResolveRefs resolves each block against the part it came from.
/// @note A note's w:type decides whether it is one: a separator, its continuation and the continuation
///       notice are machinery, whatever their w:id. A note is its own story, so a field, a pending
///       paragraph join and a bookmark after its last paragraph all end with it.
cWALK_STATUS DocWalkNotes(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, // What is read
                          csi32 partIndex, cIR_NOTE_KIND kind);                                                               // Which notes

/// Walks the notes of one story out of bytes that are already known to be well-formed UTF-8.
/// @param document   A document holding the references the notes are wanted for.
/// @param styles     The style model, which may be empty.
/// @param numbering  The numbering model, which may be empty.
/// @param bytes      The part's UTF-8 bytes; they are not owned and need not outlive the call.
/// @param byteCount  How many there are.
/// @param kind       Which story the part holds.
/// @param partIndex  The part index to record on each note, or -1.
/// @return Why the walk stopped.
/// @note DocWalkNotes is this plus the package read in front of it, split for the unit suite exactly as
///       DocWalkBytes is split from DocWalk.
cWALK_STATUS DocWalkNotesBytes(IR_DOCUMENTptrc document, cSTYLE_MODELptr styles, cNUM_MODELptr numbering, // What is read
                               cui8ptr bytes, cui64 byteCount, cIR_NOTE_KIND kind, csi32 partIndex);      // Out of what

/// The user-facing sentence for a walk status, ready to hand to DiagErrorText.
/// @param package  The package the walk ran over; a null pointer still yields a usable sentence.
/// @param status   What DocWalk returned.
/// @return A NUL-terminated ASCII sentence with no trailing punctuation, naming the part it is about
///         when one is known -- since M10 a walk reads more than one part, so a sentence about the bytes
///         of one names it. It is valid until the next call on the same package.
/// @note A container or encoding refusal keeps the package's own sentence, which says which rule the
///       bytes broke -- a bad CRC-32, a decompression cap, ill-formed UTF-8. Folding all of those into
///       "the part could not be read" would throw away the only half of the message worth reading.
cchptr DocWalkResultText(OPC_PACKAGEptrc package, cWALK_STATUS status);
