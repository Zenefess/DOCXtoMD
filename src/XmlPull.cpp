/*
 * File: XmlPull.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: XML pull tokenizer: scanning, reference decoding, namespace scoping and the depth caps.
 * To Do: 1) Expose a prefix lookup once mc:Choice's Requires attribute has to be resolved at M7.
 *        2) Benchmark an AVX2 scan for the next '<' against the byte loop before adopting one (bd1/bd2).
 *        3) Decode a reference straight into the caller's buffer if the scratch ever shows up in a profile.
 * Dependencies: BuildGuards.h, Utf.h, XmlPull.h, typedefs.h, memory management.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros, and memory management.h pulls those two in that order itself.
#include <windows.h>
#include "typedefs.h"
#include "memory management.h"
#include "Utf.h"
#include "XmlPull.h"

//== Byte classes

// One bit per property a scanner asks about. The table below answers all four in one indexed load, which
// is what keeps the scanning loops free of chains of comparisons.
constexpr cui8 XML_CLASS_SPACE = 0x01u; // #x20, #x9, #xD or #xA
constexpr cui8 XML_CLASS_NAME  = 0x02u; // May appear anywhere in a name
constexpr cui8 XML_CLASS_START = 0x04u; // May begin a name
constexpr cui8 XML_CLASS_BAD   = 0x08u; // Outside XML 1.0's Char production, so illegal anywhere

// Wrapped in a struct so a constexpr function can return the whole table by value.
struct XML_CLASS_LOOKUP {
   ui8 slot[256];
};

// The classes one byte belongs to. OOXML spells every element, attribute and prefix name in ASCII, so
// XML's full Unicode NameStartChar and NameChar productions are deliberately relaxed to "the ASCII name
// characters, plus any byte a multi-byte UTF-8 sequence uses" -- the part was validated as UTF-8 before
// it was opened, so accepting those bytes wholesale cannot admit anything but a real character.
static constexpr ui8 XmlClassOf(cui32 byte) {
   cbool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
   cbool digit  = (byte >= '0' && byte <= '9');
   ui8   flags  = 0;

   if(byte == 0x20u || byte == 0x09u || byte == 0x0Du || byte == 0x0Au) flags |= XML_CLASS_SPACE;
   if(letter || byte == '_' || byte == ':' || byte >= 0x80u) flags |= ui8(XML_CLASS_START | XML_CLASS_NAME);
   if(digit || byte == '-' || byte == '.') flags |= XML_CLASS_NAME;
   if(byte < 0x20u && !(flags & XML_CLASS_SPACE)) flags |= XML_CLASS_BAD;
   return flags;
}

static constexpr XML_CLASS_LOOKUP XmlBuildClasses(void) {
   XML_CLASS_LOOKUP table = {};

   for(ui32 index = 0; index < 256u; ++index) table.slot[index] = XmlClassOf(index);
   return table;
}

static constexpr XML_CLASS_LOOKUP XML_CLASSES = XmlBuildClasses();

// The three rows a mistyped table loses first: a tab is whitespace and not a control byte, a NUL is a
// control byte and nothing else, and a digit may continue a name but may not begin one.
static_assert(XML_CLASSES.slot[0x09u] == XML_CLASS_SPACE, "XML classes: a tab is whitespace, not a forbidden control byte.");
static_assert(XML_CLASSES.slot[0x00u] == XML_CLASS_BAD, "XML classes: a NUL is outside XML's Char production.");
static_assert(XML_CLASSES.slot['0'] == XML_CLASS_NAME, "XML classes: a digit continues a name but cannot begin one.");

//== Namespace URIs

// Every namespace this build knows, by URI. Both URI families map onto one value, which is the whole of
// the ISO Strict story (correctness rule 2): a Strict document walks exactly the same code as a
// Transitional one, and nothing anywhere matches on the prefix a file happens to have chosen.
struct XML_NS_ENTRY {
   cchptr uri;   ///< The namespace URI, NUL-terminated
   XML_NS space; ///< What this build calls it
};

static constexpr XML_NS_ENTRY XML_NS_URIS[] = {
    // Transitional first, then the Strict alias where ISO 29500 defines one     // family
    {"http://www.w3.org/XML/1998/namespace", XML_NS_XML},                       // built-in
    {"http://schemas.openxmlformats.org/wordprocessingml/2006/main", XML_NS_W}, // transitional
    {"http://purl.oclc.org/ooxml/wordprocessingml/main", XML_NS_W},             // strict
    {"http://schemas.openxmlformats.org/officeDocument/2006/relationships", XML_NS_R},
    {"http://purl.oclc.org/ooxml/officeDocument/relationships", XML_NS_R}, // strict
    {"http://schemas.openxmlformats.org/drawingml/2006/main", XML_NS_A},   // transitional
    {"http://purl.oclc.org/ooxml/drawingml/main", XML_NS_A},               // strict
    {"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing", XML_NS_WP},
    {"http://purl.oclc.org/ooxml/drawingml/wordprocessingDrawing", XML_NS_WP},   // strict
    {"http://schemas.openxmlformats.org/drawingml/2006/picture", XML_NS_PIC},    // transitional
    {"http://purl.oclc.org/ooxml/drawingml/picture", XML_NS_PIC},                // strict
    {"http://schemas.openxmlformats.org/officeDocument/2006/math", XML_NS_M},    // transitional
    {"http://purl.oclc.org/ooxml/officeDocument/math", XML_NS_M},                // strict
    {"urn:schemas-microsoft-com:vml", XML_NS_V},                                 // no strict form
    {"http://schemas.openxmlformats.org/markup-compatibility/2006", XML_NS_MC},  // not versioned
    {"http://schemas.openxmlformats.org/package/2006/content-types", XML_NS_CT}, // not versioned
    {"http://schemas.openxmlformats.org/package/2006/relationships", XML_NS_PR}  // not versioned
};

constexpr cui32 XML_NS_URI_COUNT = ui32(sizeof(XML_NS_URIS) / sizeof(XML_NS_URIS[0]));

// The length of a NUL-terminated constant, computed in the compiler so no length is written out by hand.
static constexpr cui64 XmlStaticLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// The URI the xml prefix is bound to without any declaration, and the one no document may declare.
constexpr cchptr XML_URI_XML   = "http://www.w3.org/XML/1998/namespace";
constexpr cchptr XML_URI_XMLNS = "http://www.w3.org/2000/xmlns/";

//== Sentences

// One sentence per XML_RESULT, in the order the enum declares them.
static constexpr cchptr XML_RESULT_SENTENCE[XML_RESULT_COUNT] = {
    // Written for a user reading a console, so each names the document rather than the byte offset
    "the part is well-formed XML",                                                            // XML_OK
    "not a valid DOCX; a part carries a document type declaration, which is never read",      // DOCTYPE
    "not a valid DOCX; a part is malformed XML",                                              // SYNTAX
    "not a valid DOCX; a part ends in the middle of an element",                              // UNCLOSED
    "not a valid DOCX; a part closes an element it did not open",                             // MISMATCH
    "not a valid DOCX; a part nests elements deeper than the converter will follow",          // DEPTH
    "not a valid DOCX; a part uses an XML namespace prefix it never declares",                // NAMESPACE
    "not a valid DOCX; an element carries too many attributes, or carries one twice",         // ATTRIBUTES
    "not a valid DOCX; a part declares more XML namespaces at once than the converter holds", // BINDINGS
    "not a valid DOCX; a part refers to an XML entity that nothing defines",                  // ENTITY
    "not a valid DOCX; a part names a character that XML does not allow",                     // CHARACTER
    "not a valid DOCX; a part carries markup outside its root element",                       // TRAILING
    "not a valid DOCX; a part is encoded as UTF-16, which the converter transcodes first",    // ENCODING
    "not enough memory to read a part"                                                        // MEMORY
};

//-- Small helpers

// Records a failure and turns the reader into one that only ever reports it again.
static cXML_TOKEN XmlFail(XML_READERptrc reader, cXML_RESULT result, cui64 at) {
   reader->result      = result;
   reader->errorOffset = at;
   reader->token       = XML_TOKEN_ERROR;
   return XML_TOKEN_ERROR;
}

// Compares a view against a NUL-terminated string, exactly.
static cbool XmlViewEqual(cXML_TEXT text, cchptr name) {
   ui64 index = 0;

   while(index < text.length && name[index] && text.bytes[index] == name[index]) ++index;
   return index == text.length && !name[index];
}

// Compares two views, exactly.
static cbool XmlViewsEqual(cXML_TEXT a, cXML_TEXT b) {
   if(a.length != b.length) return false;
   for(ui64 index = 0; index < a.length; ++index) {
      if(a.bytes[index] != b.bytes[index]) return false;
   }
   return true;
}

// Makes the scratch arena exist. Decoding always produces fewer bytes than it consumes, so one buffer the
// size of the whole part is both sufficient for any token and final: it never has to grow, which is what
// lets a decoded view be handed out as a pointer rather than as an offset to be patched later.
static cbool XmlScratchReady(XML_READERptrc reader) {
   if(reader->scratch) return true;
   reader->scratch = (chptr)amalloc(reader->byteCount + UTF_MAX_ENCODED + 1u, 16u);
   if(!reader->scratch) return false;
   reader->scratchBytes = reader->byteCount + UTF_MAX_ENCODED + 1u;
   return true;
}

// Appends one byte to the scratch arena. The caller has already made it exist.
static void XmlScratchByte(XML_READERptrc reader, cui8 byte) {
   if(reader->scratchUsed < reader->scratchBytes) reader->scratch[reader->scratchUsed++] = char(byte);
}

//-- Names and namespaces

// Splits a qualified name at its one colon. A second colon, a leading one or a trailing one is a
// namespace error rather than a syntax one: the name is well formed, it just cannot be resolved.
static cbool XmlSplitName(cXML_TEXT qualified, XML_TEXTptr prefix, XML_TEXTptr local) {
   ui64 colon = qualified.length;

   for(ui64 index = 0; index < qualified.length; ++index) {
      if(qualified.bytes[index] != ':') continue;
      if(colon != qualified.length) return false; // A second colon
      colon = index;
   }
   if(colon == qualified.length) {
      *prefix = {nullptr, 0};
      *local  = qualified;
      return true;
   }
   if(!colon || colon + 1u == qualified.length) return false; // A leading or trailing colon

   *prefix = {qualified.bytes, colon};
   *local  = {qualified.bytes + colon + 1u, qualified.length - colon - 1u};
   return true;
}

// Which known namespace a URI is.
static cXML_NS XmlSpaceOfUri(cXML_TEXT uri) {
   for(ui32 index = 0; index < XML_NS_URI_COUNT; ++index) {
      if(XmlViewEqual(uri, XML_NS_URIS[index].uri)) return XML_NS_URIS[index].space;
   }
   return XML_NS_OTHER;
}

// Resolves a prefix against the bindings in force, innermost first. An empty prefix asks for the default
// namespace, which is absent until an xmlns declaration supplies one.
static cbool XmlResolvePrefix(cXML_READERptr reader, cXML_TEXT prefix, XML_TEXTptr uri, XML_NSptr space) {
   if(prefix.length == 3u && prefix.bytes[0] == 'x' && prefix.bytes[1] == 'm' && prefix.bytes[2] == 'l') {
      *uri   = {XML_URI_XML, XmlStaticLength(XML_URI_XML)};
      *space = XML_NS_XML;
      return true;
   }
   for(ui32 index = reader->bindingCount; index-- > 0;) {
      if(!XmlViewsEqual(reader->bindings[index].prefix, prefix)) continue;
      if(!reader->bindings[index].uri.length) break; // Undeclared again by xmlns="" or xmlns:p=""
      *uri   = reader->bindings[index].uri;
      *space = reader->bindings[index].space;
      return true;
   }
   *uri   = {nullptr, 0};
   *space = XML_NS_NONE;
   return !prefix.length; // No default namespace is legal; an unresolved prefix is not
}

//-- References

// Whether a code point may appear in an XML document at all, per XML 1.0's Char production.
static cbool XmlIsChar(cui32 point) {
   if(point == 0x09u || point == 0x0Au || point == 0x0Du) return true;
   if(point >= 0x20u && point <= 0xD7FFu) return true;
   if(point >= 0xE000u && point <= 0xFFFDu) return true;
   return point >= 0x10000u && point <= 0x10FFFFu;
}

// Decodes one character or entity reference into the scratch arena and steps past its semicolon. The
// limit is the end of the construct the reference sits in, so a reference with no semicolon fails inside
// its own value or text run rather than swallowing whatever follows the quote or the tag.
// Nothing decoded is ever scanned again, so &#38;#38; yields the five bytes &#38; and not an ampersand
// that goes on to be read as the start of another reference.
static cXML_RESULT XmlDecodeReference(XML_READERptrc reader, ui64ptrc at, cui64 limit) {
   cui8ptr bytes = reader->bytes;
   ui64    index = *at + 1u; // Past the ampersand
   ui32    point = 0;

   reader->errorOffset = *at;
   if(index < limit && bytes[index] == '#') {
      cbool hex  = (index + 1u < limit && bytes[index + 1u] == 'x');
      ui32  seen = 0;

      index += (hex ? 2u : 1u);
      while(index < limit && bytes[index] != ';') {
         cui32 byte = bytes[index];
         ui32  value;

         if(byte >= '0' && byte <= '9') value = byte - ui32('0');
         else if(hex && byte >= 'a' && byte <= 'f') value = byte - ui32('a') + 10u;
         else if(hex && byte >= 'A' && byte <= 'F') value = byte - ui32('A') + 10u;
         else return XML_ERROR_ENTITY;

         // Pinned rather than accumulated once it is out of range, so a reference with fifty digits
         // cannot wrap the accumulator back into the legal span.
         if(point <= 0x10FFFFu) point = point * (hex ? 16u : 10u) + value;
         if(point > 0x10FFFFu) point = 0x110000u;
         ++seen;
         ++index;
      }
      if(index >= limit || !seen) return XML_ERROR_ENTITY;
      ++index;
      if(!XmlIsChar(point)) return XML_ERROR_CHARACTER;
   } else {
      ui64 start = index;

      while(index < limit && bytes[index] != ';') ++index;
      if(index >= limit) return XML_ERROR_ENTITY;

      cXML_TEXT name = {(cchptr)bytes + start, index - start};

      ++index;
      // The five XML defines. There is no document type declaration to define any others, because one is
      // refused outright, so an unknown name here is an error rather than something to look up.
      if(XmlViewEqual(name, "amp")) point = ui32('&');
      else if(XmlViewEqual(name, "lt")) point = ui32('<');
      else if(XmlViewEqual(name, "gt")) point = ui32('>');
      else if(XmlViewEqual(name, "quot")) point = ui32('"');
      else if(XmlViewEqual(name, "apos")) point = ui32('\'');
      else return XML_ERROR_ENTITY;
   }

   ui8   encoded[UTF_MAX_ENCODED];
   cui32 length = UtfEncode(point, encoded);

   if(!XmlScratchReady(reader)) return XML_ERROR_MEMORY;
   for(ui32 byte = 0; byte < length; ++byte) XmlScratchByte(reader, encoded[byte]);
   *at = index;
   return XML_OK;
}

//-- Attribute values

// Reads one quoted attribute value, decoding references and normalising whitespace per XML 1.0 3.3.3.
// A value with none of that in it is handed back as a view straight into the part, which is the case
// every style id, every numeric value and almost every relationship target falls into.
static cXML_RESULT XmlScanValue(XML_READERptrc reader, ui64ptrc at, XML_TEXTptr value) {
   cui8ptr bytes = reader->bytes;
   cui64   limit = reader->byteCount;
   cui8    quote = (*at < limit ? bytes[*at] : 0u);

   reader->errorOffset = *at;
   if(quote != '"' && quote != '\'') return XML_ERROR_SYNTAX;

   cui64 start  = *at + 1u;
   ui64  index  = start;
   bool  simple = true;

   while(index < limit && bytes[index] != quote) {
      cui8 byte = bytes[index];

      if(XML_CLASSES.slot[byte] & XML_CLASS_BAD) {
         reader->errorOffset = index;
         return XML_ERROR_CHARACTER;
      }
      // A literal < in a value is malformed, and saying so here rather than letting the scan run to the
      // next quote keeps the report next to the mistake instead of a screenful past it.
      if(byte == '<') {
         reader->errorOffset = index;
         return XML_ERROR_SYNTAX;
      }
      if(byte == '&' || byte == '\r' || byte == '\n' || byte == '\t') simple = false;
      ++index;
   }
   if(index >= limit) return XML_ERROR_UNCLOSED;
   if(simple) {
      *value = {(cchptr)bytes + start, index - start};
      *at    = index + 1u;
      return XML_OK;
   }
   if(!XmlScratchReady(reader)) return XML_ERROR_MEMORY;

   cui64 built = reader->scratchUsed;
   ui64  walk  = start;

   while(walk < index) {
      cui8 byte = bytes[walk];

      if(byte == '&') {
         cXML_RESULT decoded = XmlDecodeReference(reader, &walk, index);

         if(decoded != XML_OK) return decoded;
         continue;
      }
      // Line ends normalise to one linefeed first, and every literal whitespace byte then becomes a
      // space. A character reference to whitespace is deliberately left alone: it is not literal.
      if(byte == '\r' && walk + 1u < index && bytes[walk + 1u] == '\n') ++walk;
      XmlScratchByte(reader, ui8(byte == '\r' || byte == '\n' || byte == '\t' ? ' ' : byte));
      ++walk;
   }
   *value = {reader->scratch + built, reader->scratchUsed - built};
   *at    = index + 1u;
   return XML_OK;
}

//-- Tags

// Steps over any run of XML whitespace.
static cui64 XmlSkipSpace(cui8ptr bytes, cui64 at, cui64 limit) {
   ui64 index = at;

   while(index < limit && (XML_CLASSES.slot[bytes[index]] & XML_CLASS_SPACE)) ++index;
   return index;
}

// Reads one name, which must begin with a name-start byte.
static cbool XmlScanName(cui8ptr bytes, ui64ptrc at, cui64 limit, XML_TEXTptr name) {
   cui64 start = *at;
   ui64  index = start;

   if(index >= limit || !(XML_CLASSES.slot[bytes[index]] & XML_CLASS_START)) return false;
   while(index < limit && (XML_CLASSES.slot[bytes[index]] & XML_CLASS_NAME)) ++index;
   *name = {(cchptr)bytes + start, index - start};
   *at   = index;
   return true;
}

// Records one namespace declaration for the element being opened. The prefix is empty for a default
// declaration, and an empty URI undeclares whatever was bound before, which xmlns="" is defined to do.
static cXML_RESULT XmlPushBinding(XML_READERptrc reader, cXML_TEXT prefix, cXML_TEXT uri, cui32 pushed) {
   if(XmlViewEqual(prefix, "xmlns")) return XML_ERROR_NAMESPACE; // xmlns: is not a declarable prefix
   if(XmlViewEqual(prefix, "xml") && !XmlViewEqual(uri, XML_URI_XML)) return XML_ERROR_NAMESPACE;
   if(XmlViewEqual(uri, XML_URI_XMLNS)) return XML_ERROR_NAMESPACE; // Nothing may be bound to it
   if(reader->bindingCount >= XML_MAX_NAMESPACES) return XML_ERROR_BINDINGS;

   // A prefix declared twice on one element is a duplicate attribute, which XML forbids; only the
   // declarations this element itself pushed are candidates, because an outer one is legitimately shadowed.
   for(ui32 index = reader->bindingCount - pushed; index < reader->bindingCount; ++index) {
      if(XmlViewsEqual(reader->bindings[index].prefix, prefix)) return XML_ERROR_ATTRIBUTES;
   }
   reader->bindings[reader->bindingCount++] = {prefix, uri, (uri.length ? XmlSpaceOfUri(uri) : XML_NS_NONE)};
   return XML_OK;
}

// Reads a start tag, its attributes and its namespace declarations, and opens the element.
static cXML_TOKEN XmlParseStartTag(XML_READERptrc reader) {
   cui8ptr bytes = reader->bytes;
   cui64   limit = reader->byteCount;
   cui64   tagAt = reader->at;
   ui64    at    = reader->at + 1u;

   XML_TEXT qualified = {nullptr, 0};

   // Refused before anything is parsed: a second root, or any element after the root closed, is junk, and
   // an element deeper than the stack holds is where a part built of a million open tags is stopped.
   if(reader->closedRoot || (reader->sawRoot && !reader->openCount)) return XmlFail(reader, XML_ERROR_TRAILING, tagAt);
   if(reader->openCount >= XML_MAX_DEPTH) return XmlFail(reader, XML_ERROR_DEPTH, tagAt);
   if(!XmlScanName(bytes, &at, limit, &qualified)) return XmlFail(reader, XML_ERROR_SYNTAX, at);

   cui64 floor   = reader->scratchUsed;
   ui32  pushed  = 0;
   bool  spaced  = true; // Whitespace is required between a name and an attribute, and between attributes
   bool  closing = false;

   reader->attributeCount = 0;
   for(;;) {
      cui64 before = at;

      at     = XmlSkipSpace(bytes, at, limit);
      spaced = (at > before);
      if(at >= limit) return XmlFail(reader, XML_ERROR_UNCLOSED, tagAt);
      if(bytes[at] == '>') {
         ++at;
         break;
      }
      if(bytes[at] == '/') {
         if(at + 1u >= limit || bytes[at + 1u] != '>') return XmlFail(reader, XML_ERROR_SYNTAX, at);
         at += 2u;
         closing = true;
         break;
      }
      if(!spaced) return XmlFail(reader, XML_ERROR_SYNTAX, at);

      XML_TEXT attribute = {nullptr, 0};

      if(!XmlScanName(bytes, &at, limit, &attribute)) return XmlFail(reader, XML_ERROR_SYNTAX, at);
      at = XmlSkipSpace(bytes, at, limit);
      if(at >= limit || bytes[at] != '=') return XmlFail(reader, XML_ERROR_SYNTAX, at);
      at = XmlSkipSpace(bytes, at + 1u, limit);

      XML_TEXT value = {nullptr, 0};

      cXML_RESULT read = XmlScanValue(reader, &at, &value);

      if(read != XML_OK) return XmlFail(reader, read, reader->errorOffset);

      XML_TEXT prefix = {nullptr, 0};
      XML_TEXT local  = {nullptr, 0};

      if(!XmlSplitName(attribute, &prefix, &local)) return XmlFail(reader, XML_ERROR_NAMESPACE, at);

      // A namespace declaration is not an attribute a caller ever sees; it is scoping, and it takes
      // effect for this element's own name too, whatever order the attributes happen to be written in.
      if(XmlViewEqual(attribute, "xmlns")) {
         cXML_RESULT bound = XmlPushBinding(reader, {nullptr, 0}, value, pushed);

         if(bound != XML_OK) return XmlFail(reader, bound, at);
         ++pushed;
         continue;
      }
      if(XmlViewEqual(prefix, "xmlns")) {
         cXML_RESULT bound = XmlPushBinding(reader, local, value, pushed);

         if(bound != XML_OK) return XmlFail(reader, bound, at);
         ++pushed;
         continue;
      }
      if(reader->attributeCount >= XML_MAX_ATTRIBUTES) return XmlFail(reader, XML_ERROR_ATTRIBUTES, at);
      // The qualified name is what is stored for now; the resolution pass below splits it, because a
      // declaration further along the same tag can still change what its prefix means.
      reader->attributes[reader->attributeCount++] = {attribute, value, {nullptr, 0}, XML_NS_NONE};
   }

   // Resolution comes after the whole tag has been read, so a prefix declared on this element resolves
   // for this element, and an attribute written before its own xmlns declaration resolves too.
   XML_TEXT prefix = {nullptr, 0};
   XML_TEXT local  = {nullptr, 0};

   if(!XmlSplitName(qualified, &prefix, &local)) return XmlFail(reader, XML_ERROR_NAMESPACE, tagAt);
   if(!XmlResolvePrefix(reader, prefix, &reader->uri, &reader->space)) return XmlFail(reader, XML_ERROR_NAMESPACE, tagAt);
   reader->name   = local;
   reader->prefix = prefix;

   bool preserve = (reader->openCount ? reader->elements[reader->openCount - 1u].preserve : false);

   for(ui32 index = 0; index < reader->attributeCount; ++index) {
      XML_TEXT attributePrefix = {nullptr, 0};
      XML_TEXT attributeLocal  = {nullptr, 0};
      XML_TEXT attributeUri    = {nullptr, 0};

      XmlSplitName(reader->attributes[index].name, &attributePrefix, &attributeLocal);
      // An unprefixed attribute is in no namespace at all, whatever default a surrounding xmlns declares:
      // a default namespace binds element names only, which is why Id, Type and Target in a relationship
      // part are XML_NS_NONE even though the part declares a default namespace on its root.
      if(attributePrefix.length) {
         if(!XmlResolvePrefix(reader, attributePrefix, &attributeUri, &reader->attributes[index].space)) {
            return XmlFail(reader, XML_ERROR_NAMESPACE, tagAt);
         }
      }
      reader->attributes[index].name = attributeLocal;
      reader->attributes[index].uri  = attributeUri;
      if(reader->attributes[index].space == XML_NS_XML && XmlViewEqual(attributeLocal, "space")) {
         preserve = XmlViewEqual(reader->attributes[index].value, "preserve");
      }
   }
   // Two attributes are the same one when their namespace URI and local name both match. Comparing the
   // resolved namespace *value* instead would be wrong: every namespace this build does not know is
   // XML_NS_OTHER, so w14:id and w15:id -- two different namespaces, both unknown -- would collide.
   for(ui32 index = 1u; index < reader->attributeCount; ++index) {
      for(ui32 earlier = 0; earlier < index; ++earlier) {
         if(!XmlViewsEqual(reader->attributes[index].uri, reader->attributes[earlier].uri)) continue;
         if(XmlViewsEqual(reader->attributes[index].name, reader->attributes[earlier].name)) {
            return XmlFail(reader, XML_ERROR_ATTRIBUTES, tagAt);
         }
      }
   }
   // Whatever this tag decoded stays: a namespace URI that needed decoding lives in the arena, and the
   // binding that points at it outlives this token. Rewinding to zero on the next token would leave the
   // binding aimed at bytes the next token overwrites -- which silently mis-resolves prefixes and makes
   // two distinct namespaces compare equal. Keeping the whole tag's decode is a few bytes more than
   // strictly needed and costs nothing: the floors down one path are disjoint spans of the part, so the
   // arena's byteCount-sized proof still holds.
   reader->elements[reader->openCount] = {qualified, (pushed ? reader->scratchUsed : floor), pushed, preserve};
   ++reader->openCount;
   reader->depth         = reader->openCount;
   reader->preserveSpace = preserve;
   reader->sawRoot       = true;
   reader->pendingEnd    = closing;
   reader->at            = at;
   reader->token         = XML_TOKEN_START_ELEMENT;
   return XML_TOKEN_START_ELEMENT;
}

// Closes the innermost element and reports it. Its namespace is resolved before its own declarations are
// popped, so an element that declared the prefix it is written with still reports the right namespace.
static cXML_TOKEN XmlCloseElement(XML_READERptrc reader, cui64 at) {
   cXML_ELEMENT element = reader->elements[reader->openCount - 1u];

   XML_TEXT prefix = {nullptr, 0};
   XML_TEXT local  = {nullptr, 0};

   if(!XmlSplitName(element.name, &prefix, &local)) return XmlFail(reader, XML_ERROR_NAMESPACE, at);
   if(!XmlResolvePrefix(reader, prefix, &reader->uri, &reader->space)) return XmlFail(reader, XML_ERROR_NAMESPACE, at);

   reader->name           = local;
   reader->prefix         = prefix;
   reader->depth          = reader->openCount;
   reader->attributeCount = 0;
   reader->bindingCount -= element.bindings;
   --reader->openCount;
   reader->preserveSpace = (reader->openCount ? reader->elements[reader->openCount - 1u].preserve : false);
   reader->closedRoot    = !reader->openCount;
   reader->token         = XML_TOKEN_END_ELEMENT;
   return XML_TOKEN_END_ELEMENT;
}

// Reads an end tag and matches it against the element that is open.
static cXML_TOKEN XmlParseEndTag(XML_READERptrc reader) {
   cui8ptr bytes = reader->bytes;
   cui64   limit = reader->byteCount;
   cui64   tagAt = reader->at;
   ui64    at    = reader->at + 2u;

   XML_TEXT qualified = {nullptr, 0};

   if(!XmlScanName(bytes, &at, limit, &qualified)) return XmlFail(reader, XML_ERROR_SYNTAX, at);
   at = XmlSkipSpace(bytes, at, limit);
   if(at >= limit) return XmlFail(reader, XML_ERROR_UNCLOSED, tagAt);
   if(bytes[at] != '>') return XmlFail(reader, XML_ERROR_SYNTAX, at);
   if(!reader->openCount) return XmlFail(reader, XML_ERROR_MISMATCH, tagAt);
   if(!XmlViewsEqual(qualified, reader->elements[reader->openCount - 1u].name)) return XmlFail(reader, XML_ERROR_MISMATCH, tagAt);

   reader->at = at + 1u;
   return XmlCloseElement(reader, tagAt);
}

//-- Character data

// Whether a literal stands at this offset. A truncated match reports false, so a caller has to decide
// for itself whether "could not tell" means malformed or means the part simply ended.
static cbool XmlMatches(cui8ptr bytes, cui64 at, cui64 limit, cchptr text) {
   ui64 index = 0;

   while(text[index]) {
      if(at + index >= limit || bytes[at + index] != ui8(text[index])) return false;
      ++index;
   }
   return true;
}

// Whether a CDATA section opens at this offset.
static cbool XmlIsCdata(cui8ptr bytes, cui64 at, cui64 limit) { return XmlMatches(bytes, at, limit, "<![CDATA["); }

// Reads one run of character data, folding CDATA sections and references into it. A comment or a
// processing instruction ends the run instead, because merging across one would force a copy of text that
// otherwise never has to be touched.
static cXML_TOKEN XmlParseText(XML_READERptrc reader) {
   cui8ptr bytes  = reader->bytes;
   cui64   limit  = reader->byteCount;
   cui64   start  = reader->at;
   ui64    index  = start;
   bool    simple = true;

   while(index < limit && bytes[index] != '<') {
      cui8 byte = bytes[index];

      if(XML_CLASSES.slot[byte] & XML_CLASS_BAD) return XmlFail(reader, XML_ERROR_CHARACTER, index);
      if(byte == '&' || byte == '\r') simple = false;
      ++index;
   }
   if(simple && !XmlIsCdata(bytes, index, limit)) {
      reader->text = {(cchptr)bytes + start, index - start};
      reader->at   = index;
   } else {
      if(!XmlScratchReady(reader)) return XmlFail(reader, XML_ERROR_MEMORY, start);

      cui64 built = reader->scratchUsed;
      ui64  walk  = start;
      ui64  stop  = start;

      for(;;) {
         if(walk >= limit) break;

         // Where this run of character data ends, found once and reused. Recomputing it for every
         // reference would cost a run of n references n times the run's length: a few kilobytes of
         // "&amp;" would take minutes, and a quarter of a megabyte would never finish. The cursor only
         // ever moves forward, and each recompute steps past a '<', so the total stays linear.
         if(walk >= stop) {
            stop = walk;
            while(stop < limit && bytes[stop] != '<') ++stop;
         }
         if(bytes[walk] == '<') {
            if(!XmlIsCdata(bytes, walk, limit)) break;
            walk += 9u;

            // A CDATA section ends at the first ]]> and holds no markup and no references, but XML's
            // line-end normalisation still reaches inside it.
            for(;;) {
               if(walk >= limit) return XmlFail(reader, XML_ERROR_UNCLOSED, start);
               if(walk + 3u <= limit && bytes[walk] == ']' && bytes[walk + 1u] == ']' && bytes[walk + 2u] == '>') {
                  walk += 3u;
                  break;
               }
               if(XML_CLASSES.slot[bytes[walk]] & XML_CLASS_BAD) return XmlFail(reader, XML_ERROR_CHARACTER, walk);
               if(bytes[walk] == '\r') {
                  if(walk + 1u < limit && bytes[walk + 1u] == '\n') ++walk;
                  XmlScratchByte(reader, ui8('\n'));
               } else {
                  XmlScratchByte(reader, bytes[walk]);
               }
               ++walk;
            }
            continue;
         }
         if(bytes[walk] == '&') {
            cXML_RESULT decoded = XmlDecodeReference(reader, &walk, stop);

            if(decoded != XML_OK) return XmlFail(reader, decoded, reader->errorOffset);
            continue;
         }
         if(XML_CLASSES.slot[bytes[walk]] & XML_CLASS_BAD) return XmlFail(reader, XML_ERROR_CHARACTER, walk);
         if(bytes[walk] == '\r') {
            if(walk + 1u < limit && bytes[walk + 1u] == '\n') ++walk;
            XmlScratchByte(reader, ui8('\n'));
         } else {
            XmlScratchByte(reader, bytes[walk]);
         }
         ++walk;
      }
      reader->text = {reader->scratch + built, reader->scratchUsed - built};
      reader->at   = walk;
   }

   bool blank = true;

   for(ui64 byte = 0; byte < reader->text.length; ++byte) {
      if(!(XML_CLASSES.slot[ui8(reader->text.bytes[byte])] & XML_CLASS_SPACE)) blank = false;
   }
   reader->allWhitespace = blank;
   reader->token         = XML_TOKEN_TEXT;
   return XML_TOKEN_TEXT;
}

//== Entry points

cXML_RESULT XmlOpen(XML_READERptrc reader, cui8ptr bytes, cui64 byteCount) {
   mzero(reader, sizeof(XML_READER));
   reader->bytes     = bytes;
   reader->byteCount = byteCount;
   reader->token     = XML_TOKEN_NONE;
   reader->result    = XML_OK;

   cui64 mark = UtfBomBytes(bytes, byteCount);

   if(mark == 2u) {
      reader->result = XML_ERROR_ENCODING;
      reader->token  = XML_TOKEN_ERROR;
      return XML_ERROR_ENCODING;
   }
   reader->at = mark;
   return XML_OK;
}

void XmlClose(XML_READERptrc reader) {
   mdealloc(reader->scratch);
   reader->scratch      = nullptr;
   reader->scratchBytes = 0;
   reader->scratchUsed  = 0;
}

cXML_TOKEN XmlNext(XML_READERptrc reader) {
   if(reader->token == XML_TOKEN_ERROR || reader->token == XML_TOKEN_END_OF_INPUT) return reader->token;

   // The arena is reused from every token, which is what makes the decoded views cheap and what makes
   // them expire the moment the next token is read -- down to the innermost open element's floor rather
   // than to zero, because below that floor sit the namespace URIs its bindings point at.
   reader->scratchUsed = (reader->openCount ? reader->elements[reader->openCount - 1u].scratchFloor : 0);
   if(reader->pendingEnd) {
      reader->pendingEnd = false;
      return XmlCloseElement(reader, reader->at);
   }
   for(;;) {
      if(reader->at >= reader->byteCount) {
         if(reader->openCount) return XmlFail(reader, XML_ERROR_UNCLOSED, reader->at);
         if(!reader->sawRoot) return XmlFail(reader, XML_ERROR_TRAILING, reader->at);
         reader->token = XML_TOKEN_END_OF_INPUT;
         return XML_TOKEN_END_OF_INPUT;
      }

      cui8ptr bytes = reader->bytes;
      cui64   limit = reader->byteCount;
      cui64   at    = reader->at;

      if(bytes[at] != '<' || XmlIsCdata(bytes, at, limit)) {
         // Whitespace around the root element is legal and carries nothing; anything else out there is
         // markup that does not belong to any element, which is a truncated or spliced part.
         if(!reader->openCount) {
            if(bytes[at] != '<') {
               ui64 scan = at;

               while(scan < limit && bytes[scan] != '<') {
                  if(!(XML_CLASSES.slot[bytes[scan]] & XML_CLASS_SPACE)) return XmlFail(reader, XML_ERROR_TRAILING, scan);
                  ++scan;
               }
               reader->at = scan;
               continue;
            }
            return XmlFail(reader, XML_ERROR_TRAILING, at);
         }

         cXML_TOKEN token = XmlParseText(reader);

         if(token != XML_TOKEN_TEXT) return token;
         if(!reader->text.length) continue; // A run that decodes to nothing is not a token
         return token;
      }
      if(at + 1u >= limit) return XmlFail(reader, XML_ERROR_UNCLOSED, at);
      if(bytes[at + 1u] == '/') return XmlParseEndTag(reader);
      if(bytes[at + 1u] == '?') {
         ui64 scan = at + 2u;

         while(scan + 1u < limit && !(bytes[scan] == '?' && bytes[scan + 1u] == '>')) ++scan;
         if(scan + 1u >= limit) return XmlFail(reader, XML_ERROR_UNCLOSED, at);
         reader->at = scan + 2u;
         continue;
      }
      if(bytes[at + 1u] == '!') {
         if(at + 4u <= limit && bytes[at + 2u] == '-' && bytes[at + 3u] == '-') {
            ui64 scan = at + 4u;

            while(scan + 2u < limit && !(bytes[scan] == '-' && bytes[scan + 1u] == '-' && bytes[scan + 2u] == '>')) ++scan;
            if(scan + 2u >= limit) return XmlFail(reader, XML_ERROR_UNCLOSED, at);
            reader->at = scan + 3u;
            continue;
         }
         // A document type declaration is refused where it stands, before its internal subset is looked
         // at, which is what makes the billion-laughs and XXE families cost nothing to defend against.
         if(XmlMatches(bytes, at, limit, "<!DOCTYPE")) return XmlFail(reader, XML_ERROR_DOCTYPE, at);
         // Anything else opening with "<!" is either malformed or cut off, and saying which is the
         // difference between a useful message and one that blames a declaration that is not there.
         if(at + 9u > limit) return XmlFail(reader, XML_ERROR_UNCLOSED, at);
         return XmlFail(reader, XML_ERROR_SYNTAX, at);
      }
      return XmlParseStartTag(reader);
   }
}

cbool XmlSkipElement(XML_READERptrc reader) {
   if(reader->token != XML_TOKEN_START_ELEMENT) return false;

   cui32 target = reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_END_ELEMENT && reader->depth == target) return true;
      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
   }
}

cXML_TEXT XmlAttribute(cXML_READERptr reader, cXML_NS space, cchptr localName) {
   for(ui32 index = 0; index < reader->attributeCount; ++index) {
      if(reader->attributes[index].space != space) continue;
      if(XmlViewEqual(reader->attributes[index].name, localName)) return reader->attributes[index].value;
   }
   return {nullptr, 0};
}

cbool XmlTextEqual(cXML_TEXT text, cchptr name) { return XmlViewEqual(text, name); }

cbool XmlIsElement(cXML_READERptr reader, cXML_NS space, cchptr localName) {
   if(reader->token != XML_TOKEN_START_ELEMENT || reader->space != space) return false;
   return XmlViewEqual(reader->name, localName);
}

cchptr XmlResultText(cXML_RESULT result) {
   if(result < XML_OK || result >= XML_RESULT_COUNT) return "not a valid DOCX; a part could not be read";
   return XML_RESULT_SENTENCE[result];
}
