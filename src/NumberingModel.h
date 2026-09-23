/*
 * File: NumberingModel.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-10
 * Last Modified: 2026-09-23
 * Description: numbering.xml as resolved per-numId levels, and the counter pass that turns them into markers.
 * To Do: 1) Read w:lvl/w:pStyle so a paragraph whose style a level names can take that level's ilvl.
 *        2) Keep w:lvlText once a policy wants a literal roman or letter marker rather than a decimal.
 *        3) Carry w:numPr from w:docDefaults, which no producer writes and which needs its own guard.
 *        4) Give a note's lists counters of their own if Word is found numbering them apart from the
 *           body's: NumAssignMarkers keys one counter table on the abstract definition and walks the
 *           blocks in reading order, the body's and then each note's, so a note's items continue the
 *           sequence of a body list over the same definition.
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

//== Limits

/// How many w:abstractNum elements one numbering part may declare, and how many w:num elements. Word
/// writes one abstract definition per list a document has ever carried and rarely passes a few dozen;
/// four thousand is far past any producer and bounds what a hostile part can make this reader allocate,
/// which is the only thing left to bound once every lookup here goes through an index.
constexpr cui32 NUM_MAX_ABSTRACT = 4096u;
constexpr cui32 NUM_MAX_NUMS     = 4096u;

/// How many levels a list definition has. ISO/IEC 29500-1 fixes w:ilvl at 0 to 8, which is nine.
constexpr cui32 NUM_MAX_LEVELS = 9u;

/// How far a w:numStyleLink delegation may be chased. The specification sets no limit and malformed
/// files carry loops, so the walk is bounded exactly as StyleModel bounds a w:basedOn chain.
constexpr cui32 NUM_MAX_DELEGATE = 16u;

/// The longest style identifier kept from a w:numStyleLink. It is only ever handed to StyleFind, which
/// matches against identifiers StyleModel already caps at the same width.
constexpr cui64 NUM_MAX_NAME_BYTES = 256u;

/// The largest number an ordered marker may carry. CommonMark accepts a start of at most nine digits;
/// a longer run of digits is not a list marker at all, so the paragraph would silently stop being a
/// list. A counter past this saturates rather than wrapping, which is what a hostile w:start needs.
constexpr cui32 NUM_MAX_NUMBER = 999999999u;

//== Results

/// Why loading a numbering part stopped. Every value but NUM_OK means no definitions were loaded.
enum NUM_RESULT : si32 {
   NUM_OK = 0,       ///< The part was read, or there was no part to read
   NUM_ERROR_MEMORY, ///< An allocation failed
   NUM_ERROR_PART,   ///< The part could not be inflated or is not text; the package records why
   NUM_ERROR_XML,    ///< The part is not well-formed XML; the model records which rule it broke
   NUM_ERROR_ROOT,   ///< The part's root element is not w:numbering, so it is not a numbering part at all
   NUM_ERROR_LIMIT,  ///< The part declares more definitions than this reader accepts
   NUM_RESULT_COUNT  ///< Number of values above; not a result
};

/// Constant form of NUM_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const NUM_RESULT cNUM_RESULT;

//== Levels

/// What one level's w:numFmt classifies as. The enumeration ST_NumberFormat holds sixty-odd tokens and
/// this reader needs none of them by name: GitHub-Flavored Markdown can spell a bullet, a decimal
/// number and nothing else, so every counting format collapses onto one value.
/// @note An unrecognised token, and an absent w:numFmt, both read as NUM_FORMAT_ORDERED. Every token
///       the specification defines but bullet and none is a counting format, so a token this build has
///       never heard of is far likelier to be one than to be a bullet -- and degrading it to a bullet
///       would throw away ordering the counter already has, while degrading it to a decimal loses only
///       a glyph shape CONVERSION_REFERENCE row 15 says the renderer discards anyway.
enum NUM_FORMAT : si8 {
   NUM_FORMAT_ABSENT  = -1, ///< No definition at all: the numId names no list this model can resolve
   NUM_FORMAT_ORDERED = 0,  ///< A counting format, whatever its glyph; the marker is a decimal number
   NUM_FORMAT_BULLET,       ///< w:numFmt bullet, or a level whose marker is a picture
   NUM_FORMAT_PLAIN         ///< w:numFmt none: an item indented like its neighbours with no marker at all
};

/// Constant form of NUM_FORMAT, spelled per GCS r2.
typedef const NUM_FORMAT cNUM_FORMAT;

/// One level of one list definition, after every w:lvlOverride has been applied.
/// @note start already carries any w:startOverride for this level, because a startOverride is the start
///       for the whole life of the numId that carries it and not a one-shot seed -- ISO/IEC 29500's own
///       gloss on w:start is that the value is used when a level first starts *and whenever it is
///       restarted*. The separate question of *when* it fires is the override mask on the instance.
struct NUM_LEVEL {
   si32       start;   ///< What the level's counter takes when it starts, w:startOverride folded in
   si32       restart; ///< w:lvlRestart: 0 never, N restart only under a level shallower than N, -1 absent
   NUM_FORMAT format;  ///< What the level's marker is
};

/// Constant and pointer forms of NUM_LEVEL, spelled per GCS r2/t2.
typedef const NUM_LEVEL        cNUM_LEVEL;
typedef NUM_LEVEL             *NUM_LEVELptr;
typedef NUM_LEVEL *const       NUM_LEVELptrc;
typedef const NUM_LEVEL       *cNUM_LEVELptr;
typedef const NUM_LEVEL *const cNUM_LEVELptrc;

//== Model

/// One w:abstractNum. Its levels are as the part declared them, before any instance's overrides.
struct NUM_ABSTRACT {
   si32      abstractId;             ///< w:abstractNumId exactly as written
   ui32      linkAt;                 ///< Heap offset of w:numStyleLink's style id; the empty string when none
   si32      delegate;               ///< Where the delegation chase ended: itself, or -1 when it did not end
   NUM_LEVEL levels[NUM_MAX_LEVELS]; ///< One per w:lvl the definition declared
};

/// One w:num, resolved: the abstract definition it names, that definition's levels with this instance's
/// w:lvlOverride elements applied, and which levels carry a pending restart.
struct NUM_INSTANCE {
   si32      numId;                  ///< w:numId exactly as written
   si32      abstractId;             ///< w:abstractNumId as written, or -1 when the num declares none
   si32      counterKey;             ///< Which counter row this instance shares; never -1
   ui16      overrideMask;           ///< One bit per level carrying a w:startOverride, for the first-use reset
   NUM_LEVEL levels[NUM_MAX_LEVELS]; ///< The resolved levels, every one of them defined
};

/// Constant and pointer forms of the model's records, spelled per GCS r2/t2.
typedef NUM_ABSTRACT       *NUM_ABSTRACTptr;
typedef const NUM_ABSTRACT *cNUM_ABSTRACTptr;
typedef NUM_INSTANCE       *NUM_INSTANCEptr;
typedef NUM_INSTANCE *const NUM_INSTANCEptrc;
typedef const NUM_INSTANCE *cNUM_INSTANCEptr;

/// One numbering model, built over one numbering part. A worker owns one of these and never shares it
/// (D6), so nothing here takes a lock.
struct al32 NUM_MODEL {
   NUM_ABSTRACTptr abstracts;        ///< One per w:abstractNum, in declaration order
   NUM_INSTANCEptr instances;        ///< One per w:num, in declaration order
   chptr           heap;             ///< Every string this model owns, addressed by offset
   si32ptr         buckets;          ///< Open-addressed index of w:numId onto instance number, or null
   ui64            heapUsed;         ///< Bytes of heap in use
   ui64            heapCapacity;     ///< Bytes allocated at heap
   ui64            abstractCapacity; ///< Records allocated at abstracts
   ui64            instanceCapacity; ///< Records allocated at instances
   ui32            bucketMask;       ///< One less than the bucket count, which is a power of two
   ui32            abstractCount;    ///< Definitions in abstracts
   ui32            instanceCount;    ///< Instances in instances
   ui32            counterRows;      ///< Counter rows one document needs: one per abstract, one per orphan
   XML_RESULT      lastXml;          ///< Which XML rule the part broke, for the message
   OPC_RESULT      lastOpc;          ///< How the package refused the part, for the message
   bool            hasPart;          ///< Whether a numbering part was found and read at all
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives. al32 says so, and the
// assertion below keeps it said whatever a later field does to the size.
static_assert(alignof(NUM_MODEL) >= 32u, "NumberingModel: NUM_MODEL is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of NUM_MODEL, spelled per GCS r2/t2.
typedef NUM_MODEL       *NUM_MODELptr;
typedef const NUM_MODEL *cNUM_MODELptr;
typedef NUM_MODEL *const NUM_MODELptrc;

//== Entry points

/// Prepares an empty model, in which every numId names no list.
/// @param model  Receives the model. Every field is written, so it need not be initialised, and NumClose
///               is safe to call afterwards whether or not NumLoad ever runs.
/// @note A document with no numbering part is legal and is the common case -- most documents carry no
///       lists -- so an empty model is a working model rather than an error state. Every paragraph
///       carrying a w:numPr against one is a dangling reference, which CONVERSION_REFERENCE 2.9 rules
///       is simply not numbered.
void NumOpen(NUM_MODELptrc model);

/// Reads one numbering part into a prepared model.
/// @param model      A model NumOpen has prepared.
/// @param package    The package the part belongs to.
/// @param partIndex  The numbering part, resolved through the main part's relationships; -1 loads
///                   nothing and succeeds, which is what an absent part means.
/// @param styles     The style model, already loaded. It is read for the duration of the call only, to
///                   chase w:numStyleLink; nothing here keeps a pointer into it.
/// @return NUM_OK, or why the part could not be used.
/// @note A part that is present and malformed is a refusal, not a shrug, on StyleLoad's own reasoning:
///       an absent optional part means "there are no lists", while a broken one means the document is
///       defective and guessing would hide it. CONVERSION_REFERENCE 5.4's latitude to degrade is about
///       a broken *reference* -- a dangling numId, a missing abstract definition -- and not about bytes
///       that are not well-formed XML.
cNUM_RESULT NumLoad(NUM_MODELptrc model, OPC_PACKAGEptrc package, csi32 partIndex, cSTYLE_MODELptr styles);

/// Reads one numbering part out of bytes that are already known to be well-formed UTF-8.
/// @param model      A model NumOpen has prepared.
/// @param bytes      The part's UTF-8 bytes; they are not owned and need not outlive the call.
/// @param byteCount  How many there are.
/// @param styles     The style model, which may be empty; a null pointer chases no w:numStyleLink.
/// @return NUM_OK, or why the part could not be used.
/// @note NumLoad is this plus the package read in front of it. The split exists so the parser can be
///       driven from a string literal by the unit suite, which opens no file and builds no package.
cNUM_RESULT NumLoadBytes(NUM_MODELptrc model, cui8ptr bytes, cui64 byteCount, cSTYLE_MODELptr styles);

/// Releases everything the model holds, and leaves it safe to close again.
/// @param model  A model previously passed to NumOpen.
void NumClose(NUM_MODELptrc model);

/// Finds a list instance by the identifier a paragraph's w:numPr names.
/// @param model  A prepared model.
/// @param numId  The w:val of a w:numPr's w:numId.
/// @return The instance index, or -1 when the model declares no such w:num.
/// @note Answered from an index built once at load, because a lookup happens once per numbered
///       paragraph and a part may declare thousands of instances: a scan makes that product quadratic,
///       which is the defect M5 found in StyleFind and M7 found twice more in OpcPackage. A duplicated
///       w:numId resolves to the first record, as every other duplicate in this project does.
csi32 NumFind(cNUM_MODELptr model, csi32 numId);

/// How many w:num elements the model holds.
/// @return The count, or 0 for a model with no numbering part behind it.
cui32 NumCount(cNUM_MODELptr model);

/// What one level of one list looks like, after delegation and every override.
/// @param model  A prepared model.
/// @param numId  The w:val of a w:numPr's w:numId.
/// @param level  The w:ilvl, 0 to 8; a level outside that is clamped.
/// @return The level, or a level whose format is NUM_FORMAT_ABSENT when the numId names nothing.
/// @note Every level of a resolvable instance is defined. A level the abstract definition never
///       declared borrows the nearest shallower one that did, which is what a singleLevel definition
///       needs when a paragraph is written one level in under it; a definition that declared no level
///       at all, and one whose abstract is missing or whose delegation looped, is a bullet at every
///       level -- CONVERSION_REFERENCE 5.4's "degrade to bullets or plain text, never crash", taking
///       the first branch because the document has told us the paragraph is an item and only the
///       format is unknown.
cNUM_LEVEL NumLevelOf(cNUM_MODELptr model, csi32 numId, cui8 level);

/// Turns every list reference the walk recorded into the marker the emitter writes.
/// @param document  A document DocWalk has filled and RunCoalesce, LinkResolve and MediaPlan have been
///                  over. Its blocks are read in document order and their list fields rewritten.
/// @param model     The numbering model; an empty one leaves every list item a plain paragraph.
/// @return true when the pass finished, false when it could not allocate; the document's failed flag
///         is set in that case, so a caller that already checks IrFailed need not check this as well.
/// @note This is a pass and not part of the walk, and the reason is IrRewind. DocWalker walks the first
///       mc:Choice of an mc:AlternateContent speculatively and rewinds it when an mc:Fallback follows,
///       and an IR_MARK carries no walker state -- which is why the walker saves and restores its
///       paragraph classification by hand around one. A counter table is nine counters per definition
///       and could not be rewound that way, so a discarded mc:Choice would consume a number the
///       document never showed. That is the defect M6 found in the monospace vote, one size larger.
/// @note It also puts the numbers where CONVERSION_REFERENCE 6.2 puts them, in stage [9] rather than in
///       the stage [6] walk, and for the same reason LinkResolver is a pass: a number is settled over
///       the whole document rather than over one paragraph.
/// @note Counters are keyed on the resolved **abstract** definition and never on the numId, which
///       CONVERSION_REFERENCE 2.9 states in as many words: two numIds sharing one abstract definition
///       continue one sequence, which is how Word spells "continue previous list". A w:startOverride is
///       keyed by numId instead and is applied the first time that numId is used, which is how Word
///       spells "restart at 1" -- a new numId over the same abstract definition.
/// @note Incrementing a level clears every deeper one, subject to that deeper level's own w:lvlRestart:
///       a value of 0 never restarts, a value of N restarts only under a level shallower than N, and an
///       absent one restarts under any shallower level. Clearing sets a counter back to *unstarted*
///       rather than to a value, so the start that seeds it next is chosen by whichever numId is in
///       force at that point -- which is what makes an abstract-keyed counter and a num-keyed start
///       agree with each other.
cbool NumAssignMarkers(IR_DOCUMENTptrc document, cNUM_MODELptr model);

/// The user-facing sentence for a result, ready to hand to DiagErrorText.
/// @param package  The package the part came from; a null pointer still yields a usable sentence.
/// @param model    The model the result came from; a null pointer still yields a usable sentence.
/// @param result   The result to describe.
/// @return A NUL-terminated ASCII sentence with no trailing punctuation, naming the part it is about
///         when one is known. It is valid until the next call on the same package.
/// @note A container or encoding refusal keeps the package's own sentence, which says which rule the
///       bytes broke rather than only that they could not be read.
cchptr NumResultText(OPC_PACKAGEptrc package, cNUM_MODELptr model, cNUM_RESULT result);
