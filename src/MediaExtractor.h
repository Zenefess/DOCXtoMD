/*
 * File: MediaExtractor.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: Media extraction: image parts to files beside the document, and the paths that reach them.
 * To Do: 1) Deduplicate by content as well as by part, for a producer that stores one picture twice.
 *        2) Name a file after the document as well as its number, if M13's Batch does not pre-flight a
 *           shared --media-dir the way it pre-flights a shared output name.
 *        3) Emit the wp:extent size as an HTML img element where a document depends on it (row 23).
 * Dependencies: Diag.h, Ir.h, OpcPackage.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Diag.h"
#include "Ir.h"
#include "OpcPackage.h"

//== Limits

/// How many distinct image parts one document may extract. A document drawing more pictures than this is
/// not one anybody wrote; the ceiling exists so a hostile package cannot size an allocation.
constexpr cui32 MEDIA_MAX_FILES = 4096u;

/// The longest generated file name, terminator included: "image" plus a number plus an extension.
constexpr cui64 MEDIA_MAX_NAME_BYTES = 32u;

//== The plan

/// Which package part each generated file comes from. One entry per file, in the order the document
/// first draws them, so the numbering a reader sees follows the document rather than the archive.
struct al32 MEDIA_SET {
   si32ptr parts;    ///< The part index behind each file
   chptr   names;    ///< One NUL-terminated leaf name per file, in fixed-width slots
   ui32    count;    ///< Files planned
   ui32    capacity; ///< Entries allocated
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives. al32 says so, and the
// assertion below keeps it said whatever a later field does to the size.
static_assert(alignof(MEDIA_SET) >= 32u, "MediaExtractor: MEDIA_SET is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of MEDIA_SET, spelled per GCS r2/t2.
typedef MEDIA_SET       *MEDIA_SETptr;
typedef const MEDIA_SET *cMEDIA_SETptr;
typedef MEDIA_SET *const MEDIA_SETptrc;

//== Entry points

/// Prepares an empty plan.
/// @param set  Receives the plan. Every field is written, so it need not be initialised, and MediaClose
///             is safe to call afterwards whatever happens next.
void MediaOpen(MEDIA_SETptrc set);

/// Releases everything the plan holds, and leaves it safe to close again.
/// @param set  A plan previously passed to MediaOpen.
void MediaClose(MEDIA_SETptrc set);

/// Gives every image drawn from the package a file name, and rewrites the spans to point at it.
/// @param set          A prepared plan; it is filled with one entry per distinct part.
/// @param document     A document whose references LinkResolve has already turned into part names.
/// @param package      The package the parts belong to.
/// @param linkPrefix   What stands in front of the file name in the emitted path, with no separator of
///                     its own; an empty string puts the files beside the document.
/// @param emitImages   false turns every image into its alt text instead. Two things ask for that and no
///                     others: --no-images, and a document whose media directory could not be derived at
///                     all. --stdout is *not* one of them -- D13 rules that it extracts its pictures
///                     beside the input, so that the piped document and a written one are the same bytes.
/// @return true when the pass finished, false when it could not allocate.
/// @note Nothing is read or written here: the extension comes from the content type the package already
///       knows, and the bytes are not inflated until MediaWrite. A document converted with --no-images
///       therefore never decompresses a picture at all.
/// @note An image is named imageN by the order the document first draws it, and a part drawn twice keeps
///       one file (CONVERSION_REFERENCE 5.8). The extension is the content type's and never the entry
///       name's, because a producer -- Google Docs in particular -- writes entry names whose extension
///       does not match what is inside them.
/// @note An image whose part the archive does not hold, and one whose reference resolved to nothing,
///       degrades to its alt text exactly as --no-images does. Neither is a refusal: a picture that
///       cannot be found is a defect in the document, not in the conversion.
/// @note So does an image inside a fenced code block, and that one is decided here rather than left to
///       the emitter. A fence emits its text and nothing else, so planning the picture would put a file
///       on disk that no line of the document refers to -- and drop the alt text with it.
/// @note The path written into the document percent-encodes '#', '%' and '?'. MD_CONTEXT_LINK_DEST
///       leaves those three alone deliberately, because a producer's own target arrives already encoded
///       far more often than it arrives holding a literal one; none of that holds for a name derived
///       here, where all three are ordinary bytes of a file name and a document called "draft #2.docx"
///       would otherwise link its pictures to a fragment of itself.
cbool MediaPlan(MEDIA_SETptrc set, IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cchptr linkPrefix, cbool emitImages);

/// Writes every planned file into the media directory, creating it if it is not there.
/// @param set        A plan MediaPlan has filled.
/// @param package    The package the parts come from; its bytes are inflated here and not before.
/// @param mediaDir   Where the files go, as a path Win32 will accept. It is created if absent.
/// @return EXIT_ALL_CONVERTED, or EXIT_OUTPUT when a file could not be written, or EXIT_NOT_DOCX when a
///         part could not be inflated. Every failure has already been reported.
/// @note Called after the document itself has been written, so a conversion that fails *before this
///       point* leaves no files behind at all. A half-written picture is deleted the way a half-written
///       .md is. The other order is not promised and should not be read into the first: a media
///       directory that cannot be created leaves the .md beside pictures it names and does not have,
///       which is the right way round -- the text is what the conversion was for -- and the caller still
///       reports the run as failed.
/// @note The plan owns its file names rather than borrowing the document's arena, which is what lets
///       the intermediate representation be released the moment the Markdown is out of it.
cEXIT_CODE MediaWrite(cMEDIA_SETptr set, OPC_PACKAGEptrc package, cwchptr mediaDir);

/// The file extension one content type names, including its dot.
/// @param contentType  The content type as [Content_Types].xml gave it, which may carry parameters.
/// @param fallback     The part name, whose own extension is used when the content type names none.
/// @param dest         Receives the extension, NUL-terminated, dot included.
/// @param destBytes    Bytes available at dest, terminator included.
/// @return How many bytes the extension occupies, not counting the terminator.
/// @note CONVERSION_REFERENCE 1.2's table, plus the two raster formats it predates. A type this build
///       does not know falls back to the part name's own extension when that is short and alphanumeric,
///       and to ".bin" when it is not -- a name a Markdown renderer will not display either way, which
///       is honest about a picture nothing can show rather than inventing a format for it.
/// @note Pure: it touches no package and allocates nothing, which is what makes it the piece the unit
///       tests can hammer directly.
cui64 MediaExtension(cchptr contentType, cchptr fallback, chptrc dest, cui64 destBytes);
