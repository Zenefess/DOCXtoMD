/*
 * File: OpcPackage.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: The OPC package model: content types, relationship graphs, and main-part discovery.
 * To Do: 1) Add an uncached part read for MediaExtractor at M7, which streams an image out once.
 *        2) Add the content-type to file-extension table when M7 first names an extracted image.
 *        3) Release a cached part on demand, once a document large enough to want it turns up.
 * Dependencies: Diag.h, Utf.h, XmlPull.h, ZipReader.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Diag.h"
#include "Utf.h"
#include "XmlPull.h"
#include "ZipReader.h"

//== Limits

/// How many Default or Override rows [Content_Types].xml may carry. Word writes a handful of Defaults
/// and one Override per typed part; a package needing thousands of either is not a document.
constexpr cui32 OPC_MAX_TYPE_ROWS = 4096u;

/// How many relationships the whole package may hold. A document with a hyperlink on every line is the
/// case that makes this large: each one is a relationship in the main part's own .rels.
constexpr cui32 OPC_MAX_RELS = 65536u;

/// The longest relationship Target that will be resolved, and the longest part name that can come out.
constexpr cui64 OPC_MAX_TARGET_BYTES = 2048u;
constexpr cui64 OPC_MAX_PART_BYTES   = 1024u;

/// How many officeDocument relationships the package may declare before it is refused. ISO/IEC 29500-2
/// allows exactly one; the ceiling exists so a package declaring tens of thousands of them cannot make
/// discovery cost one scan of the part table each.
constexpr cui32 OPC_MAX_MAIN_CANDIDATES = 8u;

/// How many path segments a resolved part name may have.
constexpr cui32 OPC_MAX_SEGMENTS = 64u;

/// The part index that means the package itself rather than one of its parts, which is what the
/// relationships in _rels/.rels belong to.
constexpr csi32 OPC_PACKAGE_PART = -1;

//== Results

/// Why a package operation stopped. Every value but OPC_OK means nothing usable was produced.
/// @note OpcExitCode maps these onto the process exit codes. ZipReader documents its own mapping in a
///       note and leaves the switch to main.cpp; this module has three times as many values, so it
///       carries the mapping in code instead. That divergence is deliberate.
enum OPC_RESULT : si32 {
   OPC_OK = 0,                        ///< The operation succeeded
   OPC_ERROR_MEMORY,                  ///< An allocation failed
   OPC_ERROR_RANGE,                   ///< A part or relationship index outside the package was asked for
   OPC_ERROR_ZIP,                     ///< The container layer refused; the package records which way
   OPC_ERROR_NO_CONTENT_TYPES,        ///< The package has no [Content_Types].xml
   OPC_ERROR_NO_PACKAGE_RELS,         ///< The package has no _rels/.rels
   OPC_ERROR_CONTENT_TYPES_MALFORMED, ///< [Content_Types].xml is not a usable Types part
   OPC_ERROR_RELS_MALFORMED,          ///< A relationships part is not a usable Relationships part
   OPC_ERROR_NOT_UTF8,                ///< A part is not well-formed UTF-8
   OPC_ERROR_XML,                     ///< A part is not well-formed XML; the package records which rule
   OPC_ERROR_NO_MAIN_REL,             ///< _rels/.rels declares no main document part
   OPC_ERROR_MAIN_PART_MISSING,       ///< The main document part the package names is not in the archive
   OPC_ERROR_REL_TARGET,              ///< A relationship target is not a part name this reader will accept
   OPC_ERROR_LIMIT,                   ///< A structural cap was reached, so the package is refused
   OPC_RESULT_COUNT                   ///< Number of values above; not a result
};

/// Constant form of OPC_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const OPC_RESULT cOPC_RESULT;

//== Relationships

/// The relationship types this converter acts on. Both the Transitional and the Strict URI families map
/// onto the same value, exactly as the XML namespaces do.
enum OPC_REL_KIND : si32 {
   OPC_REL_OTHER = 0,       ///< A relationship kind this build does not act on
   OPC_REL_OFFICE_DOCUMENT, ///< The main document part; this is what discovery follows
   OPC_REL_STYLES,          ///< word/styles.xml
   OPC_REL_NUMBERING,       ///< word/numbering.xml
   OPC_REL_SETTINGS,        ///< word/settings.xml
   OPC_REL_FOOTNOTES,       ///< word/footnotes.xml
   OPC_REL_ENDNOTES,        ///< word/endnotes.xml
   OPC_REL_COMMENTS,        ///< word/comments.xml
   OPC_REL_IMAGE,           ///< A media part, or an external image URL
   OPC_REL_HYPERLINK,       ///< Almost always external
   OPC_REL_HEADER,          ///< A header part, which the mapping policy skips
   OPC_REL_FOOTER,          ///< A footer part, which the mapping policy skips
   OPC_REL_THEME,           ///< word/theme/theme1.xml
   OPC_REL_KIND_COUNT       ///< Number of values above; not a kind
};

/// Constant form of OPC_REL_KIND, spelled per GCS r2.
typedef const OPC_REL_KIND cOPC_REL_KIND;

/// One relationship, handed to a caller by value. The pointers are into the package's own string heap
/// and stay valid until the next OpcLoadRels grows it, so read what is wanted before loading more.
struct OPC_REL_VIEW {
   cchptr       id;       ///< The Id attribute, which r:id references match against
   cchptr       type;     ///< The Type URI exactly as written
   cchptr       target;   ///< The Target exactly as written, resolved or not
   cchptr       part;     ///< The resolved part name, leading '/' included; empty when external
   OPC_REL_KIND kind;     ///< Which relationship type the URI names
   bool         external; ///< Whether TargetMode said External, so target is a URI and not a part
};

/// Constant form of OPC_REL_VIEW, spelled per GCS r2.
typedef const OPC_REL_VIEW cOPC_REL_VIEW;

//== Package

/// One relationship as the package stores it: offsets into the string heap, which a growing heap does
/// not invalidate the way it would invalidate pointers.
struct OPC_REL {
   ui32         idAt;       ///< Heap offset of the Id
   ui32         typeAt;     ///< Heap offset of the Type URI
   ui32         targetAt;   ///< Heap offset of the Target as written
   ui32         resolvedAt; ///< Heap offset of the resolved part name; the empty string when external
   OPC_REL_KIND kind;       ///< Which relationship type the URI names
   bool         external;   ///< Whether TargetMode said External
};

/// One row of [Content_Types].xml: an extension or a part name, and what it types.
struct OPC_TYPE_ROW {
   ui32 keyAt;  ///< Heap offset of the extension (a Default row) or the part name (an Override row)
   ui32 typeAt; ///< Heap offset of the content type
};

/// One part of the package. There is exactly one per central directory record, in the same order, so a
/// part index and an entry index are the same number and nothing has to map between them.
struct OPC_PART {
   cchptr name;          ///< The entry name, owned by the reader; no leading '/'
   ui8ptr bytes;         ///< The inflated bytes once the part has been loaded, otherwise null
   ui64   byteCount;     ///< Bytes at bytes
   ui32   contentTypeAt; ///< Heap offset of the resolved content type; 0 is the empty string
   ui32   relsAt;        ///< Where this part's relationships begin in the package's array
   ui32   relCount;      ///< How many it has
   bool   relsLoaded;    ///< Whether the lookup has been done, which a part with none also sets
   bool   typeKnown;     ///< Whether the content type has been resolved; a part may legitimately have none
};

/// Constant and pointer forms of the package's records, spelled per GCS r2/t2.
typedef OPC_PART           *OPC_PARTptr;
typedef OPC_REL            *OPC_RELptr;
typedef const OPC_REL       cOPC_REL;
typedef OPC_TYPE_ROW       *OPC_TYPE_ROWptr;
typedef OPC_TYPE_ROW      **OPC_TYPE_ROWptrptr;
typedef const OPC_TYPE_ROW *cOPC_TYPE_ROWptr;

/// One package built over one opened archive. A worker owns one of these and never shares it (D6), so
/// nothing here takes a lock.
struct al32 OPC_PACKAGE {
   ZIP_READERptr   reader;            ///< Borrowed, not owned: OpcClose leaves it open
   OPC_PARTptr     parts;             ///< One per entry, in central directory order
   chptr           heap;              ///< Every string this model owns, addressed by offset
   OPC_TYPE_ROWptr defaults;          ///< Default rows of [Content_Types].xml, by extension
   OPC_TYPE_ROWptr overrides;         ///< Override rows, by part name
   OPC_RELptr      rels;              ///< Every relationship loaded so far, grouped by part
   ui64            heapUsed;          ///< Bytes of heap in use
   ui64            heapCapacity;      ///< Bytes allocated at heap
   ui64            defaultCapacity;   ///< Rows allocated at defaults
   ui64            overrideCapacity;  ///< Rows allocated at overrides
   ui64            relCapacity;       ///< Relationships allocated at rels
   ui32            partCount;         ///< Entries in parts
   ui32            defaultCount;      ///< Rows in defaults
   ui32            overrideCount;     ///< Rows in overrides
   ui32            relCount;          ///< Relationships in rels
   ui32            packageRelsAt;     ///< Where the package's own relationships begin
   ui32            packageRelCount;   ///< How many the package itself has
   si32            mainPart;          ///< Part index of the main document part, or -1
   si32            failedPart;        ///< The part a failure was found in, or -1 when none is named
   ZIP_RESULT      lastZip;           ///< How the container layer last refused, for the message
   XML_RESULT      lastXml;           ///< Which XML rule a part last broke, for the message
   UTF8_RESULT     lastUtf;           ///< Which UTF-8 rule a part last broke, for the message
   bool            packageRelsLoaded; ///< Whether _rels/.rels has been read
   char            message[512];      ///< Where a sentence naming the failing part is composed
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores. Today's size does not land on one, so the scalar path runs and the alignment happens
// not to matter -- which is exactly the kind of accident a single added field turns into a fault. al32
// removes the accident, and the assertion keeps it removed.
static_assert(alignof(OPC_PACKAGE) >= 32u, "OpcPackage: OPC_PACKAGE is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of OPC_PACKAGE, spelled per GCS r2/t2.
typedef OPC_PACKAGE       *OPC_PACKAGEptr;
typedef const OPC_PACKAGE *cOPC_PACKAGEptr;
typedef OPC_PACKAGE *const OPC_PACKAGEptrc;

//== Entry points

/// Builds the package model over an already-opened archive, and resolves the main document part.
/// @param package  Receives the model. Every field is written before anything can fail, so OpcClose is
///                 safe to call whatever this returns.
/// @param reader   An archive ZipOpen has already accepted. It is borrowed, not owned: OpcClose leaves
///                 it open, and the caller closes the package first and the reader second.
/// @return OPC_OK, or why the package was refused.
/// @note This is where correctness rule 1 is kept: the only two names read by name are the two ISO/IEC
///       29500-2 guarantees, and the main document part comes from the officeDocument relationship in
///       _rels/.rels, cross-checked against [Content_Types].xml but never replaced by it.
/// @note [Content_Types].xml takes over in exactly one case: the officeDocument relationship resolved
///       to a part the archive does not contain. A target that was refused outright, or one declared
///       External, never reaches that path -- it fails the package instead, so a traversal target can
///       never turn into a silent conversion of whichever part happened to be typed as the body.
/// @note Relationship attribute values reach this module's string heap NUL-terminated, which is safe
///       only because XmlPull refuses a NUL in a value: a NUL is well-formed UTF-8, so UtfValidate
///       alone would let one through and truncate a Target at it.
cOPC_RESULT OpcOpen(OPC_PACKAGEptrc package, ZIP_READERptrc reader);

/// Releases everything the model holds, and leaves it safe to close again. The archive stays open.
/// @param package  A package previously passed to OpcOpen, whatever that returned.
void OpcClose(OPC_PACKAGEptrc package);

/// Finds a part by name.
/// @param package   An opened package.
/// @param partName  A part name, with or without its leading '/'.
/// @return The part index, or -1 when the package has no such part.
/// @note OPC compares part names case-insensitively, so this does too; ZipFindEntry deliberately does
///       not, which is why part lookup lives here rather than there.
/// @note docs/CONVERSION_REFERENCE.md 6.2 [3] prescribes a lowercase key heap with the original name
///       kept beside it. This folds on comparison instead and keeps no keys at all, which deletes an
///       allocation, its sizing pass and its failure path; with a package of twenty parts the cost is
///       unmeasurable. The divergence is deliberate and is recorded here rather than left to be found.
csi32 OpcFindPart(cOPC_PACKAGEptr package, cchptr partName);

/// The entry name of one part, without a leading '/'.
/// @param package    An opened package.
/// @param partIndex  A part index.
/// @return The name, or an empty string for an index outside the package.
cchptr OpcPartName(cOPC_PACKAGEptr package, csi32 partIndex);

/// The content type of one part, resolved through Override then Default.
/// @param package    An opened package.
/// @param partIndex  A part index.
/// @return The content type, or an empty string when nothing types the part. Never null.
/// @note Resolved on the first ask and remembered, rather than for every part when the package opens.
///       An eager pass costs parts times Override rows -- ten thousand by four thousand at the caps,
///       which is seconds of work on a package that is about to be accepted. Only a handful of parts is
///       ever asked about, so doing it lazily removes the product rather than merely shrinking it.
/// @note An Override on the part's own name wins, then a Default for its extension. From M7 this is
///       what names an extracted image's file extension: a media part's true type comes from here
///       and never from the extension its ZIP entry name happens to carry, which producers get wrong.
cchptr OpcContentTypeOf(OPC_PACKAGEptrc package, csi32 partIndex);

/// The main document part.
/// @param package  An opened package.
/// @return Its part index, which OpcOpen has already proved exists.
csi32 OpcMainPart(cOPC_PACKAGEptr package);

/// Inflates one part and caches it on the package.
/// @param package    An opened package.
/// @param partIndex  A part index.
/// @return OPC_OK, or why the part could not be read. Loading a part twice does nothing the second time.
/// @note The bytes are borrowed, not given: they live until OpcClose and must not be freed by a caller.
///       Caching is a correctness matter and not only a speed one -- ZipReadEntry charges its
///       decompression cap on every read and never credits it back, so re-reading a part repeatedly
///       would walk an innocent document into the bomb caps.
/// @note These bytes have not been checked for anything. Nothing may be tokenised out of them until
///       OpcLoadXmlPart has run: that is where M4's definition of done -- an ill-formed part refused
///       rather than walked -- is actually kept, and this entry point is for media and nothing else.
cOPC_RESULT OpcLoadPart(OPC_PACKAGEptrc package, csi32 partIndex);

/// Inflates one part, checks that it is text, and transcodes it when it turns out to be UTF-16.
/// @param package    An opened package.
/// @param partIndex  A part index.
/// @return OPC_OK, or why the part could not be used. OPC_ERROR_NOT_UTF8 is the one M4's definition of
///         done names: a part carrying ill-formed UTF-8 is refused here, with the rule it broke recorded
///         for the message, rather than being handed to a tokenizer that would read nonsense out of it.
/// @note Every XML part goes through this rather than through OpcLoadPart, so nothing is ever tokenised
///       before it has been proved to be text. A UTF-16 part is transcoded in place, so a caller always
///       sees UTF-8 whatever the producer wrote.
cOPC_RESULT OpcLoadXmlPart(OPC_PACKAGEptrc package, csi32 partIndex);

/// The cached bytes of a part, and how many there are.
/// @param package    An opened package.
/// @param partIndex  A part index.
/// @return The bytes, or null until OpcLoadPart has been called for that part.
cui8ptr OpcPartBytes(cOPC_PACKAGEptr package, csi32 partIndex);

/// How many bytes a loaded part holds.
/// @param package    An opened package.
/// @param partIndex  A part index.
/// @return The number of bytes, or 0 until OpcLoadPart has been called for that part.
cui64 OpcPartByteCount(cOPC_PACKAGEptr package, csi32 partIndex);

/// Reads the relationships of one part, or of the package itself.
/// @param package    An opened package.
/// @param partIndex  A part index, or OPC_PACKAGE_PART for the relationships in _rels/.rels.
/// @return OPC_OK, or why they could not be read. A part with no relationships part of its own is not a
///         failure: it has none, which is recorded so the lookup is never repeated.
/// @note Relationship ids are scoped per part -- rId3 in document.xml and rId3 in footnotes.xml are
///       unrelated -- which is why every lookup here takes the part the reference was found in.
cOPC_RESULT OpcLoadRels(OPC_PACKAGEptrc package, csi32 partIndex);

/// How many relationships a part has, once they have been loaded.
cui32 OpcRelCount(cOPC_PACKAGEptr package, csi32 partIndex);

/// The index of one of a part's relationships, for OpcRel.
/// @return The package-wide relationship index, or -1 when there is no such relationship.
csi32 OpcRelAt(cOPC_PACKAGEptr package, csi32 partIndex, cui32 index);

/// Finds one of a part's relationships by its Id.
/// @return The package-wide relationship index, or -1 when the part declares no such Id.
csi32 OpcFindRelById(cOPC_PACKAGEptr package, csi32 partIndex, cchptr id);

/// Finds the first of a part's relationships of a given kind.
/// @return The package-wide relationship index, or -1 when the part declares none.
csi32 OpcFindRelByKind(cOPC_PACKAGEptr package, csi32 partIndex, cOPC_REL_KIND kind);

/// One relationship, by package-wide index.
/// @return Its fields. An index outside the package yields a view of empty strings and OPC_REL_OTHER.
cOPC_REL_VIEW OpcRel(cOPC_PACKAGEptr package, csi32 relIndex);

/// Resolves a relationship target against the part it was declared in.
/// @param sourcePartName      The part holding the relationship, leading '/' included; "/" is the package.
/// @param target              The Target attribute exactly as written.
/// @param external            Whether TargetMode said External, in which case the target is copied through.
/// @param resolved            Receives the resolved part name, leading '/' included, NUL-terminated.
/// @param resolvedCapacity    Bytes available at resolved, terminator included.
/// @return OPC_OK, or OPC_ERROR_REL_TARGET when the target is not a part name this reader will accept.
/// @note Pure: it touches no package and allocates nothing, which is what makes it the piece the unit
///       tests can hammer directly.
/// @note Dot segments are removed inside the package namespace only, and one that would climb above the
///       root is refused rather than clamped -- RFC 3986 discards it silently, but a target that escapes
///       the package is either hostile or broken, and turning ../../../x into /x reports the wrong thing.
/// @note Percent escapes are decoded after the dot segments are removed and the result is re-checked,
///       which is what closes the %2e%2e bypass. A scheme -- anything matching letter, then letters,
///       digits, '+', '-' or '.', then ':' -- is refused outright, and that one rule covers drive
///       letters, file:// URLs and an http:// target mislabelled Internal.
cOPC_RESULT OpcResolveTarget(cchptr sourcePartName, cchptr target, cbool external, chptrc resolved, cui64 resolvedCapacity);

/// The name of the relationships part belonging to a part.
/// @param partName       The part, leading '/' included; "/" means the package itself.
/// @param relsName       Receives the relationships part name, leading '/' included, NUL-terminated.
/// @param relsCapacity   Bytes available at relsName, terminator included.
/// @return OPC_OK, or OPC_ERROR_REL_TARGET when the name will not fit or is not a part name.
cOPC_RESULT OpcRelsPartName(cchptr partName, chptrc relsName, cui64 relsCapacity);

/// Composes a sentence that names the part it is about.
/// @param package    The package, whose own buffer the sentence is built in.
/// @param sentence   The sentence to say, with no trailing punctuation.
/// @param partIndex  The part to name; -1 yields the sentence unchanged.
/// @return The composed sentence, valid until the next call on the same package.
/// @note A message that says which part broke is the difference between a diagnosis and a shrug, and
///       a package holds a dozen parts. main.cpp uses this too, for the failures it reports itself.
cchptr OpcMessageIn(OPC_PACKAGEptrc package, cchptr sentence, csi32 partIndex);

/// The user-facing sentence for a result, ready to hand to DiagErrorText.
/// @param package  The package the result came from; a null pointer still yields a usable sentence.
/// @param result   The result to describe.
/// @return A NUL-terminated ASCII sentence with no trailing punctuation, naming the part it is about
///         when one is known. The container and XML layers' own reasons are folded in, so one call
///         gives the best message available. It is valid until the next call on the same package.
cchptr OpcResultText(OPC_PACKAGEptrc package, cOPC_RESULT result);

/// The process exit code a result maps onto.
/// @param package  The package the result came from; a null pointer maps OPC_ERROR_ZIP to exit 3.
/// @param result   The result to map.
/// @return EXIT_NOT_DOCX for everything the document is to blame for, EXIT_INTERNAL for this program's
///         own failures, and whatever ZipReader's documented rule says for a container refusal.
cEXIT_CODE OpcExitCode(cOPC_PACKAGEptr package, cOPC_RESULT result);
