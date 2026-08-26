/*
 * File: StyleModel.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: Style part parsing, basedOn folding, role and font detection, and toggle-XOR resolution.
 * To Do: 1) Fold w:link so a character style can be reached from the paragraph style it pairs with.
 *        2) Match a font family ending in "mono" as monospace, once a corpus says the fixed table misses.
 *        3) Grow the walker's one-entry style cache if a document is found alternating between many.
 * Dependencies: BuildGuards.h, OpcPackage.h, StyleModel.h, XmlPull.h, typedefs.h, memory management.h,
 *               windows.h
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
#include "OpcPackage.h"
#include "XmlPull.h"
#include "StyleModel.h"

//-- Tables

// One row per toggle property of ISO/IEC 29500-1 17.7.3, in STYLE_TOGGLE order. The trailing comment on
// each row is load-bearing: clang-format will not join two lines a comment ends, so the table keeps the
// one-row-per-property shape the specification prints it in.
struct STYLE_TOGGLE_ROW {
   cchptr       name;   ///< The local name, in the WordprocessingML namespace
   STYLE_TOGGLE toggle; ///< Which toggle it is
};

static constexpr STYLE_TOGGLE_ROW STYLE_TOGGLE_NAMES[] = {
    {"b", STYLE_TOGGLE_BOLD},               // bold
    {"bCs", STYLE_TOGGLE_BOLD_CS},          // bold, complex script
    {"i", STYLE_TOGGLE_ITALIC},             // italic
    {"iCs", STYLE_TOGGLE_ITALIC_CS},        // italic, complex script
    {"caps", STYLE_TOGGLE_CAPS},            // all capitals
    {"smallCaps", STYLE_TOGGLE_SMALL_CAPS}, // small capitals
    {"strike", STYLE_TOGGLE_STRIKE},        // single strikethrough
    {"outline", STYLE_TOGGLE_OUTLINE},      // outlined glyphs
    {"shadow", STYLE_TOGGLE_SHADOW},        // shadowed glyphs
    {"emboss", STYLE_TOGGLE_EMBOSS},        // embossed glyphs
    {"imprint", STYLE_TOGGLE_IMPRINT},      // engraved glyphs
    {"vanish", STYLE_TOGGLE_VANISH}         // hidden text
};

static_assert(sizeof(STYLE_TOGGLE_NAMES) / sizeof(STYLE_TOGGLE_NAMES[0]) == ui64(STYLE_TOGGLE_COUNT),
              "StyleModel: the toggle name table and the STYLE_TOGGLE enumeration have drifted apart.");

// The normalized names that make a paragraph style a blockquote (CONVERSION_REFERENCE row 13) and the
// ones that make it a fenced code block (row 12). Row 13's w:ind >= 720 twips heuristic is deliberately
// not implemented: the reference itself has it off by default, because an indent is ambiguous.
static constexpr cchptr STYLE_QUOTE_NAMES[] = {
    "quote",         // Word's built-in Quote
    "intense quote", // Word's built-in Intense Quote
    "block text",    // Word's built-in Block Text
    "quotations"     // LibreOffice's own name for the same thing
};

static constexpr cchptr STYLE_CODE_BLOCK_NAMES[] = {
    "html preformatted", // Word's built-in, and what a paste from a browser lands in
    "preformatted text", // LibreOffice
    "source code",       // Pandoc
    "code"               // The bare name, which a hand-made template usually spells
};

// The normalized names that make a *character* style an inline code span (row 11 (a)). "code" is in both
// tables on purpose: the same word names a block when a paragraph style carries it and a span when a
// character style does, which is why StyleRoleOfName is told the type.
static constexpr cchptr STYLE_CODE_SPAN_NAMES[] = {
    "code",          // The bare name
    "html code",     // Word's built-in HTML Code
    "verbatim char", // Pandoc
    "source text",   // LibreOffice
    "macro text"     // LibreOffice's other monospace character style
};

// The monospace families of CONVERSION_REFERENCE row 11, normalized. A run whose effective w:rFonts
// w:ascii names one of these is code even with no style saying so, which is how a hand-formatted
// snippet in an ordinary paragraph is recognised.
static constexpr cchptr STYLE_MONOSPACE_FONTS[] = {
    "consolas",         // Microsoft
    "courier",          // The original
    "courier new",      // Word's default monospace
    "cascadia code",    // Microsoft, ligatures
    "cascadia mono",    // Microsoft, no ligatures
    "lucida console",   // Microsoft
    "menlo",            // Apple
    "monaco",           // Apple
    "dejavu sans mono", // Free desktops
    "liberation mono",  // Red Hat's metric-compatible Courier New
    "fira code",        // Mozilla, ligatures
    "fira mono",        // Mozilla, no ligatures
    "jetbrains mono",   // JetBrains
    "source code pro",  // Adobe
    "roboto mono",      // Google
    "ubuntu mono",      // Canonical
    "ibm plex mono"     // IBM
};

// One sentence per STYLE_RESULT, in enumeration order.
static constexpr cchptr STYLE_RESULT_TEXT[] = {
    "the style part was read",                                     // STYLE_OK
    "not enough memory to hold the document's styles",             // STYLE_ERROR_MEMORY
    "the style part could not be read",                            // STYLE_ERROR_PART
    "the style part is not well-formed XML",                       // STYLE_ERROR_XML
    "the style part's root element is not w:styles",               // STYLE_ERROR_ROOT
    "the style part declares more styles than this reader accepts" // STYLE_ERROR_LIMIT
};

static_assert(sizeof(STYLE_RESULT_TEXT) / sizeof(STYLE_RESULT_TEXT[0]) == ui64(STYLE_RESULT_COUNT),
              "StyleModel: the result sentence table and the STYLE_RESULT enumeration have drifted apart.");

//-- Small helpers

// Bytes before the terminator.
static cui64 StyleLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Exact comparison of two NUL-terminated strings.
static cbool StyleTextEqual(cchptr a, cchptr b) {
   ui64 index = 0;

   while(a[index] && a[index] == b[index]) ++index;
   return a[index] == b[index];
}

// FNV-1a over a NUL-terminated identifier. Any well-mixed hash will do here: it is only ever used to
// pick a bucket, and every candidate is confirmed with a full byte comparison before it is returned.
static cui32 StyleTextHash(cchptr text) {
   ui32 hash = 2166136261u;

   for(ui64 index = 0; text[index]; ++index) {
      hash = ui32((hash ^ ui32(ui8(text[index]))) * 16777619u);
   }
   return hash;
}

// Whether a byte is one of the four XML calls whitespace.
static cbool StyleIsSpace(cchar byte) { return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n'; }

//-- Growable storage

// Grows a block to hold at least the requested number of elements, doubling so that filling one costs
// amortised constant time. Offsets survive a move, which is why every reference into the heap is one.
static cbool StyleReserve(ptrptrc block, ui64ptrc capacity, cui64 needed, cui64 unit) {
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

// Copies a string into the model's heap and reports where it landed. Offset 0 is always the empty string,
// so a zero offset is a usable value rather than a sentinel a caller has to test for.
static cbool StyleHeapAdd(STYLE_MODELptrc model, cchptr text, cui64 length, ui32ptrc at) {
   if(!StyleReserve((ptrptrc)&model->heap, &model->heapCapacity, model->heapUsed + length + 1u, 1u)) return false;
   *at = ui32(model->heapUsed);
   for(ui64 index = 0; index < length; ++index) model->heap[model->heapUsed + index] = text[index];
   model->heap[model->heapUsed + length] = 0;
   model->heapUsed += length + 1u;
   return true;
}

// The string one heap offset names.
static cchptr StyleHeapText(cSTYLE_MODELptr model, cui32 at) { return (model->heap ? model->heap + at : ""); }

//== Names and roles

cui64 StyleNormalizeName(cchptr text, cui64 byteCount, chptrc dest, cui64 destBytes) {
   ui64 used    = 0;
   ui64 index   = 0;
   bool pending = false; // A run of whitespace waiting to become one space, once something follows it

   if(!destBytes) return 0;
   while(index < byteCount && used + 1u < destBytes) {
      cchar byte = text[index];

      // LibreOffice escapes a space in an identifier as _20_, so Source_20_Text is "source text".
      if(byte == '_' && index + 3u < byteCount && text[index + 1u] == '2' && text[index + 2u] == '0' && text[index + 3u] == '_') {
         index += 4u;
         if(used) pending = true;
         continue;
      }
      if(StyleIsSpace(byte)) {
         index += 1u;
         if(used) pending = true;
         continue;
      }
      if(pending) {
         dest[used++] = ' ';
         pending      = false;
         if(used + 1u >= destBytes) break;
      }
      dest[used++] = (byte >= 'A' && byte <= 'Z' ? char(byte - 'A' + 'a') : byte);
      index += 1u;
   }
   dest[used] = 0;
   return used;
}

// Whether a normalized name appears in one of the tables above.
static cbool StyleNameInTable(cchptr normalized, cchptrcptr table, cui64 rows) {
   for(ui64 index = 0; index < rows; ++index) {
      if(StyleTextEqual(normalized, table[index])) return true;
   }
   return false;
}

cbool StyleFontIsMonospace(cchptr normalized) {
   cui64 rows = sizeof(STYLE_MONOSPACE_FONTS) / sizeof(STYLE_MONOSPACE_FONTS[0]);

   return StyleNameInTable(normalized, STYLE_MONOSPACE_FONTS, rows);
}

cSTYLE_ROLE StyleRoleOfName(cchptr normalized, cSTYLE_TYPE type, ui8ptrc level) {
   *level = 0;
   // A character style names one role and no other: the inline code of row 11. Letting it name a heading
   // would make Word's own "Heading 1 Char" linked style into a heading wherever a run referenced it.
   if(type == STYLE_TYPE_CHARACTER) {
      cui64 rows = sizeof(STYLE_CODE_SPAN_NAMES) / sizeof(STYLE_CODE_SPAN_NAMES[0]);

      return (StyleNameInTable(normalized, STYLE_CODE_SPAN_NAMES, rows) ? STYLE_ROLE_CODE : STYLE_ROLE_NORMAL);
   }
   if(StyleTextEqual(normalized, "title")) return STYLE_ROLE_TITLE;
   if(StyleTextEqual(normalized, "subtitle")) return STYLE_ROLE_SUBTITLE;

   cui64 quotes = sizeof(STYLE_QUOTE_NAMES) / sizeof(STYLE_QUOTE_NAMES[0]);
   cui64 blocks = sizeof(STYLE_CODE_BLOCK_NAMES) / sizeof(STYLE_CODE_BLOCK_NAMES[0]);

   if(StyleNameInTable(normalized, STYLE_QUOTE_NAMES, quotes)) return STYLE_ROLE_QUOTE;
   if(StyleNameInTable(normalized, STYLE_CODE_BLOCK_NAMES, blocks)) return STYLE_ROLE_CODE;

   cui64  length  = StyleLength(normalized);
   cchptr heading = "heading";

   // Both spellings reach here: "heading 1" as w:name writes it, and "heading1" as a styleId does.
   if(length != 8u && length != 9u) return STYLE_ROLE_NORMAL;
   for(ui64 index = 0; index < 7u; ++index) {
      if(normalized[index] != heading[index]) return STYLE_ROLE_NORMAL;
   }
   if(length == 9u && normalized[7] != ' ') return STYLE_ROLE_NORMAL;

   cchar digit = normalized[length - 1u];

   if(digit < '1' || digit > '9') return STYLE_ROLE_NORMAL;
   *level = ui8(digit - '0');
   return STYLE_ROLE_HEADING;
}

cbool StyleOnOff(cXML_TEXT value) {
   if(!value.bytes) return true; // An absent w:val means true, per ISO/IEC 29500-1 17.17.4
   if(XmlTextEqual(value, "1") || XmlTextEqual(value, "true") || XmlTextEqual(value, "on")) return true;
   // Anything the specification does not name reads as false. That is a defensive fallback and not a
   // rule: a file only ever writes one of the three spellings above or one of their negatives.
   return false;
}

void StyleClearDirect(STYLE_DIRECT_RUNptrc direct) {
   direct->toggleTrue      = 0;
   direct->toggleSpecified = 0;
   direct->characterStyle  = -1;
   direct->doubleStrike    = -1;
   direct->webHidden       = -1;
   direct->monospace       = -1;
   direct->vertAlign       = STYLE_VERT_UNSET;
}

//-- Property layers

// What one w:rPr and one w:pPr together contribute, before they are written into a style or into
// docDefaults. The run half is a STYLE_DIRECT_RUN because a style's w:rPr and a run's own w:rPr hold
// exactly the same vocabulary, and reading them twice is how the two would drift apart.
struct STYLE_LAYER {
   STYLE_DIRECT_RUN run;        ///< What a w:rPr contributed
   si32             outlineLvl; ///< What a w:pPr contributed, or -1
};

typedef STYLE_LAYER *const STYLE_LAYERptrc;

// Clears a layer to "nothing specified".
static void StyleClearLayer(STYLE_LAYERptrc layer) {
   StyleClearDirect(&layer->run);
   layer->outlineLvl = -1;
}

void StyleReadDirectProperty(XML_READERptrc reader, STYLE_DIRECT_RUNptrc direct) {
   for(ui32 index = 0; index < ui32(STYLE_TOGGLE_COUNT); ++index) {
      if(!XmlIsElement(reader, XML_NS_W, STYLE_TOGGLE_NAMES[index].name)) continue;

      cui16 bit = StyleToggleBit(STYLE_TOGGLE_NAMES[index].toggle);

      direct->toggleSpecified = ui16(direct->toggleSpecified | bit);
      if(StyleOnOff(XmlAttribute(reader, XML_NS_W, "val"))) direct->toggleTrue = ui16(direct->toggleTrue | bit);
      else direct->toggleTrue = ui16(direct->toggleTrue & ~bit);
      return;
   }
   if(XmlIsElement(reader, XML_NS_W, "dstrike")) {
      direct->doubleStrike = si8(StyleOnOff(XmlAttribute(reader, XML_NS_W, "val")) ? 1 : 0);
      return;
   }
   if(XmlIsElement(reader, XML_NS_W, "webHidden")) {
      direct->webHidden = si8(StyleOnOff(XmlAttribute(reader, XML_NS_W, "val")) ? 1 : 0);
      return;
   }
   if(XmlIsElement(reader, XML_NS_W, "rFonts")) {
      cXML_TEXT ascii = XmlAttribute(reader, XML_NS_W, "ascii");

      // A w:rFonts with no w:ascii says nothing this reader can use -- w:asciiTheme names a theme slot
      // and theme1.xml is not loaded -- so it stays unspecified. A specified false would be worse than
      // silence: it would cancel a monospace font an outer layer had already established.
      if(ascii.bytes) {
         char font[STYLE_MAX_NAME_BYTES];

         StyleNormalizeName(ascii.bytes, ascii.length, font, sizeof(font));
         direct->monospace = si8(StyleFontIsMonospace(font) ? 1 : 0);
      }
      return;
   }
   if(XmlIsElement(reader, XML_NS_W, "vertAlign")) {
      cXML_TEXT value = XmlAttribute(reader, XML_NS_W, "val");

      if(XmlTextEqual(value, "superscript")) direct->vertAlign = STYLE_VERT_SUPERSCRIPT;
      else if(XmlTextEqual(value, "subscript")) direct->vertAlign = STYLE_VERT_SUBSCRIPT;
      else direct->vertAlign = STYLE_VERT_BASELINE;
   }
}

// Reads one element of a w:pPr into a layer. A w:rPr inside a w:pPr is the paragraph *mark's* formatting
// and never reaches text (CONVERSION_REFERENCE 2.1), so it is skipped rather than folded in.
static void StyleReadParagraphProperty(XML_READERptrc reader, STYLE_LAYERptrc layer) {
   if(!XmlIsElement(reader, XML_NS_W, "outlineLvl")) return;

   cXML_TEXT value = XmlAttribute(reader, XML_NS_W, "val");

   if(!value.bytes || !value.length) return;

   si32 parsed = 0;

   for(ui64 index = 0; index < value.length; ++index) {
      if(value.bytes[index] < '0' || value.bytes[index] > '9') return;
      parsed = parsed * 10 + si32(value.bytes[index] - '0');
      if(parsed > 9) return;
   }
   layer->outlineLvl = parsed;
}

// Walks the children of the w:rPr the reader is on, reading each and skipping it. Returns false only when
// the part stopped being readable.
static cbool StyleReadRunBag(XML_READERptrc reader, STYLE_LAYERptrc layer) {
   cui32 containerDepth = reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == containerDepth) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      StyleReadDirectProperty(reader, &layer->run);
      if(!XmlSkipElement(reader)) return false;
   }
}

// Walks the children of the w:pPr the reader is on.
static cbool StyleReadParagraphBag(XML_READERptrc reader, STYLE_LAYERptrc layer) {
   cui32 containerDepth = reader->depth;

   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == containerDepth) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      StyleReadParagraphProperty(reader, layer);
      if(!XmlSkipElement(reader)) return false;
   }
}

//-- Part parsing

// Reads one w:style element into the model. A false return means either the part stopped being readable
// or the model could not grow; limit says which of the two the caller should report.
static cbool StyleReadStyle(STYLE_MODELptrc model, XML_READERptrc reader, boolptrc limit) {
   if(model->styleCount >= STYLE_MAX_STYLES) {
      *limit = true;
      return false;
   }

   char        name[STYLE_MAX_NAME_BYTES];
   char        identifier[STYLE_MAX_NAME_BYTES];
   STYLE_LAYER runs;
   STYLE_LAYER marks;
   cXML_TEXT   idValue   = XmlAttribute(reader, XML_NS_W, "styleId");
   cXML_TEXT   typeValue = XmlAttribute(reader, XML_NS_W, "type");
   cXML_TEXT   defValue  = XmlAttribute(reader, XML_NS_W, "default");
   cbool       marked    = (defValue.bytes && StyleOnOff(defValue));
   STYLE_TYPE  type      = STYLE_TYPE_PARAGRAPH;
   ui32        idAt      = 0;
   ui32        basedOnAt = 0;
   ui32        nameAt    = 0;
   bool        sawName   = false;
   cui32       depthHere = reader->depth;

   name[0] = 0;
   StyleClearLayer(&runs);
   StyleClearLayer(&marks);
   if(XmlTextEqual(typeValue, "character")) type = STYLE_TYPE_CHARACTER;
   else if(XmlTextEqual(typeValue, "table")) type = STYLE_TYPE_TABLE;
   else if(XmlTextEqual(typeValue, "numbering")) type = STYLE_TYPE_NUMBERING;

   // Every attribute is read and copied before the walk begins: a view the reader hands out dies on the
   // next XmlNext call, and the walk below makes hundreds of them.
   // Capped at what the walker's lookup key can hold. ST_String stops at 255 bytes, so a longer id is
   // already out of spec; storing it in full would make it one the document can never match, because
   // DocFindStyle truncates a w:pStyle value to the same ceiling and the two would differ at byte 255.
   cui64 idLength = (idValue.length < STYLE_MAX_NAME_BYTES ? idValue.length : STYLE_MAX_NAME_BYTES - 1u);

   if(idValue.bytes && !StyleHeapAdd(model, idValue.bytes, idLength, &idAt)) return false;
   StyleNormalizeName(idValue.bytes ? idValue.bytes : "", idValue.bytes ? idValue.length : 0u, identifier, sizeof(identifier));
   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(reader, XML_NS_W, "rPr")) {
         if(!StyleReadRunBag(reader, &runs)) return false;
         continue;
      }
      if(XmlIsElement(reader, XML_NS_W, "pPr")) {
         if(!StyleReadParagraphBag(reader, &marks)) return false;
         continue;
      }
      if(XmlIsElement(reader, XML_NS_W, "name")) {
         cXML_TEXT value = XmlAttribute(reader, XML_NS_W, "val");

         if(value.bytes) {
            StyleNormalizeName(value.bytes, value.length, name, sizeof(name));
            sawName = true;
         }
      } else if(XmlIsElement(reader, XML_NS_W, "basedOn")) {
         cXML_TEXT value = XmlAttribute(reader, XML_NS_W, "val");
         // Capped like the identifier above, and for the same reason: StyleLinkChains resolves this
         // through StyleFind, so a value longer than a stored id could never match one.
         cui64 kept = (value.length < STYLE_MAX_NAME_BYTES ? value.length : STYLE_MAX_NAME_BYTES - 1u);

         if(value.bytes && !StyleHeapAdd(model, value.bytes, kept, &basedOnAt)) return false;
      }
      if(!XmlSkipElement(reader)) return false;
   }

   cchptr key   = (sawName ? name : identifier);
   ui8    level = 0;

   // The name is the portable key -- a localized Word writes an English w:name over a localized styleId --
   // but a producer that omits w:name leaves only the identifier, so both are tried.
   STYLE_ROLE role = StyleRoleOfName(key, type, &level);

   if(role == STYLE_ROLE_NORMAL) role = StyleRoleOfName(identifier, type, &level);
   if(!StyleHeapAdd(model, key, StyleLength(key), &nameAt)) return false;
   if(!StyleReserve((ptrptrc)&model->styles, &model->styleCapacity, ui64(model->styleCount) + 1u, sizeof(STYLE_RECORD))) return false;

   STYLE_RECORDptr record = model->styles + model->styleCount;

   record->idAt         = idAt;
   record->nameAt       = nameAt;
   record->basedOnAt    = basedOnAt;
   record->basedOn      = -1;
   record->toggleTrue   = runs.run.toggleTrue;
   record->outlineLvl   = marks.outlineLvl;
   record->role         = role;
   record->headingLevel = level;
   record->doubleStrike = runs.run.doubleStrike;
   record->webHidden    = runs.run.webHidden;
   record->monospace    = runs.run.monospace;
   record->type         = type;
   record->typeSaid     = (typeValue.bytes != nullptr);
   record->vertAlign    = runs.run.vertAlign;
   record->isDefault    = marked;
   ++model->styleCount;
   if(marked && type == STYLE_TYPE_PARAGRAPH && model->defaultParagraph < 0) model->defaultParagraph = si32(model->styleCount) - 1;
   return true;
}

// Reads w:docDefaults into the model. Both wrappers are optional, and so is the property bag inside each.
static cbool StyleReadDefaults(STYLE_MODELptrc model, XML_READERptrc reader) {
   cui32       depthHere = reader->depth;
   STYLE_LAYER runs;
   STYLE_LAYER marks;

   StyleClearLayer(&runs);
   StyleClearLayer(&marks);
   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;

      cbool runDefault = XmlIsElement(reader, XML_NS_W, "rPrDefault");
      cbool parDefault = XmlIsElement(reader, XML_NS_W, "pPrDefault");

      if(!runDefault && !parDefault) {
         if(!XmlSkipElement(reader)) return false;
         continue;
      }

      cui32 wrapperDepth = reader->depth;

      for(;;) {
         cXML_TOKEN inner = XmlNext(reader);

         if(inner == XML_TOKEN_ERROR || inner == XML_TOKEN_END_OF_INPUT) return false;
         if(inner == XML_TOKEN_END_ELEMENT && reader->depth == wrapperDepth) break;
         if(inner != XML_TOKEN_START_ELEMENT) continue;
         if(runDefault && XmlIsElement(reader, XML_NS_W, "rPr")) {
            if(!StyleReadRunBag(reader, &runs)) return false;
            continue;
         }
         if(parDefault && XmlIsElement(reader, XML_NS_W, "pPr")) {
            if(!StyleReadParagraphBag(reader, &marks)) return false;
            continue;
         }
         if(!XmlSkipElement(reader)) return false;
      }
   }
   model->defaults.toggleTrue   = runs.run.toggleTrue;
   model->defaults.outlineLvl   = marks.outlineLvl;
   model->defaults.doubleStrike = runs.run.doubleStrike;
   model->defaults.webHidden    = runs.run.webHidden;
   model->defaults.monospace    = runs.run.monospace;
   model->defaults.vertAlign    = runs.run.vertAlign;
   return true;
}

//-- Identifier index

// Builds the styleId index, once, after every style has been read and before anything looks one up.
// Open addressing with linear probing over a power-of-two table at least twice the style count, so the
// load factor stays under a half and a probe is short. A failure here is not fatal: StyleFind falls
// back to its linear scan, which is slower but returns the same answer.
static void StyleBuildIndex(STYLE_MODELptrc model) {
   ui64 count = 8u;

   while(count < ui64(model->styleCount) * 2u) count *= 2u;

   si32ptr buckets = (si32ptr)amalloc(count * sizeof(si32), 32u);

   if(!buckets) return;
   for(ui64 slot = 0; slot < count; ++slot) buckets[slot] = -1;

   cui32 mask = ui32(count - 1u);

   for(ui32 index = 0; index < model->styleCount; ++index) {
      cchptr identifier = StyleHeapText(model, model->styles[index].idAt);

      if(!identifier[0]) continue; // An empty id can never be looked up, so it never takes a slot

      ui32 slot = StyleTextHash(identifier) & mask;
      bool seen = false;

      while(buckets[slot] >= 0) {
         // Duplicate identifiers resolve to the first record, which is what the linear scan did and
         // what the header documents, so a later one is dropped rather than overwriting the earlier.
         if(StyleTextEqual(StyleHeapText(model, model->styles[buckets[slot]].idAt), identifier)) {
            seen = true;
            break;
         }
         slot = (slot + 1u) & mask;
      }
      if(!seen) buckets[slot] = si32(index);
   }
   model->buckets    = buckets;
   model->bucketMask = mask;
}

//-- Chain folding

// Resolves every w:basedOn identifier to an index, now that every style has been read.
//
// A link across two style *types* is dropped, which ISO/IEC 29500-1 17.7.4.3 requires: a character
// style's parent shall be a character style, and an implementation ignores the reference when it is
// not. It matters here beyond conformance, because a cross-type link is a hole straight through the
// monospace guard StyleReadBaseline sets: a character style based on a monospace default *paragraph*
// style inherits its family, and StyleResolveRun takes the character layer before the guard is read --
// so italic prose converts to a fenced code block. A role leaks the same way in the other direction.
//
// The test needs both styles to have *said* what they are. w:type is optional and this reader defaults
// an absent one to paragraph, so comparing the stored types alone would drop a typeless style's link to
// a real character style -- which is a shape producers write, and dropping it silently loses the code
// span it carries.
static void StyleLinkChains(STYLE_MODELptrc model) {
   for(ui32 index = 0; index < model->styleCount; ++index) {
      cchptr parent = StyleHeapText(model, model->styles[index].basedOnAt);

      model->styles[index].basedOn = (parent[0] ? StyleFind(model, parent) : -1);
      // A style based on itself is a one-element cycle, and files carrying one exist.
      if(model->styles[index].basedOn == si32(index)) model->styles[index].basedOn = -1;

      csi32 found = model->styles[index].basedOn;

      if(found < 0) continue;

      cSTYLE_RECORDptr child    = model->styles + index;
      cSTYLE_RECORDptr parentOf = model->styles + found;

      if(child->typeSaid && parentOf->typeSaid && child->type != parentOf->type) model->styles[index].basedOn = -1;
   }
}

// Folds one style's whole basedOn chain into its resolved record. The chain is collected leaf-first and
// applied root-first, which is what leaves the nearest specification standing; the toggle parity is
// order-independent, so the same pass computes it.
static void StyleFoldChain(STYLE_MODELptrc model, cui32 index) {
   si32 chain[STYLE_MAX_CHAIN];
   ui32 length = 0;
   si32 walk   = si32(index);

   while(walk >= 0 && length < STYLE_MAX_CHAIN) {
      bool seen = false;

      for(ui32 back = 0; back < length; ++back) {
         if(chain[back] == walk) seen = true;
      }
      if(seen) break; // A cycle: stop where it closes rather than following it round again
      chain[length++] = walk;
      walk            = model->styles[walk].basedOn;
   }

   STYLE_RESOLVEDptr resolved = model->resolved + index;

   resolved->toggleParity = 0;
   resolved->outlineLvl   = -1;
   resolved->role         = STYLE_ROLE_NORMAL;
   resolved->headingLevel = 0;
   resolved->doubleStrike = -1;
   resolved->webHidden    = -1;
   resolved->monospace    = -1;
   resolved->vertAlign    = STYLE_VERT_UNSET;
   for(ui32 step = length; step > 0; --step) {
      cSTYLE_RECORDptr record = model->styles + chain[step - 1u];

      resolved->toggleParity = ui16(resolved->toggleParity ^ record->toggleTrue);
      if(record->outlineLvl >= 0) resolved->outlineLvl = record->outlineLvl;
      if(record->doubleStrike >= 0) resolved->doubleStrike = record->doubleStrike;
      if(record->webHidden >= 0) resolved->webHidden = record->webHidden;
      if(record->monospace >= 0) resolved->monospace = record->monospace;
      if(record->vertAlign != STYLE_VERT_UNSET) resolved->vertAlign = record->vertAlign;
      if(record->role != STYLE_ROLE_NORMAL) {
         resolved->role         = record->role;
         resolved->headingLevel = record->headingLevel;
      }
   }
}

//-- The document's font baseline

// Settles whether row 12's monospace heuristic can say anything about this document at all.
//
// The heuristic asks whether every text-bearing run of a paragraph is monospace, which is a useful
// signal only where it tells one run from its neighbours. A document that sets a monospace family as its
// *baseline* makes it true of every run, so every paragraph becomes a fenced code block and the whole
// file converts to one fence with every delimiter dead inside it. Legal filings set Courier that way.
//
// A document declares that baseline in two places and the nearer one wins, exactly as w:dstrike layers:
// the w:default="1" paragraph style, folded down its own basedOn chain, and w:docDefaults behind it.
// Both halves matter and the first is the likelier: Word's w:docDefaults normally names a *theme* slot,
// which specifies no family at all, and Modify Style on Normal is what actually carries the font. A
// default paragraph style naming a proportional family therefore switches the heuristic back *on* even
// under a monospace w:docDefaults, which is the right answer -- an unstyled paragraph is proportional
// there, so a monospace run really does stand out.
static void StyleReadBaseline(STYLE_MODELptrc model) {
   si8 baseline = -1;

   if(model->defaultParagraph >= 0) baseline = model->resolved[model->defaultParagraph].monospace;
   if(baseline < 0) baseline = model->defaults.monospace;
   model->monoDefault = (baseline > 0);
}

//== Entry points

void StyleOpen(STYLE_MODELptrc model) {
   mzero(model, sizeof(STYLE_MODEL));
   model->defaults.outlineLvl   = -1;
   model->defaults.doubleStrike = -1;
   model->defaults.webHidden    = -1;
   model->defaults.monospace    = -1;
   model->defaults.vertAlign    = STYLE_VERT_UNSET;
   model->defaultParagraph      = -1;
   model->lastXml               = XML_OK;
   model->lastOpc               = OPC_OK;
}

cSTYLE_RESULT StyleLoad(STYLE_MODELptrc model, OPC_PACKAGEptrc package, csi32 partIndex) {
   if(partIndex < 0) return STYLE_OK; // No styles part: every property takes its specification default

   cOPC_RESULT loaded = OpcLoadXmlPart(package, partIndex);

   if(loaded != OPC_OK) {
      model->lastOpc = loaded;
      return (loaded == OPC_ERROR_MEMORY ? STYLE_ERROR_MEMORY : STYLE_ERROR_PART);
   }
   return StyleLoadBytes(model, OpcPartBytes(package, partIndex), OpcPartByteCount(package, partIndex));
}

cSTYLE_RESULT StyleLoadBytes(STYLE_MODELptrc model, cui8ptr bytes, cui64 byteCount) {
   XML_READER reader;
   ui32       empty = 0;

   // Offset 0 is reserved as the empty string, so a style that declares no w:basedOn has a basedOnAt a
   // lookup can read as "nothing" rather than as whichever identifier happened to be stored first.
   if(!model->heap && !StyleHeapAdd(model, "", 0, &empty)) return STYLE_ERROR_MEMORY;
   model->lastXml = XmlOpen(&reader, bytes, byteCount);
   if(model->lastXml != XML_OK) {
      XmlClose(&reader);
      return STYLE_ERROR_XML;
   }

   STYLE_RESULT verdict = STYLE_OK;
   bool         limit   = false;
   bool         opened  = false;

   for(;;) {
      cXML_TOKEN token = XmlNext(&reader);

      if(token == XML_TOKEN_END_OF_INPUT || token == XML_TOKEN_ERROR) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!opened) {
         // The root has to be w:styles. A part typed as styles that holds something else is a defective
         // package, not an optional part that happens to be absent, so it is refused rather than ignored.
         if(!XmlIsElement(&reader, XML_NS_W, "styles")) {
            verdict = STYLE_ERROR_ROOT;
            break;
         }
         opened = true;
         continue;
      }
      if(reader.depth == 2u && XmlIsElement(&reader, XML_NS_W, "docDefaults")) {
         if(!StyleReadDefaults(model, &reader)) break;
         continue;
      }
      if(reader.depth == 2u && XmlIsElement(&reader, XML_NS_W, "style")) {
         if(!StyleReadStyle(model, &reader, &limit)) {
            verdict = (limit ? STYLE_ERROR_LIMIT : STYLE_ERROR_MEMORY);
            break;
         }
         continue;
      }
      if(!XmlSkipElement(&reader)) break;
   }

   cXML_RESULT broke = reader.result;

   XmlClose(&reader);
   // A tokenizer failure outranks the reason the walk stopped: it names the rule the part broke, which
   // is what a reader of the message can act on.
   if(broke != XML_OK) {
      model->lastXml = broke;
      return STYLE_ERROR_XML;
   }
   if(verdict != STYLE_OK) return verdict;
   if(!opened) return STYLE_ERROR_ROOT; // No root element at all, so the part names no styles

   cui64 wanted = ui64(model->styleCount ? model->styleCount : 1u);

   model->resolved = (STYLE_RESOLVEDptr)amalloc(sizeof(STYLE_RESOLVED) * wanted, 32u);
   if(!model->resolved) return STYLE_ERROR_MEMORY;
   // The index goes in before the chains, because StyleLinkChains is itself a lookup per style and
   // would otherwise be the load-time half of the same quadratic.
   StyleBuildIndex(model);
   StyleLinkChains(model);
   for(ui32 index = 0; index < model->styleCount; ++index) StyleFoldChain(model, index);
   StyleReadBaseline(model);
   model->hasPart = true;
   return STYLE_OK;
}

void StyleClose(STYLE_MODELptrc model) {
   mdealloc(model->buckets);
   mdealloc(model->styles);
   mdealloc(model->resolved);
   mdealloc(model->heap);
   StyleOpen(model);
}

csi32 StyleFind(cSTYLE_MODELptr model, cchptr styleId) {
   if(!styleId || !styleId[0]) return -1;
   // Without the index this is a linear scan of full byte comparisons, and the walker calls it once
   // per styled paragraph: a document with many paragraphs and a styles.xml with many long-prefixed
   // identifiers then costs paragraphs x styles x identifier length, which a 45 KB .docx can drive
   // into minutes of silence. The scan survives only for a model whose index could not be allocated.
   if(model->buckets) {
      cui32 mask = model->bucketMask;
      ui32  slot = StyleTextHash(styleId) & mask;

      for(ui32 step = 0; step <= mask; ++step) {
         csi32 found = model->buckets[slot];

         if(found < 0) return -1; // An empty slot ends the probe: nothing past it can be this id
         if(StyleTextEqual(StyleHeapText(model, model->styles[found].idAt), styleId)) return found;
         slot = (slot + 1u) & mask;
      }
      return -1;
   }
   for(ui32 index = 0; index < model->styleCount; ++index) {
      if(StyleTextEqual(StyleHeapText(model, model->styles[index].idAt), styleId)) return si32(index);
   }
   return -1;
}

csi32 StyleDefaultParagraph(cSTYLE_MODELptr model) { return model->defaultParagraph; }

cui32 StyleCount(cSTYLE_MODELptr model) { return model->styleCount; }

cchptr StyleName(cSTYLE_MODELptr model, csi32 styleIndex) {
   if(styleIndex < 0 || ui32(styleIndex) >= model->styleCount) return "";
   return StyleHeapText(model, model->styles[styleIndex].nameAt);
}

cSTYLE_PARAGRAPH_PROPS StyleResolveParagraph(cSTYLE_MODELptr model, csi32 styleIndex, csi32 directOutline) {
   STYLE_PARAGRAPH_PROPS props = {STYLE_ROLE_NORMAL, 0};

   cbool              known    = (styleIndex >= 0 && ui32(styleIndex) < model->styleCount && model->resolved);
   cSTYLE_RESOLVEDptr resolved = (known ? model->resolved + styleIndex : nullptr);

   if(resolved && resolved->role == STYLE_ROLE_HEADING) {
      props.role         = STYLE_ROLE_HEADING;
      props.headingLevel = ui8(resolved->headingLevel > 6u ? 6u : resolved->headingLevel);
      return props;
   }
   // Title and Subtitle carry no level of their own; the mapping policy gives them the first two.
   if(resolved && resolved->role == STYLE_ROLE_TITLE) {
      props.role         = STYLE_ROLE_TITLE;
      props.headingLevel = 1u;
      return props;
   }
   if(resolved && resolved->role == STYLE_ROLE_SUBTITLE) {
      props.role         = STYLE_ROLE_SUBTITLE;
      props.headingLevel = 2u;
      return props;
   }
   // A quote or a code style returns here rather than falling through, so that a w:outlineLvl on the
   // paragraph cannot turn a quotation into a heading. The name deciding before the outline level is
   // CONVERSION_REFERENCE 2.8, and these two roles are named the same way a heading is.
   if(resolved && (resolved->role == STYLE_ROLE_QUOTE || resolved->role == STYLE_ROLE_CODE)) {
      props.role = resolved->role;
      return props;
   }

   si32 outline = directOutline;

   if(outline < 0 && resolved) outline = resolved->outlineLvl;
   if(outline < 0) outline = model->defaults.outlineLvl;
   if(outline < 0 || outline > 8) return props; // 9 is body text, and so is an absent level

   cui8 level = ui8(outline + 1);

   props.role         = STYLE_ROLE_HEADING;
   props.headingLevel = ui8(level > 6u ? 6u : level);
   return props;
}

cSTYLE_RUN_PROPS StyleResolveRun(cSTYLE_MODELptr model, csi32 paragraphStyle, cSTYLE_DIRECT_RUNptr direct) {
   STYLE_RUN_PROPS props = {0, false, false, false, false, STYLE_VERT_BASELINE};

   cbool paragraphKnown = (paragraphStyle >= 0 && ui32(paragraphStyle) < model->styleCount && model->resolved);
   cbool characterKnown = (direct->characterStyle >= 0 && ui32(direct->characterStyle) < model->styleCount && model->resolved);

   cSTYLE_RESOLVEDptr fromParagraph = (paragraphKnown ? model->resolved + paragraphStyle : nullptr);
   cSTYLE_RESOLVEDptr fromCharacter = (characterKnown ? model->resolved + direct->characterStyle : nullptr);

   cui16 paragraphParity = (fromParagraph ? fromParagraph->toggleParity : ui16(0));
   cui16 characterParity = (fromCharacter ? fromCharacter->toggleParity : ui16(0));
   cui16 chainParity     = ui16(paragraphParity ^ characterParity);

   // The three layers of ISO/IEC 29500-1 17.7.3 as bit masks: what the run itself said is final, then a
   // docDefaults true beats every style, then the chain parity decides whatever neither of them named.
   cui16 untouched   = ui16(~direct->toggleSpecified);
   cui16 fromDirect  = ui16(direct->toggleSpecified & direct->toggleTrue);
   cui16 fromDefault = ui16(untouched & model->defaults.toggleTrue);
   cui16 fromChain   = ui16(untouched & ~model->defaults.toggleTrue & chainParity);

   props.toggles = ui16(fromDirect | fromDefault | fromChain);

   // Everything else is nearest-wins: the run, then its character style chain, then the paragraph style
   // chain, then docDefaults, which is the layer order CONVERSION_REFERENCE 5.2 lists.
   si8 doubleStrike = direct->doubleStrike;

   if(doubleStrike < 0 && fromCharacter) doubleStrike = fromCharacter->doubleStrike;
   if(doubleStrike < 0 && fromParagraph) doubleStrike = fromParagraph->doubleStrike;
   if(doubleStrike < 0) doubleStrike = model->defaults.doubleStrike;
   props.doubleStrike = (doubleStrike > 0);

   si8 webHidden = direct->webHidden;

   if(webHidden < 0 && fromCharacter) webHidden = fromCharacter->webHidden;
   if(webHidden < 0 && fromParagraph) webHidden = fromParagraph->webHidden;
   if(webHidden < 0) webHidden = model->defaults.webHidden;
   props.webHidden = (webHidden > 0);

   si8 monospace = direct->monospace;

   if(monospace < 0 && fromCharacter) monospace = fromCharacter->monospace;
   // The one place this layering is not the same as w:dstrike's. A monospace document baseline would
   // make every run in the file code, so it makes none: the two layers that carry the baseline are
   // dropped and only the run's own w:rFonts and its character style may still say monospace. Those two
   // survive because each is a statement about one run rather than about the document, which is the
   // whole of what the heuristic is for. See StyleReadBaseline and the note in the header.
   if(monospace < 0 && !model->monoDefault) {
      if(fromParagraph) monospace = fromParagraph->monospace;
      if(monospace < 0) monospace = model->defaults.monospace;
   }
   props.monospace = (monospace > 0);
   // Only the *character* style chain can make a run a code span. A paragraph style carrying the code
   // role is a fenced block, and giving each of its runs a backtick pair inside the fence would put
   // literal backticks in code.
   props.codeStyle = (fromCharacter && fromCharacter->role == STYLE_ROLE_CODE);

   STYLE_VERT_ALIGN vertAlign = direct->vertAlign;

   if(vertAlign == STYLE_VERT_UNSET && fromCharacter) vertAlign = fromCharacter->vertAlign;
   if(vertAlign == STYLE_VERT_UNSET && fromParagraph) vertAlign = fromParagraph->vertAlign;
   if(vertAlign == STYLE_VERT_UNSET) vertAlign = model->defaults.vertAlign;
   props.vertAlign = (vertAlign == STYLE_VERT_UNSET ? STYLE_VERT_BASELINE : vertAlign);
   return props;
}

cchptr StyleResultText(OPC_PACKAGEptrc package, cSTYLE_MODELptr model, cSTYLE_RESULT result) {
   if(result == STYLE_ERROR_PART && model && model->lastOpc != OPC_OK) return OpcResultText(package, model->lastOpc);
   if(result == STYLE_ERROR_XML && model && model->lastXml != XML_OK) return XmlResultText(model->lastXml);
   if(result < 0 || result >= STYLE_RESULT_COUNT) return "the style part could not be read";
   return STYLE_RESULT_TEXT[result];
}
