/*
 * File: StyleModel.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: styles.xml as a resolved-property cache: basedOn chains, toggle parity and style roles.
 * To Do: 1) Add the monospace rFonts set and the code and quote roles when M6 detects inline code.
 *        2) Carry w:numPr from a style's pPr when M8 gives numbering a home to be read into.
 *        3) Resolve w:link pairing once a character style has to be found from its paragraph twin.
 * Dependencies: Diag.h, OpcPackage.h, XmlPull.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Diag.h"
#include "OpcPackage.h"
#include "XmlPull.h"

//== Limits

/// How many w:style elements one styles.xml may declare. Word ships around 380 built-in definitions in a
/// document that uses a dozen; four thousand is far past any producer and still refuses a part built to
/// make resolution quadratic.
constexpr cui32 STYLE_MAX_STYLES = 4096u;

/// How far a w:basedOn chain may be followed. ISO/IEC 29500 sets no limit and malformed files carry
/// cycles, so the walk is bounded and a chain that reaches the bound simply stops there.
constexpr cui32 STYLE_MAX_CHAIN = 16u;

/// The longest style identifier or style name that is kept. Anything longer is truncated rather than
/// refused: a name is only ever compared against "heading 1" and its neighbours.
constexpr cui64 STYLE_MAX_NAME_BYTES = 256u;

//== Results

/// Why loading a style part stopped. Every value but STYLE_OK means no styles were loaded.
enum STYLE_RESULT : si32 {
   STYLE_OK = 0,       ///< The part was read, or there was no part to read
   STYLE_ERROR_MEMORY, ///< An allocation failed
   STYLE_ERROR_PART,   ///< The part could not be inflated or is not text; the package records why
   STYLE_ERROR_XML,    ///< The part is not well-formed XML; the model records which rule it broke
   STYLE_ERROR_ROOT,   ///< The part's root element is not w:styles, so it is not a style part at all
   STYLE_ERROR_LIMIT,  ///< The part declares more styles than STYLE_MAX_STYLES
   STYLE_RESULT_COUNT  ///< Number of values above; not a result
};

/// Constant form of STYLE_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const STYLE_RESULT cSTYLE_RESULT;

//== Properties

/// The twelve toggle properties of ISO/IEC 29500-1 17.7.3, in one fixed order so that a set of them fits
/// in a ui16 and the XOR of two sets is one instruction.
/// @note w:dstrike is deliberately absent: the specification does not list it, so it is a plain on/off
///       property where nearest-wins rather than XOR decides. That asymmetry with w:strike is real.
enum STYLE_TOGGLE : si32 {
   STYLE_TOGGLE_BOLD = 0,   ///< w:b
   STYLE_TOGGLE_BOLD_CS,    ///< w:bCs
   STYLE_TOGGLE_ITALIC,     ///< w:i
   STYLE_TOGGLE_ITALIC_CS,  ///< w:iCs
   STYLE_TOGGLE_CAPS,       ///< w:caps
   STYLE_TOGGLE_SMALL_CAPS, ///< w:smallCaps
   STYLE_TOGGLE_STRIKE,     ///< w:strike
   STYLE_TOGGLE_OUTLINE,    ///< w:outline
   STYLE_TOGGLE_SHADOW,     ///< w:shadow
   STYLE_TOGGLE_EMBOSS,     ///< w:emboss
   STYLE_TOGGLE_IMPRINT,    ///< w:imprint
   STYLE_TOGGLE_VANISH,     ///< w:vanish
   STYLE_TOGGLE_COUNT       ///< Number of values above; not a toggle
};

/// Constant form of STYLE_TOGGLE, spelled per GCS r2.
typedef const STYLE_TOGGLE cSTYLE_TOGGLE;

/// The bit one toggle occupies in a toggle set.
/// @param toggle  A STYLE_TOGGLE.
/// @return Its bit.
constexpr cui16 StyleToggleBit(cSTYLE_TOGGLE toggle) { return ui16(1u << ui32(toggle)); }

/// What w:vertAlign says about a run. Not a toggle: the nearest specification wins, and baseline cancels.
enum STYLE_VERT_ALIGN : si8 {
   STYLE_VERT_UNSET    = -1, ///< No layer specified it
   STYLE_VERT_BASELINE = 0,  ///< w:vertAlign val="baseline"
   STYLE_VERT_SUPERSCRIPT,   ///< w:vertAlign val="superscript"
   STYLE_VERT_SUBSCRIPT      ///< w:vertAlign val="subscript"
};

/// Constant form of STYLE_VERT_ALIGN, spelled per GCS r2.
typedef const STYLE_VERT_ALIGN cSTYLE_VERT_ALIGN;

/// What a style contributes to a paragraph's shape. Roles are read from the normalized style name and the
/// normalized style id, and the nearest specification along the basedOn chain wins.
/// @note M6 adds the quote and code roles; declaring them here before anything acts on them would be an
///       enumerator no code reads, which is worse than adding one later.
enum STYLE_ROLE : si8 {
   STYLE_ROLE_NORMAL = 0, ///< Nothing special; a body paragraph
   STYLE_ROLE_HEADING,    ///< A heading, whose level is carried beside the role
   STYLE_ROLE_TITLE,      ///< The Title style
   STYLE_ROLE_SUBTITLE,   ///< The Subtitle style
   STYLE_ROLE_COUNT       ///< Number of values above; not a role
};

/// Constant form of STYLE_ROLE, spelled per GCS r2.
typedef const STYLE_ROLE cSTYLE_ROLE;

/// What w:type says a style is. An absent w:type means paragraph, which is the schema's own default.
enum STYLE_TYPE : si8 {
   STYLE_TYPE_PARAGRAPH = 0, ///< A paragraph style, referenced by w:pStyle
   STYLE_TYPE_CHARACTER,     ///< A character style, referenced by w:rStyle
   STYLE_TYPE_TABLE,         ///< A table style, which nothing reads yet
   STYLE_TYPE_NUMBERING,     ///< A numbering style, which M8 will chase through w:numStyleLink
   STYLE_TYPE_COUNT          ///< Number of values above; not a type
};

/// Constant form of STYLE_TYPE, spelled per GCS r2.
typedef const STYLE_TYPE cSTYLE_TYPE;

//== Resolved views

/// The run properties in force on one run, after every layer has been applied.
struct STYLE_RUN_PROPS {
   ui16             toggles;      ///< One bit per STYLE_TOGGLE: the effective on or off value
   bool             doubleStrike; ///< w:dstrike, nearest specification wins
   STYLE_VERT_ALIGN vertAlign;    ///< w:vertAlign, nearest specification wins; never STYLE_VERT_UNSET here
};

/// Constant and pointer forms of STYLE_RUN_PROPS, spelled per GCS r2/t2.
typedef const STYLE_RUN_PROPS        cSTYLE_RUN_PROPS;
typedef const STYLE_RUN_PROPS       *cSTYLE_RUN_PROPSptr;
typedef const STYLE_RUN_PROPS *const cSTYLE_RUN_PROPSptrc;

/// The paragraph properties in force on one paragraph, after every layer has been applied.
struct STYLE_PARAGRAPH_PROPS {
   STYLE_ROLE role;         ///< What the style chain says the paragraph is
   ui8        headingLevel; ///< 1 to 6 when the paragraph is a heading, otherwise 0
};

/// Constant form of STYLE_PARAGRAPH_PROPS, spelled per GCS r2.
typedef const STYLE_PARAGRAPH_PROPS cSTYLE_PARAGRAPH_PROPS;

/// What one run's own w:rPr specifies, which is the layer that wins outright.
/// @note toggleSpecified is what makes direct formatting final: a toggle a run names is taken from
///       toggleTrue whatever the styles say, and one it does not name falls through to the XOR.
struct STYLE_DIRECT_RUN {
   ui16             toggleTrue;      ///< Bit set for each toggle the run specifies as true
   ui16             toggleSpecified; ///< Bit set for each toggle the run specifies at all
   si32             characterStyle;  ///< Index of the w:rStyle style, or -1 when there is none
   si8              doubleStrike;    ///< -1 unspecified, 0 specified false, 1 specified true
   STYLE_VERT_ALIGN vertAlign;       ///< STYLE_VERT_UNSET when the run does not specify it
};

/// Constant and pointer forms of STYLE_DIRECT_RUN, spelled per GCS r2/t2.
typedef const STYLE_DIRECT_RUN        cSTYLE_DIRECT_RUN;
typedef STYLE_DIRECT_RUN             *STYLE_DIRECT_RUNptr;
typedef const STYLE_DIRECT_RUN       *cSTYLE_DIRECT_RUNptr;
typedef STYLE_DIRECT_RUN *const       STYLE_DIRECT_RUNptrc;
typedef const STYLE_DIRECT_RUN *const cSTYLE_DIRECT_RUNptrc;

//== Model

/// One w:style as the part declared it. Strings are heap offsets, which a growing heap does not
/// invalidate the way it would invalidate pointers.
struct STYLE_RECORD {
   ui32             idAt;         ///< Heap offset of w:styleId exactly as written
   ui32             nameAt;       ///< Heap offset of the normalized w:name, or of the normalized id when absent
   ui32             basedOnAt;    ///< Heap offset of the w:basedOn id as written; the empty string when none
   si32             basedOn;      ///< Index of the based-on style, or -1; filled once every style is read
   ui16             toggleTrue;   ///< Bit set for each toggle this one style specifies as true
   si32             outlineLvl;   ///< w:pPr/w:outlineLvl, or -1 when this style does not specify it
   STYLE_ROLE       role;         ///< What this one style's own name and id say it is
   ui8              headingLevel; ///< 1 to 9 as the name said, before clamping; 0 when the role is not heading
   si8              doubleStrike; ///< -1 unspecified, 0 specified false, 1 specified true
   STYLE_TYPE       type;         ///< What w:type said
   STYLE_VERT_ALIGN vertAlign;    ///< w:vertAlign, or STYLE_VERT_UNSET
   bool             isDefault;    ///< Whether w:default was true
};

/// One style's properties folded down its whole basedOn chain, computed once when the part is loaded.
struct STYLE_RESOLVED {
   ui16             toggleParity; ///< XOR over the chain of every explicit true toggle specification
   si32             outlineLvl;   ///< The nearest w:outlineLvl along the chain, or -1
   STYLE_ROLE       role;         ///< The nearest role along the chain
   ui8              headingLevel; ///< Its heading level, before clamping
   si8              doubleStrike; ///< The nearest w:dstrike along the chain, or -1
   STYLE_VERT_ALIGN vertAlign;    ///< The nearest w:vertAlign along the chain, or STYLE_VERT_UNSET
};

/// What w:docDefaults contributes. It is not a style and never joins the XOR: a toggle it sets true is
/// true unless a run's own w:rPr says otherwise, which is what ISO/IEC 29500-1 17.7.2 asks for.
struct STYLE_DEFAULTS {
   ui16             toggleTrue;   ///< Bit set for each toggle w:rPrDefault specifies as true
   si32             outlineLvl;   ///< w:pPrDefault/w:outlineLvl, or -1
   si8              doubleStrike; ///< -1 unspecified, 0 specified false, 1 specified true
   STYLE_VERT_ALIGN vertAlign;    ///< w:vertAlign, or STYLE_VERT_UNSET
};

/// Constant and pointer forms of the model's records, spelled per GCS r2/t2.
typedef STYLE_RECORD         *STYLE_RECORDptr;
typedef const STYLE_RECORD   *cSTYLE_RECORDptr;
typedef STYLE_RESOLVED       *STYLE_RESOLVEDptr;
typedef const STYLE_RESOLVED *cSTYLE_RESOLVEDptr;

/// One style model, built over one styles part. A worker owns one of these and never shares it (D6), so
/// nothing here takes a lock.
struct al32 STYLE_MODEL {
   STYLE_RECORDptr   styles;           ///< One per w:style, in declaration order
   STYLE_RESOLVEDptr resolved;         ///< One per style, folded down its chain
   chptr             heap;             ///< Every string this model owns, addressed by offset
   ui64              heapUsed;         ///< Bytes of heap in use
   ui64              heapCapacity;     ///< Bytes allocated at heap
   ui64              styleCapacity;    ///< Records allocated at styles and at resolved
   STYLE_DEFAULTS    defaults;         ///< What w:docDefaults contributes
   ui32              styleCount;       ///< Styles in styles
   si32              defaultParagraph; ///< Index of the default paragraph style, or -1
   XML_RESULT        lastXml;          ///< Which XML rule the part broke, for the message
   OPC_RESULT        lastOpc;          ///< How the package refused the part, for the message
   bool              hasPart;          ///< Whether a styles part was found and read at all
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives. al32 says so, and the
// assertion below keeps it said whatever a later field does to the size.
static_assert(alignof(STYLE_MODEL) >= 32u, "StyleModel: STYLE_MODEL is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of STYLE_MODEL, spelled per GCS r2/t2.
typedef STYLE_MODEL       *STYLE_MODELptr;
typedef const STYLE_MODEL *cSTYLE_MODELptr;
typedef STYLE_MODEL *const STYLE_MODELptrc;

//== Entry points

/// Prepares an empty model, which resolves every property to its specification default.
/// @param model  Receives the model. Every field is written, so it need not be initialised, and
///               StyleClose is safe to call afterwards whether or not StyleLoad ever runs.
/// @note A document with no styles part is legal and common -- every optional part is optional -- so an
///       empty model is a working model rather than an error state.
void StyleOpen(STYLE_MODELptrc model);

/// Reads one styles part into a prepared model.
/// @param model      A model StyleOpen has prepared.
/// @param package    The package the part belongs to.
/// @param partIndex  The styles part, resolved through the main part's relationships; -1 loads nothing
///                   and succeeds, which is what an absent part means.
/// @return STYLE_OK, or why the part could not be used.
/// @note A part that is present and malformed is a refusal, not a shrug: an absent optional part means
///       "use the defaults", while a broken one means the document is defective and guessing would hide
///       it. That reading matches D8's treatment of a part that is not well-formed UTF-8.
cSTYLE_RESULT StyleLoad(STYLE_MODELptrc model, OPC_PACKAGEptrc package, csi32 partIndex);

/// Reads one styles part out of bytes that are already known to be well-formed UTF-8.
/// @param model      A model StyleOpen has prepared.
/// @param bytes      The part's UTF-8 bytes; they are not owned and need not outlive the call.
/// @param byteCount  How many there are.
/// @return STYLE_OK, or why the part could not be used.
/// @note StyleLoad is this plus the package read in front of it. The split exists so the parser can be
///       driven from a string literal by the unit suite, which opens no file and builds no package.
cSTYLE_RESULT StyleLoadBytes(STYLE_MODELptrc model, cui8ptr bytes, cui64 byteCount);

/// Releases everything the model holds, and leaves it safe to close again.
/// @param model  A model previously passed to StyleOpen.
void StyleClose(STYLE_MODELptrc model);

/// Finds a style by its identifier.
/// @param model    A prepared model.
/// @param styleId  The w:val of a w:pStyle or w:rStyle, NUL-terminated.
/// @return The style index, or -1 when the model declares no such style.
/// @note Identifiers compare exactly. ISO/IEC 29500 makes w:styleId an xsd:string and Word matches it
///       byte for byte; the case-insensitive matching in CONVERSION_REFERENCE 5.10 is about the style
///       *name*, which is a different key and is handled by the role table.
csi32 StyleFind(cSTYLE_MODELptr model, cchptr styleId);

/// The style a paragraph with no w:pStyle uses.
/// @param model  A prepared model.
/// @return The index of the w:default="1" paragraph style, or -1 when the model declares none.
csi32 StyleDefaultParagraph(cSTYLE_MODELptr model);

/// The number of styles the model holds.
cui32 StyleCount(cSTYLE_MODELptr model);

/// The normalized name of one style, for a caller that wants to report or match it.
/// @return The name, or an empty string for an index outside the model. Never null.
cchptr StyleName(cSTYLE_MODELptr model, csi32 styleIndex);

/// Resolves the properties in force on one paragraph.
/// @param model         A prepared model.
/// @param styleIndex    The paragraph's style, or -1 when it has none and the model has no default.
/// @param directOutline The paragraph's own w:pPr/w:outlineLvl, or -1 when it does not carry one.
/// @return The role and, for a heading, its clamped level.
/// @note The name decides before w:outlineLvl does, per CONVERSION_REFERENCE 2.8: a style chain naming a
///       heading is a heading whatever the outline level says, and the outline level is what catches a
///       paragraph whose style has no telling name.
/// @note Levels 7 to 9 clamp to 6, because GitHub-Flavored Markdown has no deeper heading.
cSTYLE_PARAGRAPH_PROPS StyleResolveParagraph(cSTYLE_MODELptr model, csi32 styleIndex, csi32 directOutline);

/// Resolves the properties in force on one run.
/// @param model           A prepared model.
/// @param paragraphStyle  The enclosing paragraph's style index, or -1.
/// @param direct          What the run's own w:rPr specified. May not be null.
/// @return The effective run properties.
/// @note The toggle rule is ISO/IEC 29500-1 17.7.3 as CONVERSION_REFERENCE 5.2 states it: a toggle the
///       run itself names is final; otherwise a docDefaults true wins outright; otherwise the value is
///       the XOR of every explicit true specification across the paragraph-style and character-style
///       chains, so an even number of them cancels out.
cSTYLE_RUN_PROPS StyleResolveRun(cSTYLE_MODELptr model, csi32 paragraphStyle, cSTYLE_DIRECT_RUNptr direct);

/// Reads one child element of a w:rPr into a direct run-property record.
/// @param reader  A reader whose last token is a start element inside a w:rPr.
/// @param direct  The record to fold the element into. May not be null.
/// @note Only the reader's attributes are touched; the caller still has to skip the element. That keeps
///       every child of a property bag on one code path whether or not this module recognised it.
/// @note w:rStyle is deliberately not handled here: resolving it needs the model, and the caller has one
///       while this function is meant to work over a bare reader.
void StyleReadDirectProperty(XML_READERptrc reader, STYLE_DIRECT_RUNptrc direct);

/// Clears a direct run-property record to "nothing specified".
/// @param direct  Receives the cleared record. May not be null.
void StyleClearDirect(STYLE_DIRECT_RUNptrc direct);

/// Reads a w:val OnOff attribute the way ISO/IEC 29500-1 17.17.4 defines it.
/// @param value  The attribute value; a view whose bytes are null means the attribute is absent.
/// @return true for an absent value and for 1, true or on; false for 0, false and off, and for anything
///         else, which is a defensive fallback rather than a rule the specification states.
cbool StyleOnOff(cXML_TEXT value);

/// Normalizes a style name or identifier for matching.
/// @param text        The bytes to normalize.
/// @param byteCount   How many there are.
/// @param dest        Receives the normalized, NUL-terminated form.
/// @param destBytes   Bytes available at dest, terminator included.
/// @return The number of bytes written, terminator excluded.
/// @note Lowercases ASCII, folds every run of ASCII whitespace to one space, trims both ends, and decodes
///       the _20_ escape LibreOffice writes into an identifier for a space (CONVERSION_REFERENCE 5.10).
///       Bytes above 0x7F pass through untouched: no producer spells a built-in style name in them.
cui64 StyleNormalizeName(cchptr text, cui64 byteCount, chptrc dest, cui64 destBytes);

/// The role a normalized style name or identifier names.
/// @param normalized  A normalized name, NUL-terminated.
/// @param level       Receives the heading level 1 to 9 when the role is a heading, otherwise 0.
/// @return The role, or STYLE_ROLE_NORMAL when the name names none.
/// @note Both spellings are matched: "heading 1" as w:name writes it and "heading1" as a styleId does.
cSTYLE_ROLE StyleRoleOfName(cchptr normalized, ui8ptrc level);

/// The user-facing sentence for a result, ready to hand to DiagErrorText.
/// @param package  The package the part came from; a null pointer still yields a usable sentence.
/// @param model    The model the result came from; a null pointer still yields a usable sentence.
/// @param result   The result to describe.
/// @return A NUL-terminated ASCII sentence with no trailing punctuation, naming the part it is about
///         when one is known. It is valid until the next call on the same package.
/// @note A container or encoding refusal keeps the package's own sentence, which says which rule the
///       bytes broke rather than only that they could not be read.
cchptr StyleResultText(OPC_PACKAGEptrc package, cSTYLE_MODELptr model, cSTYLE_RESULT result);
