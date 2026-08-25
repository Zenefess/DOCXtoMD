/*
 * File: XmlPull.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-25
 * Description: First-party streaming XML pull tokenizer over one in-memory part (decision D2).
 * To Do: 1) Enforce XML 1.0's ban on a literal ]]> in character data if a producer is ever found
 *           emitting one; today it costs a scan and rejects nothing.
 *        2) Benchmark an AVX2 scan for the next '<' against the byte loop before adopting one (bd1/bd2).
 *        3) Raise XML_MAX_ATTRIBUTES or XML_MAX_NAMESPACES if a real producer is ever found near them.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Limits

/// How deeply elements may nest before the part is refused. Real documents reach perhaps twenty levels;
/// this exists so a part made of a hundred thousand open tags cannot walk the reader's stack off its end.
constexpr cui32 XML_MAX_DEPTH = 256u;

/// How many attributes one element may carry. Namespace declarations are scoping rather than
/// attributes and are counted against XML_MAX_NAMESPACES instead, so the Word w:document root that
/// declares thirty of them arrives here carrying one -- mc:Ignorable. The ceiling is generous
/// anyway, because nothing in OOXML comes near it and a cap that bites is worse than one that does not.
constexpr cui32 XML_MAX_ATTRIBUTES = 128u;

/// How many namespace bindings may be live at once, counting every enclosing element's.
constexpr cui32 XML_MAX_NAMESPACES = 128u;

//== Results

/// Why tokenizing stopped. Every value but XML_OK means the part is not usable.
enum XML_RESULT : si32 {
   XML_OK = 0,           ///< Nothing is wrong
   XML_ERROR_DOCTYPE,    ///< A document type declaration; refused outright, because entities are an attack
   XML_ERROR_SYNTAX,     ///< A construct is malformed in a way no producer would emit
   XML_ERROR_UNCLOSED,   ///< A tag, comment, section or element runs to the end of the part
   XML_ERROR_MISMATCH,   ///< An end tag names an element other than the one that is open
   XML_ERROR_DEPTH,      ///< Elements nest deeper than XML_MAX_DEPTH
   XML_ERROR_NAMESPACE,  ///< A prefix was used that no enclosing element declares
   XML_ERROR_ATTRIBUTES, ///< One element carries more attributes than XML_MAX_ATTRIBUTES, or repeats one
   XML_ERROR_BINDINGS,   ///< More namespace bindings are live at once than XML_MAX_NAMESPACES
   XML_ERROR_ENTITY,     ///< An entity reference that is not one of XML's five, so nothing defines it
   XML_ERROR_CHARACTER,  ///< A character reference naming a code point XML does not allow in content
   XML_ERROR_TRAILING,   ///< Markup outside the root element, or no root element at all
   XML_ERROR_ENCODING,   ///< The part begins with a UTF-16 byte-order mark, so it is not UTF-8 at all
   XML_ERROR_MEMORY,     ///< The decoding scratch buffer could not be allocated
   XML_RESULT_COUNT      ///< Number of values above; not a result
};

/// Constant form of XML_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const XML_RESULT cXML_RESULT;

/// What XmlNext produced.
enum XML_TOKEN : si32 {
   XML_TOKEN_NONE = 0,      ///< Nothing has been read yet
   XML_TOKEN_START_ELEMENT, ///< An element opened; its name, namespace and attributes are readable
   XML_TOKEN_END_ELEMENT,   ///< An element closed; its name and namespace are readable
   XML_TOKEN_TEXT,          ///< Character data, with references resolved and line ends normalised
   XML_TOKEN_END_OF_INPUT,  ///< The root element closed and the part ended
   XML_TOKEN_ERROR          ///< The part is malformed; the reader's result says why
};

/// Constant form of XML_TOKEN, spelled per GCS r2.
typedef const XML_TOKEN cXML_TOKEN;

//== Namespaces

/// The namespaces this converter knows by name. Matching is by URI, never by the prefix a file happens
/// to use (correctness rule 2), and both the ECMA-376 Transitional and the ISO 29500 Strict URI families
/// map onto the same value, so a Strict document walks the same code as a Transitional one.
enum XML_NS : si32 {
   XML_NS_NONE = 0, ///< The name carries no namespace at all
   XML_NS_OTHER,    ///< A namespace this build does not recognise
   XML_NS_XML,      ///< http://www.w3.org/XML/1998/namespace, the built-in xml: prefix
   XML_NS_W,        ///< WordprocessingML main
   XML_NS_R,        ///< officeDocument relationships, the r:id attribute family
   XML_NS_A,        ///< DrawingML main
   XML_NS_WP,       ///< DrawingML WordprocessingDrawing
   XML_NS_PIC,      ///< DrawingML picture
   XML_NS_M,        ///< Office math
   XML_NS_V,        ///< VML
   XML_NS_MC,       ///< Markup compatibility and extensibility
   XML_NS_CT,       ///< The package's [Content_Types].xml
   XML_NS_PR,       ///< The package's relationship parts
   XML_NS_COUNT     ///< Number of values above; not a namespace
};

/// Constant form of XML_NS, spelled per GCS r2.
typedef const XML_NS  cXML_NS;
typedef XML_NS       *XML_NSptr;
typedef XML_NS *const XML_NSptrc;

//== Views

/// A range of bytes inside the part, or inside the reader's decoding scratch. Never NUL-terminated, and
/// only valid until the next XmlNext call on the same reader.
struct XML_TEXT {
   cchptr bytes;  ///< First byte, or null for an absent value
   ui64   length; ///< Number of bytes
};

/// Constant and pointer forms of XML_TEXT, spelled per GCS r2/t2.
typedef const XML_TEXT        cXML_TEXT;
typedef XML_TEXT             *XML_TEXTptr;
typedef const XML_TEXT       *cXML_TEXTptr;
typedef const XML_TEXT *const cXML_TEXTptrc;

/// One attribute of the element the reader is on.
/// @note An unprefixed attribute is in no namespace at all -- a default xmlns binds element names only,
///       which is why Id, Type and Target in a relationship part read as XML_NS_NONE.
/// @note The uri is what distinguishes two attributes, not the space: every namespace this build does
///       not know is XML_NS_OTHER, so comparing the space alone would make two attributes from two
///       different extension namespaces -- w14 and w15, say -- look like the same attribute twice.
struct XML_ATTRIBUTE {
   XML_TEXT name;  ///< Local name, with any prefix stripped
   XML_TEXT value; ///< Value, with references resolved and whitespace normalised
   XML_TEXT uri;   ///< URI the prefix resolved to; empty for an attribute in no namespace
   XML_NS   space; ///< Namespace the prefix resolved to
};

/// Constant and pointer forms of XML_ATTRIBUTE, spelled per GCS r2/t2.
typedef const XML_ATTRIBUTE        cXML_ATTRIBUTE;
typedef const XML_ATTRIBUTE       *cXML_ATTRIBUTEptr;
typedef const XML_ATTRIBUTE *const cXML_ATTRIBUTEptrc;

//== Reader

/// One element on the open-element stack.
struct XML_ELEMENT {
   XML_TEXT name;         ///< The qualified name exactly as the start tag spelled it
   ui64     scratchFloor; ///< Bytes of scratch this element's namespace bindings occupy, and keep
   ui32     bindings;     ///< Namespace bindings this element pushed, popped again when it closes
   bool     preserve;     ///< The xml:space state in force inside this element
};

/// Constant form of XML_ELEMENT, spelled per GCS r2.
typedef const XML_ELEMENT cXML_ELEMENT;

/// One namespace binding: a prefix, and what it currently means.
struct XML_BINDING {
   XML_TEXT prefix; ///< The prefix, empty for the default namespace
   XML_TEXT uri;    ///< The URI it is bound to, empty when a prefix is undeclared by xmlns=""
   XML_NS   space;  ///< Which known namespace that URI is, or XML_NS_OTHER
};

/// Constant form of XML_BINDING, spelled per GCS r2.
typedef const XML_BINDING cXML_BINDING;

/// One part being tokenized. About twenty kilobytes, because the element, attribute and namespace tables
/// are held inline rather than allocated: one worker owns one of these per part and never shares it (D6),
/// so hold it in the worker's context rather than deep on a recursive stack.
struct al32 XML_READER {
   cui8ptr       bytes;                          ///< The part's UTF-8 bytes; owned by the caller
   ui64          byteCount;                      ///< Bytes in bytes
   ui64          at;                             ///< Read cursor
   ui64          errorOffset;                    ///< Where a failure was found, for the message
   chptr         scratch;                        ///< Where decoded text and attribute values are built
   ui64          scratchBytes;                   ///< Bytes allocated at scratch
   ui64          scratchUsed;                    ///< Bytes of scratch the current token occupies
   XML_TEXT      name;                           ///< Local name of the current element
   XML_TEXT      prefix;                         ///< Its prefix, empty when it has none
   XML_TEXT      uri;                            ///< The URI its prefix resolved to
   XML_TEXT      text;                           ///< Character data, when the token is XML_TOKEN_TEXT
   XML_ATTRIBUTE attributes[XML_MAX_ATTRIBUTES]; ///< The current element's attributes, xmlns excluded
   XML_ELEMENT   elements[XML_MAX_DEPTH];        ///< The open-element stack
   XML_BINDING   bindings[XML_MAX_NAMESPACES];   ///< Every namespace binding currently in force
   ui32          attributeCount;                 ///< Attributes on the current element
   ui32          bindingCount;                   ///< Live namespace bindings
   ui32          depth;                          ///< Depth of the element the current token names
   ui32          openCount;                      ///< Elements currently open
   XML_TOKEN     token;                          ///< What the last XmlNext produced
   XML_RESULT    result;                         ///< Why it stopped, when the token is XML_TOKEN_ERROR
   XML_NS        space;                          ///< Namespace of the current element
   bool          preserveSpace;                  ///< Whether xml:space="preserve" is in force here
   bool          allWhitespace;                  ///< Whether a text token holds nothing but XML whitespace
   bool          pendingEnd;                     ///< A self-closing tag still owes its end token
   bool          sawRoot;                        ///< Whether a root element has been seen
   bool          closedRoot;                     ///< Whether it has closed, so any further markup is junk
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives -- including on a stack frame,
// which is 8- or 16-byte aligned by default. al32 says so, and the assertion below keeps it said.
static_assert(alignof(XML_READER) >= 32u, "XmlPull: XML_READER is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of XML_READER, spelled per GCS r2/t2.
typedef XML_READER       *XML_READERptr;
typedef const XML_READER *cXML_READERptr;
typedef XML_READER *const XML_READERptrc;

//== Entry points

/// Prepares a reader over one part's bytes.
/// @param reader     Receives the prepared reader. Every field is written, so it need not be initialised,
///                   and XmlClose is safe to call whatever this returns.
/// @param bytes      The part's UTF-8 bytes. The reader does not copy or own them, and every view it
///                   hands out points into them, so they must outlive the reader.
/// @param byteCount  Bytes at bytes.
/// @return XML_OK, or XML_ERROR_ENCODING when the bytes open with a UTF-16 byte-order mark. Nothing
///         else can fail here: the decoding scratch is allocated on first need, not now.
/// @note The bytes must already be well-formed UTF-8 -- OpcPackage puts every part through UtfValidate
///       before opening it. Nothing here reads past byteCount whatever the bytes are, so ill-formed input
///       is a correctness problem rather than a safety one, but the views would carry broken sequences.
/// @note A leading UTF-8 byte-order mark is consumed here, so callers never see one in a token.
/// @note The decoding scratch, when it is needed at all, is allocated once at byteCount plus a few
///       bytes of headroom, and
///       never grows: decoding a reference, a CDATA section or a line end never produces more bytes
///       than it consumes, so the part's own size is a ceiling no token can reach past.
cXML_RESULT XmlOpen(XML_READERptrc reader, cui8ptr bytes, cui64 byteCount);

/// Releases what a reader allocated, and leaves it safe to close again.
/// @param reader  A reader previously passed to XmlOpen.
void XmlClose(XML_READERptrc reader);

/// Reads the next token.
/// @param reader  An opened reader.
/// @return The token kind. XML_TOKEN_ERROR leaves the reason in the reader's result field, and every
///         later call returns XML_TOKEN_ERROR too rather than trying to resynchronise.
/// @note Comments, processing instructions and the XML declaration are consumed silently. A self-closing
///       element reports XML_TOKEN_START_ELEMENT and then XML_TOKEN_END_ELEMENT, so a caller written for
///       a pair of tags needs no separate case for it.
/// @note Every view the reader exposes is invalidated by the next call, because the scratch buffer the
///       decoded ones live in is reused from the start of each token. A namespace URI that had to be
///       decoded is the exception, and has to be: a binding outlives the tag that declared it, so the
///       arena is rewound only to the floor the innermost open element set, never to zero.
/// @note A comment or a processing instruction ends a run of character data; a reference and a CDATA
///       section are folded into it. So one stretch of text can arrive as two adjacent text tokens,
///       and a caller that accumulates text must append rather than replace.
/// @note A text token never arrives empty and never arrives trimmed: whitespace is content until the
///       walker decides otherwise, and allWhitespace is a convenience for that decision rather than a
///       licence to drop it -- what may be dropped is xml:space's business, not the tokenizer's.
/// @note Three relaxations of XML 1.0, all deliberate, all in the accepting direction, and all safe for
///       OOXML, which spells every name in
///       ASCII: a name may hold any byte above 0x7F without consulting the Unicode NameChar tables, and
///       the bans on a literal "]]>" inside character data and on "--" inside a comment are not
///       enforced, because enforcing either costs a
///       scan and rejects nothing a producer emits. A stricter parser -- expat, for one -- refuses both,
///       so a document this accepts is not always one every reader accepts; accepting more is the right
///       direction for a converter, and it is recorded here so nobody mistakes it for an oversight.
/// @note No attribute is ever defaulted here. There is no document type declaration to define one --
///       the tokenizer refuses those -- so an absent attribute is absent, and a caller applies
///       whatever default ISO/IEC 29500 gives it.
/// @note A NUL, and every other C0 control XML 1.0's Char production excludes, is refused wherever it
///       can appear in content. That is load-bearing above this module: a NUL is well-formed UTF-8, and
///       OpcPackage copies attribute values into NUL-terminated storage.
///       The check is on bytes, not on decoded code points, so a raw U+FFFE or U+FFFF -- which Char
///       also excludes -- passes through where the same code points written as references do not.
///       Nothing emits them and nothing downstream is harmed by them, so the byte scan stays cheap.
cXML_TOKEN XmlNext(XML_READERptrc reader);

/// Skips the element the reader has just started, and everything inside it.
/// @param reader  A reader whose last token was XML_TOKEN_START_ELEMENT.
/// @return true when the matching end element was consumed, false when the part failed on the way.
/// @note This is how the OOXML compatibility model is honoured: an element this build does not know is
///       skipped whole rather than descended into.
cbool XmlSkipElement(XML_READERptrc reader);

/// Finds an attribute of the current element.
/// @param reader     A reader whose last token was XML_TOKEN_START_ELEMENT.
/// @param space      The namespace the attribute must be in; XML_NS_NONE matches an unprefixed one.
/// @param localName  NUL-terminated ASCII local name.
/// @return The value, or a view whose bytes are null when the element has no such attribute.
cXML_TEXT XmlAttribute(cXML_READERptr reader, cXML_NS space, cchptr localName);

/// Compares a view against a NUL-terminated ASCII string.
/// @param text  The view.
/// @param name  NUL-terminated ASCII.
/// @return true when the two hold exactly the same bytes.
cbool XmlTextEqual(cXML_TEXT text, cchptr name);

/// Reports whether the reader is on a start element with a given namespace and local name.
/// @param reader     The reader.
/// @param space      The namespace to require.
/// @param localName  NUL-terminated ASCII local name.
/// @return true when the last token was that start element.
cbool XmlIsElement(cXML_READERptr reader, cXML_NS space, cchptr localName);

/// The user-facing sentence for a result, ready to hand to DiagErrorText.
/// @param result  The result to describe.
/// @return A static, NUL-terminated ASCII sentence with no trailing punctuation.
cchptr XmlResultText(cXML_RESULT result);
