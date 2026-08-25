/*
 * File: OpcPackage.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: Package model implementation: content types, relationship parsing, target resolution.
 * To Do: 1) Cache a folded copy of each part name if profiling ever shows the comparator mattering.
 *        2) Normalise a backslash in an entry name at M11, which decision D10 gave that question to.
 *        3) Read docProps/core.xml for a title, once the emitter has somewhere to put one.
 * Dependencies: BuildGuards.h, Diag.h, OpcPackage.h, Utf.h, XmlPull.h, ZipReader.h, typedefs.h,
 *               memory management.h, windows.h
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
#include "Diag.h"
#include "Utf.h"
#include "XmlPull.h"
#include "ZipReader.h"
#include "OpcPackage.h"

//== Package constants

// The only two part names ISO/IEC 29500-2 guarantees. Everything else, the main document included, is
// found through relationships -- which is correctness rule 1, and the whole point of this module.
constexpr cchptr OPC_PART_CONTENT_TYPES = "[Content_Types].xml";
constexpr cchptr OPC_PART_PACKAGE_RELS  = "_rels/.rels";

// A relationship Type is one URI prefix plus the kind's own name. Both families share that shape, so
// one prefix test and one suffix table cover Transitional and Strict together.
constexpr cchptr OPC_REL_PREFIX_TRANSITIONAL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/";
constexpr cchptr OPC_REL_PREFIX_STRICT       = "http://purl.oclc.org/ooxml/officeDocument/relationships/";

// The content types a main document part may carry: .docx, .docm, .dotx and .dotm in that order. They
// are not versioned, so Strict packages use exactly these strings too.
static constexpr cchptr OPC_MAIN_CONTENT_TYPES[] = {
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml", // .docx
    "application/vnd.ms-word.document.macroEnabled.main+xml",                           // .docm
    "application/vnd.openxmlformats-officedocument.wordprocessingml.template.main+xml", // .dotx
    "application/vnd.ms-word.template.macroEnabledTemplate.main+xml"                    // .dotm
};

constexpr cui32 OPC_MAIN_CONTENT_TYPE_COUNT = ui32(sizeof(OPC_MAIN_CONTENT_TYPES) / sizeof(OPC_MAIN_CONTENT_TYPES[0]));

// The suffix of each relationship Type URI this build acts on, indexed to match OPC_REL_KIND.
static constexpr cchptr OPC_REL_SUFFIX[OPC_REL_KIND_COUNT] = {
    // One per OPC_REL_KIND, in the order the enum declares them
    "",               // OPC_REL_OTHER, which nothing matches
    "officeDocument", // OFFICE_DOCUMENT
    "styles",         // STYLES
    "numbering",      // NUMBERING
    "settings",       // SETTINGS
    "footnotes",      // FOOTNOTES
    "endnotes",       // ENDNOTES
    "comments",       // COMMENTS
    "image",          // IMAGE
    "hyperlink",      // HYPERLINK
    "header",         // HEADER
    "footer",         // FOOTER
    "theme"           // THEME
};

// One sentence per OPC_RESULT, in the order the enum declares them.
static constexpr cchptr OPC_RESULT_SENTENCE[OPC_RESULT_COUNT] = {
    // Written for a user reading a console, so each names the document rather than the structure
    "the package is readable",                                                             // OPC_OK
    "not enough memory to read the package",                                               // MEMORY
    "the converter asked for a part that is not in the package",                           // RANGE
    "the container could not be read",                                                     // ZIP
    "not a valid DOCX; the package has no [Content_Types].xml",                            // NO_CONTENT_TYPES
    "not a valid DOCX; the package has no _rels/.rels",                                    // NO_PACKAGE_RELS
    "not a valid DOCX; a content types part does not declare content types",               // CT_MALFORMED
    "not a valid DOCX; a relationships part does not hold relationships",                  // RELS_MALFORMED
    "not a valid DOCX; a part is not valid UTF-8 text",                                    // NOT_UTF8
    "not a valid DOCX; a part is not well-formed XML",                                     // XML
    "not a valid DOCX; _rels/.rels names no main document part",                           // NO_MAIN_REL
    "not a valid DOCX; the main document part the package names is not in the archive",    // MAIN_PART_MISSING
    "not a valid DOCX; a relationship points somewhere that is not a part of the package", // REL_TARGET
    "not a valid DOCX; the package declares more structure than the converter will read"   // LIMIT
};

//-- Strings

// Length of a NUL-terminated string.
static cui64 OpcLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// ASCII lowercase of one byte. OPC part names compare case-insensitively, and only in ASCII: the
// specification says nothing about folding anything else, and neither does any producer.
static cchar OpcFold(cchar byte) { return (byte >= 'A' && byte <= 'Z' ? char(byte - 'A' + 'a') : byte); }

// Case-insensitive comparison of two NUL-terminated names.
static cbool OpcNameEqual(cchptr a, cchptr b) {
   ui64 index = 0;

   while(a[index] && OpcFold(a[index]) == OpcFold(b[index])) ++index;
   return !a[index] && !b[index];
}

// Exact comparison of two NUL-terminated strings.
static cbool OpcTextEqual(cchptr a, cchptr b) {
   ui64 index = 0;

   while(a[index] && a[index] == b[index]) ++index;
   return a[index] == b[index];
}

// Whether text begins with a prefix, exactly.
static cbool OpcStartsWith(cchptr text, cchptr prefix) {
   ui64 index = 0;

   while(prefix[index] && text[index] == prefix[index]) ++index;
   return !prefix[index];
}

//-- Growable storage

// Grows a block to hold at least the requested number of elements, doubling so that filling one costs
// amortised constant time. Offsets survive a move, which is why every reference into the heap is one.
static cbool OpcReserve(ptrptrc block, ui64ptrc capacity, cui64 needed, cui64 unit) {
   if(needed <= *capacity) return true;

   ui64 grown = (*capacity ? *capacity : 64u);

   while(grown < needed) grown *= 2u;

   ptr fresh = amalloc(grown * unit, 32u);

   if(!fresh) return false;
   if(*block) Copy(*block, fresh, *capacity * unit);
   mdealloc(*block);
   *block    = fresh;
   *capacity = grown;
   return true;
}

// Copies a string into the package heap and reports where it landed. Offset 0 is always the empty
// string, so a zero offset is a usable value rather than a sentinel a caller has to test for.
static cbool OpcHeapAdd(OPC_PACKAGEptrc package, cchptr text, cui64 length, ui32ptrc at) {
   if(!OpcReserve((ptrptrc)&package->heap, &package->heapCapacity, package->heapUsed + length + 1u, 1u)) return false;
   *at = ui32(package->heapUsed);
   for(ui64 index = 0; index < length; ++index) package->heap[package->heapUsed + index] = text[index];
   package->heap[package->heapUsed + length] = 0;
   package->heapUsed += length + 1u;
   return true;
}

// The string one heap offset names.
static cchptr OpcHeapText(cOPC_PACKAGEptr package, cui32 at) { return (package->heap ? package->heap + at : ""); }

//-- Part names

// Skips a leading '/' so an OPC part name and a ZIP entry name compare as the same thing.
static cchptr OpcEntryForm(cchptr partName) { return (partName[0] == '/' ? partName + 1u : partName); }

cOPC_RESULT OpcRelsPartName(cchptr partName, chptrc relsName, cui64 relsCapacity) {
   cui64 length = OpcLength(partName);

   if(!length || partName[0] != '/') return OPC_ERROR_REL_TARGET;

   ui64 lastSlash = 0;

   for(ui64 index = 0; index < length; ++index) {
      if(partName[index] == '/') lastSlash = index;
   }

   cchptr leaf      = partName + lastSlash + 1u;
   cui64  leafBytes = length - lastSlash - 1u;
   cui64  needed    = lastSlash + 1u + 6u + leafBytes + 5u + 1u; // <dir>/ + "_rels/" + leaf + ".rels" + NUL

   if(needed > relsCapacity) return OPC_ERROR_REL_TARGET;

   ui64 used = 0;

   for(ui64 index = 0; index <= lastSlash; ++index) relsName[used++] = partName[index];
   for(cchptr walk = "_rels/"; *walk; ++walk) relsName[used++] = *walk;
   for(ui64 index = 0; index < leafBytes; ++index) relsName[used++] = leaf[index];
   for(cchptr walk = ".rels"; *walk; ++walk) relsName[used++] = *walk;
   relsName[used] = 0;
   return OPC_OK;
}

//-- Target resolution

// Whether a string opens with a URI scheme: a letter, then letters, digits, '+', '-' or '.', then ':'.
// One rule covers file:// URLs, an http:// target mislabelled Internal, and a bare C: drive letter,
// which is a grammatically valid one-letter scheme and is exactly how a traversal attempt is spelled.
static cbool OpcHasScheme(cchptr text, cui64 length) {
   cbool alpha = (length && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')));

   if(!alpha) return false;
   for(ui64 index = 1u; index < length; ++index) {
      cchar byte = text[index];

      if(byte == ':') return true;

      cbool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
      cbool digit  = (byte >= '0' && byte <= '9');
      cbool extra  = (byte == '+' || byte == '-' || byte == '.');

      if(!letter && !digit && !extra) return false;
   }
   return false;
}

// The value of one hex digit, or 16 when the byte is not one.
static cui32 OpcHexValue(cchar byte) {
   if(byte >= '0' && byte <= '9') return ui32(byte - '0');
   if(byte >= 'a' && byte <= 'f') return ui32(byte - 'a') + 10u;
   if(byte >= 'A' && byte <= 'F') return ui32(byte - 'A') + 10u;
   return 16u;
}

// Whether a byte may not appear in a part name, once every escape has been decoded.
static cbool OpcNameByteBanned(cchar byte) {
   if(ui8(byte) < 0x20u || ui8(byte) == 0x7Fu) return true;
   return byte == ':' || byte == '\\' || byte == '?' || byte == '#' || byte == '*' || byte == '"' || byte == '<' || byte == '>' || byte == '|';
}

cOPC_RESULT OpcResolveTarget(cchptr sourcePartName, cchptr target, cbool external, chptrc resolved, cui64 resolvedCapacity) {
   if(!resolvedCapacity) return OPC_ERROR_REL_TARGET;
   resolved[0] = 0;

   cui64 targetBytes = OpcLength(target);

   if(!targetBytes || targetBytes > OPC_MAX_TARGET_BYTES) return OPC_ERROR_REL_TARGET;

   // An external target is a URI and not a part name. It is copied through untouched: normalising it,
   // decoding it or resolving it against a part would all be wrong, and none of it is ever opened.
   if(external) {
      if(targetBytes + 1u > resolvedCapacity) return OPC_ERROR_REL_TARGET;
      for(ui64 index = 0; index <= targetBytes; ++index) resolved[index] = target[index];
      return OPC_OK;
   }
   for(ui64 index = 0; index < targetBytes; ++index) {
      cchar byte = target[index];

      if(ui8(byte) < 0x20u || ui8(byte) == 0x7Fu) return OPC_ERROR_REL_TARGET;
      if(byte == '\\' || byte == '#' || byte == '?') return OPC_ERROR_REL_TARGET;
   }
   if(OpcHasScheme(target, targetBytes)) return OPC_ERROR_REL_TARGET;
   if(targetBytes >= 2u && target[0] == '/' && target[1] == '/') return OPC_ERROR_REL_TARGET;

   char joined[OPC_MAX_PART_BYTES + OPC_MAX_TARGET_BYTES + 2u];
   ui64 length = 0;

   if(target[0] == '/') {
      for(ui64 index = 0; index < targetBytes; ++index) joined[length++] = target[index];
   } else {
      cui64 sourceBytes = OpcLength(sourcePartName);

      if(!sourceBytes || sourcePartName[0] != '/' || sourceBytes > OPC_MAX_PART_BYTES) return OPC_ERROR_REL_TARGET;

      ui64 lastSlash = 0;

      for(ui64 index = 0; index < sourceBytes; ++index) {
         if(sourcePartName[index] == '/') lastSlash = index;
      }
      for(ui64 index = 0; index <= lastSlash; ++index) joined[length++] = sourcePartName[index];
      for(ui64 index = 0; index < targetBytes; ++index) joined[length++] = target[index];
   }
   joined[length] = 0;

   // Dot segments are removed inside the package namespace, never against the filesystem, and a climb
   // above the root is refused rather than quietly clamped: /../../x is a broken or hostile target, and
   // turning it into /x would report "part not found" about a part the document never asked for.
   ui64 starts[OPC_MAX_SEGMENTS];
   ui64 ends[OPC_MAX_SEGMENTS];
   ui32 depth      = 0;
   bool endedOnDot = false;
   ui64 walk       = 1u; // Past the leading '/', which every joined path has

   while(walk <= length) {
      ui64 stop = walk;

      while(stop < length && joined[stop] != '/') ++stop;

      cui64 span = stop - walk;

      if(!span) return OPC_ERROR_REL_TARGET; // An empty segment, which a part name may not have
      if(span == 1u && joined[walk] == '.') {
         endedOnDot = true;
         walk       = stop + 1u;
         continue;
      }
      if(span == 2u && joined[walk] == '.' && joined[walk + 1u] == '.') {
         if(!depth) return OPC_ERROR_REL_TARGET;
         --depth;
         endedOnDot = true;
         walk       = stop + 1u;
         continue;
      }
      if(depth >= OPC_MAX_SEGMENTS) return OPC_ERROR_REL_TARGET;
      starts[depth] = walk;
      ends[depth]   = stop;
      ++depth;
      endedOnDot = false;
      walk       = stop + 1u;
   }
   // A path whose last segment was a dot segment names the folder it lands in, and a folder is never a
   // part. That is the same rule as the trailing-slash refusal above, reached by a different spelling.
   if(!depth || endedOnDot) return OPC_ERROR_REL_TARGET;

   // Rebuilt, then decoded in place: an escape always shrinks, so the decode can never outrun the buffer.
   char built[OPC_MAX_PART_BYTES + 1u];
   ui64 used = 0;

   for(ui32 index = 0; index < depth; ++index) {
      if(used + 1u + (ends[index] - starts[index]) > OPC_MAX_PART_BYTES) return OPC_ERROR_REL_TARGET;
      built[used++] = '/';
      for(ui64 byte = starts[index]; byte < ends[index]; ++byte) built[used++] = joined[byte];
   }
   built[used] = 0;

   ui64 decoded = 0;

   for(ui64 index = 0; index < used;) {
      if(built[index] != '%') {
         built[decoded++] = built[index++];
         continue;
      }
      if(index + 2u >= used) return OPC_ERROR_REL_TARGET;

      cui32 high = OpcHexValue(built[index + 1u]);
      cui32 low  = OpcHexValue(built[index + 2u]);

      if(high > 15u || low > 15u) return OPC_ERROR_REL_TARGET;

      cchar byte = char((high << 4u) | low);

      // A '/' that arrives through an escape would introduce a segment boundary the pass above never
      // saw, which is precisely the %2e%2e%2f bypass; refusing the byte closes it at the source.
      if(!byte || byte == '/' || OpcNameByteBanned(byte)) return OPC_ERROR_REL_TARGET;
      built[decoded++] = byte;
      index += 3u;
   }
   built[decoded] = 0;

   // Re-checked after decoding, because the escapes could have spelled anything the first pass rejected.
   if(decoded < 2u || built[0] != '/') return OPC_ERROR_REL_TARGET;

   // Every byte of the finished name, not only the ones an escape produced. A colon is the case that
   // makes this matter: it is an NTFS alternate-stream separator, and the scheme test above only sees
   // one that follows a letter, so "1:stream" would otherwise arrive here unexamined.
   for(ui64 index = 1u; index < decoded; ++index) {
      if(OpcNameByteBanned(built[index])) return OPC_ERROR_REL_TARGET;
   }

   // A part name is text. A percent escape can spell a byte that is not, and a name that is not UTF-8
   // would be compared against entry names byte by byte and simply never match, which is a silent
   // "not found" where the truth is "not a name".
   ui64 badOffset = 0;

   if(UtfValidate((cui8ptr)built, decoded, &badOffset) != UTF8_OK) return OPC_ERROR_REL_TARGET;

   ui64 segment = 1u;

   while(segment <= decoded) {
      ui64 stop = segment;

      while(stop < decoded && built[stop] != '/') ++stop;

      cui64 span = stop - segment;

      if(!span) return OPC_ERROR_REL_TARGET;
      if(span == 1u && built[segment] == '.') return OPC_ERROR_REL_TARGET;
      if(span == 2u && built[segment] == '.' && built[segment + 1u] == '.') return OPC_ERROR_REL_TARGET;
      if(built[stop - 1u] == '.') return OPC_ERROR_REL_TARGET; // A segment may not end with a dot
      segment = stop + 1u;
   }
   if(decoded + 1u > resolvedCapacity) return OPC_ERROR_REL_TARGET;
   for(ui64 index = 0; index <= decoded; ++index) resolved[index] = built[index];
   return OPC_OK;
}

//-- Part loading

cOPC_RESULT OpcLoadPart(OPC_PACKAGEptrc package, csi32 partIndex) {
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return OPC_ERROR_RANGE;

   OPC_PARTptr part = package->parts + partIndex;

   if(part->bytes) return OPC_OK;

   ui8ptr      bytes     = nullptr;
   ui64        byteCount = 0;
   cZIP_RESULT read      = ZipReadEntry(package->reader, ui32(partIndex), &bytes, &byteCount);

   if(read != ZIP_OK) {
      package->lastZip    = read;
      package->failedPart = partIndex;
      return OPC_ERROR_ZIP;
   }
   part->bytes     = bytes;
   part->byteCount = byteCount;
   return OPC_OK;
}

cOPC_RESULT OpcLoadXmlPart(OPC_PACKAGEptrc package, csi32 partIndex) {
   cOPC_RESULT loaded = OpcLoadPart(package, partIndex);

   if(loaded != OPC_OK) return loaded;

   OPC_PARTptr   part     = package->parts + partIndex;
   cUTF_ENCODING encoding = UtfDetectEncoding(part->bytes, part->byteCount);

   if(encoding != UTF_ENCODING_UTF8) {
      ui8ptr transcoded    = nullptr;
      ui64   producedBytes = 0;

      cUTF8_RESULT converted = UtfTranscodeUtf16(part->bytes, part->byteCount, encoding == UTF_ENCODING_UTF16_BE, &transcoded, &producedBytes);

      if(converted != UTF8_OK) {
         package->lastUtf    = converted;
         package->failedPart = partIndex;
         return (converted == UTF8_ERROR_MEMORY ? OPC_ERROR_MEMORY : OPC_ERROR_NOT_UTF8);
      }
      mdealloc(part->bytes);
      part->bytes     = transcoded;
      part->byteCount = producedBytes;
      return OPC_OK;
   }

   ui64 badOffset = 0;

   cUTF8_RESULT checked = UtfValidate(part->bytes, part->byteCount, &badOffset);

   if(checked != UTF8_OK) {
      package->lastUtf    = checked;
      package->failedPart = partIndex;
      return OPC_ERROR_NOT_UTF8;
   }
   return OPC_OK;
}

//-- Content types

// Reads [Content_Types].xml into the Default and Override tables. Both rows are stored as written and
// compared case-insensitively later, because the specification folds neither extensions nor part names.
static cOPC_RESULT OpcParseContentTypes(OPC_PACKAGEptrc package, cui8ptr bytes, cui64 byteCount) {
   XML_READER reader;

   if(XmlOpen(&reader, bytes, byteCount) != XML_OK) {
      package->lastXml = reader.result;
      XmlClose(&reader);
      return OPC_ERROR_XML;
   }

   OPC_RESULT verdict = OPC_ERROR_CONTENT_TYPES_MALFORMED;
   bool       sawRoot = false;

   for(;;) {
      cXML_TOKEN token = XmlNext(&reader);

      if(token == XML_TOKEN_ERROR) {
         package->lastXml = reader.result;
         verdict          = OPC_ERROR_XML;
         break;
      }
      if(token == XML_TOKEN_END_OF_INPUT) {
         verdict = (sawRoot ? OPC_OK : OPC_ERROR_CONTENT_TYPES_MALFORMED);
         break;
      }
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(reader.depth == 1u) {
         if(!XmlIsElement(&reader, XML_NS_CT, "Types")) break;
         sawRoot = true;
         continue;
      }
      if(reader.depth != 2u || reader.space != XML_NS_CT) {
         // Consumed whole rather than walked into and filtered child by child. The depth guard above
         // would reach the same verdict either way; skipping is the cheaper road to it.
         XmlSkipElement(&reader);
         continue;
      }

      cbool isDefault  = XmlTextEqual(reader.name, "Default");
      cbool isOverride = XmlTextEqual(reader.name, "Override");

      if(!isDefault && !isOverride) continue;

      // Both attributes are unprefixed, so they are in no namespace at all whatever default the root
      // declares -- the trap this module would otherwise fall into on every producer's file.
      cXML_TEXT key  = XmlAttribute(&reader, XML_NS_NONE, (isDefault ? "Extension" : "PartName"));
      cXML_TEXT type = XmlAttribute(&reader, XML_NS_NONE, "ContentType");

      if(!key.bytes || !type.bytes || !key.length || !type.length) continue;

      OPC_TYPE_ROWptrptr table    = (isDefault ? &package->defaults : &package->overrides);
      ui64ptrc           capacity = (isDefault ? &package->defaultCapacity : &package->overrideCapacity);
      ui32ptrc           count    = (isDefault ? &package->defaultCount : &package->overrideCount);

      if(*count >= OPC_MAX_TYPE_ROWS) {
         verdict = OPC_ERROR_LIMIT;
         break;
      }
      if(!OpcReserve((ptrptrc)table, capacity, ui64(*count) + 1u, sizeof(OPC_TYPE_ROW))) {
         verdict = OPC_ERROR_MEMORY;
         break;
      }

      OPC_TYPE_ROW row = {};

      if(!OpcHeapAdd(package, key.bytes, key.length, &row.keyAt) || !OpcHeapAdd(package, type.bytes, type.length, &row.typeAt)) {
         verdict = OPC_ERROR_MEMORY;
         break;
      }
      (*table)[(*count)++] = row;
   }
   XmlClose(&reader);
   return verdict;
}

cchptr OpcContentTypeOf(OPC_PACKAGEptrc package, csi32 partIndex) {
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return "";

   OPC_PARTptr part = package->parts + partIndex;

   if(part->typeKnown) return OpcHeapText(package, part->contentTypeAt);
   part->typeKnown = true;

   // An Override on the part's own name wins, then a Default for its extension, then nothing at all --
   // which is legal, and simply means the part is not typed.
   for(ui32 row = 0; row < package->overrideCount; ++row) {
      if(!OpcNameEqual(OpcEntryForm(OpcHeapText(package, package->overrides[row].keyAt)), part->name)) continue;
      part->contentTypeAt = package->overrides[row].typeAt;
      return OpcHeapText(package, part->contentTypeAt);
   }

   cui64 nameBytes = OpcLength(part->name);
   ui64  dot       = nameBytes;

   for(ui64 byte = 0; byte < nameBytes; ++byte) {
      if(part->name[byte] == '.') dot = byte;
      if(part->name[byte] == '/') dot = nameBytes; // A dot in a folder name types nothing
   }
   if(dot == nameBytes) return "";
   for(ui32 row = 0; row < package->defaultCount; ++row) {
      if(!OpcNameEqual(OpcHeapText(package, package->defaults[row].keyAt), part->name + dot + 1u)) continue;
      part->contentTypeAt = package->defaults[row].typeAt;
      break;
   }
   return OpcHeapText(package, part->contentTypeAt);
}

//-- Relationships

// Which relationship kind a Type URI names. Both families share one shape -- a fixed prefix and the
// kind's own name -- so one prefix test and one suffix table cover Transitional and Strict together.
static cOPC_REL_KIND OpcRelKindOf(cchptr type) {
   cchptr suffix = nullptr;

   if(OpcStartsWith(type, OPC_REL_PREFIX_TRANSITIONAL)) suffix = type + OpcLength(OPC_REL_PREFIX_TRANSITIONAL);
   else if(OpcStartsWith(type, OPC_REL_PREFIX_STRICT)) suffix = type + OpcLength(OPC_REL_PREFIX_STRICT);
   if(!suffix) return OPC_REL_OTHER;

   for(si32 kind = 1; kind < si32(OPC_REL_KIND_COUNT); ++kind) {
      if(OpcTextEqual(suffix, OPC_REL_SUFFIX[kind])) return OPC_REL_KIND(kind);
   }
   return OPC_REL_OTHER;
}

// Reads one relationships part. The source part name is what every relative Target is resolved against,
// which is why it is passed in rather than derived here: the package's own rels resolve against "/".
static cOPC_RESULT OpcParseRels(OPC_PACKAGEptrc package, cchptr sourcePartName, cui8ptr bytes, cui64 byteCount) {
   XML_READER reader;

   if(XmlOpen(&reader, bytes, byteCount) != XML_OK) {
      package->lastXml = reader.result;
      XmlClose(&reader);
      return OPC_ERROR_XML;
   }

   OPC_RESULT verdict = OPC_ERROR_RELS_MALFORMED;
   bool       sawRoot = false;

   for(;;) {
      cXML_TOKEN token = XmlNext(&reader);

      if(token == XML_TOKEN_ERROR) {
         package->lastXml = reader.result;
         verdict          = OPC_ERROR_XML;
         break;
      }
      if(token == XML_TOKEN_END_OF_INPUT) {
         verdict = (sawRoot ? OPC_OK : OPC_ERROR_RELS_MALFORMED);
         break;
      }
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(reader.depth == 1u) {
         if(!XmlIsElement(&reader, XML_NS_PR, "Relationships")) break;
         sawRoot = true;
         continue;
      }
      if(reader.depth != 2u || !XmlIsElement(&reader, XML_NS_PR, "Relationship")) {
         XmlSkipElement(&reader);
         continue;
      }

      cXML_TEXT id     = XmlAttribute(&reader, XML_NS_NONE, "Id");
      cXML_TEXT type   = XmlAttribute(&reader, XML_NS_NONE, "Type");
      cXML_TEXT target = XmlAttribute(&reader, XML_NS_NONE, "Target");
      cXML_TEXT mode   = XmlAttribute(&reader, XML_NS_NONE, "TargetMode");

      if(!id.bytes || !type.bytes || !target.bytes) {
         verdict = OPC_ERROR_RELS_MALFORMED;
         break;
      }
      if(package->relCount >= OPC_MAX_RELS) {
         verdict = OPC_ERROR_LIMIT;
         break;
      }
      if(!OpcReserve((ptrptrc)&package->rels, &package->relCapacity, ui64(package->relCount) + 1u, sizeof(OPC_REL))) {
         verdict = OPC_ERROR_MEMORY;
         break;
      }

      OPC_REL record = {};

      record.external = XmlTextEqual(mode, "External");
      if(!OpcHeapAdd(package, id.bytes, id.length, &record.idAt) || !OpcHeapAdd(package, type.bytes, type.length, &record.typeAt) ||
         !OpcHeapAdd(package, target.bytes, target.length, &record.targetAt)) {
         verdict = OPC_ERROR_MEMORY;
         break;
      }
      record.kind = OpcRelKindOf(OpcHeapText(package, record.typeAt));

      char resolved[OPC_MAX_PART_BYTES + 1u] = {};

      if(record.external) {
         record.resolvedAt = 0; // The empty string: an external target names no part of this package
      } else {
         cOPC_RESULT walked = OpcResolveTarget(sourcePartName, OpcHeapText(package, record.targetAt), false, resolved, sizeof(resolved));

         if(walked != OPC_OK) {
            verdict = walked;
            break;
         }
         if(!OpcHeapAdd(package, resolved, OpcLength(resolved), &record.resolvedAt)) {
            verdict = OPC_ERROR_MEMORY;
            break;
         }
      }
      package->rels[package->relCount++] = record;
   }
   XmlClose(&reader);
   return verdict;
}

// Where a part's relationships begin, and how many it has. The package's own live outside the parts
// array, because _rels/.rels belongs to the package rather than to any part of it.
static cui32 OpcRelsBase(cOPC_PACKAGEptr package, csi32 partIndex) {
   if(partIndex == OPC_PACKAGE_PART) return package->packageRelsAt;
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return 0;
   return package->parts[partIndex].relsAt;
}

static cui32 OpcRelsSize(cOPC_PACKAGEptr package, csi32 partIndex) {
   if(partIndex == OPC_PACKAGE_PART) return package->packageRelCount;
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return 0;
   return package->parts[partIndex].relCount;
}

cOPC_RESULT OpcLoadRels(OPC_PACKAGEptrc package, csi32 partIndex) {
   if(partIndex != OPC_PACKAGE_PART && (partIndex < 0 || ui32(partIndex) >= package->partCount)) return OPC_ERROR_RANGE;
   if(partIndex == OPC_PACKAGE_PART ? package->packageRelsLoaded : package->parts[partIndex].relsLoaded) return OPC_OK;

   char source[OPC_MAX_PART_BYTES + 2u];
   char relsName[OPC_MAX_PART_BYTES + 16u];
   ui64 used = 0;

   source[used++] = '/';
   if(partIndex != OPC_PACKAGE_PART) {
      cchptr name      = package->parts[partIndex].name;
      cui64  nameBytes = OpcLength(name);

      if(nameBytes + 2u > sizeof(source)) return OPC_ERROR_REL_TARGET;
      for(ui64 index = 0; index < nameBytes; ++index) source[used++] = name[index];
   }
   source[used] = 0;

   cOPC_RESULT named = OpcRelsPartName(source, relsName, sizeof(relsName));

   if(named != OPC_OK) return named;

   csi32 relsPart = OpcFindPart(package, relsName);
   cui32 first    = package->relCount;

   // A part with no relationships part of its own has none, which is ordinary rather than a failure. It
   // is recorded so that the lookup is done once even when the answer is nothing.
   if(relsPart >= 0) {
      cOPC_RESULT loaded = OpcLoadXmlPart(package, relsPart);

      if(loaded != OPC_OK) return loaded;

      cOPC_RESULT parsed = OpcParseRels(package, source, package->parts[relsPart].bytes, package->parts[relsPart].byteCount);

      if(parsed != OPC_OK) {
         package->relCount   = first; // Nothing half-read is left behind for a caller to walk
         package->failedPart = relsPart;
         return parsed;
      }
   }
   if(partIndex == OPC_PACKAGE_PART) {
      package->packageRelsAt     = first;
      package->packageRelCount   = package->relCount - first;
      package->packageRelsLoaded = true;
   } else {
      package->parts[partIndex].relsAt     = first;
      package->parts[partIndex].relCount   = package->relCount - first;
      package->parts[partIndex].relsLoaded = true;
   }
   return OPC_OK;
}

//-- Main-part discovery

// Whether a content type is one a main document part may carry.
static cbool OpcIsMainContentType(cchptr contentType) {
   for(ui32 index = 0; index < OPC_MAIN_CONTENT_TYPE_COUNT; ++index) {
      if(OpcNameEqual(contentType, OPC_MAIN_CONTENT_TYPES[index])) return true;
   }
   return false;
}

// Finds the main document part. The officeDocument relationship is the specification's own discovery
// mechanism and is what decides; [Content_Types].xml is the cross-check, and only takes over when the
// relationship points at a part the archive does not contain.
static cOPC_RESULT OpcDiscoverMainPart(OPC_PACKAGEptrc package) {
   cOPC_RESULT loaded = OpcLoadRels(package, OPC_PACKAGE_PART);

   if(loaded != OPC_OK) return loaded;

   ui32 candidates = 0;
   si32 byType     = -1;
   si32 byPresence = -1;

   // Each candidate costs a scan of the part table, so the number of them is capped rather than trusted:
   // a package declaring sixty thousand officeDocument relationships over ten thousand parts would
   // otherwise spend minutes proving that none of them resolves.
   for(ui32 index = 0; index < package->packageRelCount; ++index) {
      cOPC_REL record = package->rels[package->packageRelsAt + index];

      if(record.kind != OPC_REL_OFFICE_DOCUMENT || record.external) continue;
      if(++candidates > OPC_MAX_MAIN_CANDIDATES) return OPC_ERROR_LIMIT;

      csi32 part = OpcFindPart(package, OpcHeapText(package, record.resolvedAt));

      if(part < 0) continue;
      if(byPresence < 0) byPresence = part;
      if(byType < 0 && OpcIsMainContentType(OpcContentTypeOf(package, part))) byType = part;
   }
   if(!candidates) return OPC_ERROR_NO_MAIN_REL;
   if(byType >= 0) {
      package->mainPart = byType;
      return OPC_OK;
   }
   // The relationship named a part that is there but is typed as something else. The relationship still
   // decides: a producer that omits the Override is common, and one that misroutes it is not.
   if(byPresence >= 0) {
      package->mainPart = byPresence;
      return OPC_OK;
   }
   // The relationship named nothing that exists. [Content_Types].xml is the only other statement about
   // which part is the body, so it is worth one look before the package is refused.
   ui32 examined = 0;

   for(ui32 row = 0; row < package->overrideCount; ++row) {
      if(!OpcIsMainContentType(OpcHeapText(package, package->overrides[row].typeAt))) continue;
      if(++examined > OPC_MAX_MAIN_CANDIDATES) break; // Bounded for the same reason as the walk above

      csi32 part = OpcFindPart(package, OpcHeapText(package, package->overrides[row].keyAt));

      if(part < 0) continue;
      package->mainPart = part;
      return OPC_OK;
   }
   return OPC_ERROR_MAIN_PART_MISSING;
}

//== Entry points

cOPC_RESULT OpcOpen(OPC_PACKAGEptrc package, ZIP_READERptrc reader) {
   mzero(package, sizeof(OPC_PACKAGE));
   package->mainPart   = -1;
   package->failedPart = -1;
   if(!reader) return OPC_ERROR_RANGE; // Zeroed first, so a caller may still close what it never opened
   package->reader   = reader;
   package->mainPart = -1;
   package->lastZip  = ZIP_OK;
   package->lastXml  = XML_OK;
   package->lastUtf  = UTF8_OK;

   cui32 count = (reader->entryCount ? reader->entryCount : 1u);

   package->parts = (OPC_PARTptr)amalloc(sizeof(OPC_PART) * ui64(count), 32u);
   if(!package->parts) return OPC_ERROR_MEMORY;
   mzero(package->parts, sizeof(OPC_PART) * ui64(count));
   package->partCount = reader->entryCount;
   for(ui32 index = 0; index < package->partCount; ++index) package->parts[index].name = reader->entries[index].name;

   // Offset 0 is reserved as the empty string, so a zero content-type or target offset is a usable
   // value rather than a sentinel every reader has to test for.
   ui32 empty = 0;

   if(!OpcHeapAdd(package, "", 0, &empty)) return OPC_ERROR_MEMORY;

   csi32 typesPart = OpcFindPart(package, OPC_PART_CONTENT_TYPES);

   if(typesPart < 0) return OPC_ERROR_NO_CONTENT_TYPES;
   if(OpcFindPart(package, OPC_PART_PACKAGE_RELS) < 0) return OPC_ERROR_NO_PACKAGE_RELS;

   cOPC_RESULT typesLoaded = OpcLoadXmlPart(package, typesPart);

   if(typesLoaded != OPC_OK) return typesLoaded;

   cOPC_RESULT typesParsed = OpcParseContentTypes(package, package->parts[typesPart].bytes, package->parts[typesPart].byteCount);

   if(typesParsed != OPC_OK) {
      package->failedPart = typesPart;
      return typesParsed;
   }
   return OpcDiscoverMainPart(package);
}

void OpcClose(OPC_PACKAGEptrc package) {
   if(package->parts) {
      for(ui32 index = 0; index < package->partCount; ++index) mdealloc(package->parts[index].bytes);
   }
   mdealloc(package->parts);
   mdealloc(package->heap);
   mdealloc(package->defaults);
   mdealloc(package->overrides);
   mdealloc(package->rels);
   mzero(package, sizeof(OPC_PACKAGE));
   package->mainPart   = -1;
   package->failedPart = -1;
}

csi32 OpcFindPart(cOPC_PACKAGEptr package, cchptr partName) {
   cchptr wanted = OpcEntryForm(partName);

   for(ui32 index = 0; index < package->partCount; ++index) {
      if(OpcNameEqual(package->parts[index].name, wanted)) return si32(index);
   }
   return -1;
}

cchptr OpcPartName(cOPC_PACKAGEptr package, csi32 partIndex) {
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return "";
   return package->parts[partIndex].name;
}

csi32 OpcMainPart(cOPC_PACKAGEptr package) { return package->mainPart; }

cui8ptr OpcPartBytes(cOPC_PACKAGEptr package, csi32 partIndex) {
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return nullptr;
   return package->parts[partIndex].bytes;
}

cui64 OpcPartByteCount(cOPC_PACKAGEptr package, csi32 partIndex) {
   if(partIndex < 0 || ui32(partIndex) >= package->partCount) return 0;
   return package->parts[partIndex].byteCount;
}

cui32 OpcRelCount(cOPC_PACKAGEptr package, csi32 partIndex) { return OpcRelsSize(package, partIndex); }

csi32 OpcRelAt(cOPC_PACKAGEptr package, csi32 partIndex, cui32 index) {
   if(index >= OpcRelsSize(package, partIndex)) return -1;
   return si32(OpcRelsBase(package, partIndex) + index);
}

csi32 OpcFindRelById(cOPC_PACKAGEptr package, csi32 partIndex, cchptr id) {
   cui32 base = OpcRelsBase(package, partIndex);
   cui32 size = OpcRelsSize(package, partIndex);

   for(ui32 index = 0; index < size; ++index) {
      if(OpcTextEqual(OpcHeapText(package, package->rels[base + index].idAt), id)) return si32(base + index);
   }
   return -1;
}

csi32 OpcFindRelByKind(cOPC_PACKAGEptr package, csi32 partIndex, cOPC_REL_KIND kind) {
   cui32 base = OpcRelsBase(package, partIndex);
   cui32 size = OpcRelsSize(package, partIndex);

   for(ui32 index = 0; index < size; ++index) {
      if(package->rels[base + index].kind == kind) return si32(base + index);
   }
   return -1;
}

cOPC_REL_VIEW OpcRel(cOPC_PACKAGEptr package, csi32 relIndex) {
   if(relIndex < 0 || ui32(relIndex) >= package->relCount) return {"", "", "", "", OPC_REL_OTHER, false};

   cOPC_REL record = package->rels[relIndex];

   return {OpcHeapText(package, record.idAt),
           OpcHeapText(package, record.typeAt),
           OpcHeapText(package, record.targetAt),
           OpcHeapText(package, record.resolvedAt),
           record.kind,
           record.external};
}

cchptr OpcMessageIn(OPC_PACKAGEptrc package, cchptr sentence, csi32 partIndex) {
   if(!package || partIndex < 0 || ui32(partIndex) >= package->partCount) return sentence;

   cchptr name = package->parts[partIndex].name;
   ui64   used = 0;

   while(sentence[used] && used < sizeof(package->message) - 8u) {
      package->message[used] = sentence[used];
      ++used;
   }
   for(cchptr walk = ", in "; *walk && used < sizeof(package->message) - 1u; ++walk) package->message[used++] = *walk;
   // An entry name is attacker-controlled bytes. A carriage return or a linefeed in one would overwrite
   // or forge a console line, so anything below a space is replaced rather than printed.
   for(ui64 index = 0; name[index] && used < sizeof(package->message) - 1u; ++index) {
      cchar byte = name[index];

      package->message[used++] = (ui8(byte) < 0x20u || ui8(byte) == 0x7Fu ? '?' : byte);
   }
   package->message[used] = 0;
   return package->message;
}

cchptr OpcResultText(OPC_PACKAGEptrc package, cOPC_RESULT result) {
   cchptr sentence = "not a valid DOCX; the package could not be read";

   if(result == OPC_ERROR_ZIP && package) sentence = ZipResultText(package->reader, package->lastZip);
   else if(result == OPC_ERROR_ZIP) sentence = "not a valid DOCX; the container could not be read";
   else if(result == OPC_ERROR_XML && package) sentence = XmlResultText(package->lastXml);
   else if(result == OPC_ERROR_NOT_UTF8 && package) sentence = UtfResultText(package->lastUtf);
   else if(result >= OPC_OK && result < OPC_RESULT_COUNT) sentence = OPC_RESULT_SENTENCE[result];
   return OpcMessageIn(package, sentence, package ? package->failedPart : -1);
}

cEXIT_CODE OpcExitCode(cOPC_PACKAGEptr package, cOPC_RESULT result) {
   if(result == OPC_OK) return EXIT_ALL_CONVERTED;
   if(result == OPC_ERROR_MEMORY || result == OPC_ERROR_RANGE) return EXIT_INTERNAL;
   if(result != OPC_ERROR_ZIP) return EXIT_NOT_DOCX;

   // ZipReader's documented rule, kept verbatim: an unopenable file is the environment's fault, a failed
   // allocation or a bad index is this program's, and everything else describes the document.
   cZIP_RESULT last = (package ? package->lastZip : ZIP_OK);

   if(last == ZIP_ERROR_OPEN) return EXIT_INPUT;
   if(last == ZIP_ERROR_MEMORY || last == ZIP_ERROR_RANGE) return EXIT_INTERNAL;
   return EXIT_NOT_DOCX;
}
