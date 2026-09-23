/*
 * File: NumberingModel.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-10
 * Last Modified: 2026-09-23
 * Description: Numbering part parsing, delegation chasing, override folding and the counter pass.
 * To Do: 1) Share one open-addressed index builder with StyleModel and OpcPackage, which write the
 *           same probe three times over.
 *        2) Report which numId a document referenced and this model could not resolve, as a note.
 *        3) Hold an instance's levels as a reference to its abstract definition's, once a part is
 *           found that declares thousands of instances over one definition.
 * Dependencies: BuildGuards.h, Ir.h, NumberingModel.h, OpcPackage.h, StyleModel.h, XmlPull.h,
 *               typedefs.h, memory management.h, windows.h
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
#include "Ir.h"
#include "OpcPackage.h"
#include "StyleModel.h"
#include "XmlPull.h"
#include "NumberingModel.h"

//-- Tables

// One sentence per NUM_RESULT, in enumeration order.
static constexpr cchptr NUM_RESULT_TEXT[] = {
    "the numbering part was read",                                          // NUM_OK
    "not enough memory to hold the document's numbering",                   // NUM_ERROR_MEMORY
    "the numbering part could not be read",                                 // NUM_ERROR_PART
    "the numbering part is not well-formed XML",                            // NUM_ERROR_XML
    "the numbering part's root element is not w:numbering",                 // NUM_ERROR_ROOT
    "the numbering part declares more definitions than this reader accepts" // NUM_ERROR_LIMIT
};

static_assert(sizeof(NUM_RESULT_TEXT) / sizeof(NUM_RESULT_TEXT[0]) == ui64(NUM_RESULT_COUNT),
              "NumberingModel: the result sentence table and the NUM_RESULT enumeration have drifted apart.");

//-- Counter state

// One row of the counter table: nine counters and a bit saying whether each holds anything at all.
//
// "Unstarted" has to be distinguishable from "holding zero", because w:start may legitimately be zero --
// a decimalZero list, and CommonMark accepts "0." as a marker -- so a sentinel value would collide with
// a real one. A restart therefore clears a bit rather than writing a number, which is also what lets an
// abstract-keyed counter take its start from whichever numId happens to be in force when it next runs.
struct NUM_COUNTER {
   si32 value[NUM_MAX_LEVELS]; ///< What each level's counter holds
   ui16 started;               ///< One bit per level: whether that counter holds anything at all
};

typedef NUM_COUNTER       *NUM_COUNTERptr;
typedef NUM_COUNTER *const NUM_COUNTERptrc;

//-- Small helpers

// FNV-1a over one decimal identifier. Any well-mixed hash will do: it only ever picks a bucket, and
// every candidate is confirmed against the stored identifier before it is returned.
static cui32 NumHash(csi32 value) {
   ui32 hash = 2166136261u;

   for(ui32 shift = 0; shift < 32u; shift += 8u) {
      hash = ui32((hash ^ ((ui32(value) >> shift) & 0xFFu)) * 16777619u);
   }
   return hash;
}

// Reads an attribute as a decimal integer, reporting whether it was one.
//
// A value that is absent, empty or not all digits leaves the destination alone, which is what keeps
// "unspecified" and "zero" apart: w:numId 0 is a specification of "no numbering" and a sentinel of 0
// for an absence would silently un-cancel it.
static cbool NumParseValue(cXML_TEXT value, si32ptrc out) {
   // No cap on how many characters are read, for DocReadDecimal's reason: every value here is an
   // ST_DecimalNumber, which is an xsd:integer, and leading zeros are legal in one. A cap on the
   // value's *length* rather than on its magnitude discards a legal value, and the discard is silent
   // at all eight call sites, each of which seeds its destination with -1 and ignores the result: a
   // padded w:abstractNumId left a w:num with no definition and bulleted the whole list, and a padded
   // w:numId left an instance nothing could resolve. The overflow test below is the real bound, and
   // the sign branch is why this reader cannot simply be DocReadDecimal.
   if(!value.bytes || !value.length) return false;

   si64 parsed   = 0;
   ui64 at       = 0;
   bool negative = false;

   if(value.bytes[0] == '-') {
      negative = true;
      at       = 1u;
      if(value.length == 1u) return false;
   }
   for(; at < value.length; ++at) {
      if(value.bytes[at] < '0' || value.bytes[at] > '9') return false;
      parsed = parsed * 10 + si64(value.bytes[at] - '0');
      if(parsed > 0x7FFFFFFF) return false;
   }
   *out = si32(negative ? -parsed : parsed);
   return true;
}

//-- Growable storage

// Grows a block to hold at least the requested number of elements, doubling so that filling one costs
// amortised constant time. Offsets survive a move, which is why every reference into the heap is one.
static cbool NumReserve(ptrptrc block, ui64ptrc capacity, cui64 needed, cui64 unit) {
   if(needed <= *capacity) return true;

   ui64 grown = (*capacity ? *capacity : 16u);

   while(grown < needed) grown *= 2u;

   ptr fresh = amalloc(grown * unit, 32u);

   if(!fresh) return false;
   if(*block) Copy(*block, fresh, *capacity * unit);
   mdealloc(*block);
   *block    = fresh;
   *capacity = grown;
   return true;
}

// Copies a string into the model's heap and reports where it landed. Offset 0 is always the empty
// string, so a zero offset is a usable value rather than a sentinel a caller has to test for.
static cbool NumHeapAdd(NUM_MODELptrc model, cchptr text, cui64 length, ui32ptrc at) {
   if(!NumReserve((ptrptrc)&model->heap, &model->heapCapacity, model->heapUsed + length + 1u, 1u)) return false;
   *at = ui32(model->heapUsed);
   for(ui64 index = 0; index < length; ++index) model->heap[model->heapUsed + index] = text[index];
   model->heap[model->heapUsed + length] = 0;
   model->heapUsed += length + 1u;
   return true;
}

// The string one heap offset names.
static cchptr NumHeapText(cNUM_MODELptr model, cui32 at) { return (model->heap ? model->heap + at : ""); }

//-- Level defaults

// Clears one level to "this definition declares nothing here".
static void NumClearLevel(NUM_LEVELptrc level) {
   level->start   = -1;
   level->restart = -1;
   level->format  = NUM_FORMAT_ABSENT;
}

// The bullet every degradation falls back to. CONVERSION_REFERENCE 5.4 allows bullets or plain text for
// a definition that cannot be resolved, and a bullet is the branch that keeps what the document did say
// -- that the paragraph is an item -- while inventing no number that could contradict a sibling.
static void NumBulletLevel(NUM_LEVELptrc level) {
   level->start   = 1;
   level->restart = -1;
   level->format  = NUM_FORMAT_BULLET;
}

//-- Part parsing

// What one w:numFmt token classifies as. Only two tokens are special; every other token, recognised or
// not, counts -- see the note on NUM_FORMAT for why an unknown one is a number rather than a bullet.
static cNUM_FORMAT NumFormatOfToken(cXML_TEXT value) {
   if(XmlTextEqual(value, "bullet")) return NUM_FORMAT_BULLET;
   if(XmlTextEqual(value, "none")) return NUM_FORMAT_PLAIN;
   return NUM_FORMAT_ORDERED;
}

// Whether a w:lvlText value is written out as blank space: one or more spaces, tabs or no-break spaces and
// nothing else. Pandoc writes a bullet level of " " for a list item's continuation paragraphs, so that Word
// indents one like an item and draws no marker beside it.
//
// An *empty* value is deliberately not blank here. What an empty w:lvlText draws on a bullet level is a
// question no producer this build has seen answers -- none of the four writes one -- and the one fixture
// that carries it, tests/fixtures/tablecells, was verified on Windows reading it as a bullet. Widening this
// to cover it is one line, and it should be done on evidence rather than on a reading of the schema.
static cbool NumMarkerIsBlank(cXML_TEXT value) {
   ui64 index = 0;

   if(!value.length) return false;

   while(index < value.length) {
      cui8 byte = ui8(value.bytes[index]);

      if(byte == ' ' || byte == '\t') {
         ++index;
         continue;
      }
      if(byte == 0xC2u && index + 1u < value.length && ui8(value.bytes[index + 1u]) == 0xA0u) {
         index += 2u; // U+00A0, which is what a no-break space is spelled in UTF-8
         continue;
      }
      return false;
   }
   return true;
}

// Reads the w:lvl the reader is on and consumes it, reporting which level it defines.
//
// Children are accumulated and evaluated at the close tag rather than as they arrive: the schema fixes
// an order and producers mostly honour it, but a reader that depends on the order refuses valid files.
static cbool NumReadLevel(XML_READERptrc reader, NUM_LEVELptrc level, si32ptrc ilvl) {
   cui32      depthHere = reader->depth;
   NUM_FORMAT format    = NUM_FORMAT_ORDERED;
   si32       start     = -1;
   si32       restart   = -1;
   bool       picture   = false;
   bool       blank     = false;

   *ilvl = -1;
   NumParseValue(XmlAttribute(reader, XML_NS_W, "ilvl"), ilvl);
   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(reader, XML_NS_W, "start")) {
         NumParseValue(XmlAttribute(reader, XML_NS_W, "val"), &start);
      } else if(XmlIsElement(reader, XML_NS_W, "numFmt")) {
         format = NumFormatOfToken(XmlAttribute(reader, XML_NS_W, "val"));
      } else if(XmlIsElement(reader, XML_NS_W, "lvlRestart")) {
         NumParseValue(XmlAttribute(reader, XML_NS_W, "val"), &restart);
      } else if(XmlIsElement(reader, XML_NS_W, "lvlText")) {
         // Only a w:lvlText that is written and blank counts. An absent one says nothing about the marker.
         cXML_TEXT value = XmlAttribute(reader, XML_NS_W, "val");

         blank = (value.bytes != nullptr && NumMarkerIsBlank(value));
      } else if(XmlIsElement(reader, XML_NS_W, "lvlPicBulletId")) {
         // A level whose marker is a picture cannot count, so it is a bullet whatever w:numFmt says.
         // The picture itself is never extracted: it is list decoration rather than document content,
         // and mapping row 14 ignores the glyph in every spelling a producer writes it in.
         picture = true;
      }
      if(!XmlSkipElement(reader)) return false;
   }
   // An absent w:start means 1: it is what every producer means by leaving it out, and 0 -- the type's
   // own zero value -- would put "0." in front of the first item of an ordinary list. A start past the
   // nine digits a CommonMark marker may carry would stop the line being a list at all, so it is capped.
   if(start < 0) start = 1;
   if(start > si32(NUM_MAX_NUMBER)) start = si32(NUM_MAX_NUMBER);
   // A restart under a level shallower than 9 is every level, which is what an absent one already means.
   if(restart < 0 || restart >= si32(NUM_MAX_LEVELS)) restart = -1;
   level->start   = start;
   level->restart = restart;
   level->format  = (picture ? NUM_FORMAT_BULLET : format);
   // A marker a reader cannot see is not a marker. A level whose w:lvlText draws nothing is a continuation
   // paragraph of the list, which is what w:numFmt none says in so many words -- so it becomes that, and
   // is not given a "-" or a number the document never showed. A picture bullet draws its picture whatever
   // the text says, which is why it is the one exception. The counter still counts such a level, exactly
   // as it counts a numFmt none one: the specification increments a level whatever its marker looks like.
   if(blank && !picture) level->format = NUM_FORMAT_PLAIN;
   return true;
}

// Reads the w:abstractNum the reader is on into the model.
static cbool NumReadAbstract(NUM_MODELptrc model, XML_READERptrc reader, boolptrc limit) {
   if(model->abstractCount >= NUM_MAX_ABSTRACT) {
      *limit = true;
      return false;
   }

   NUM_ABSTRACT record;
   cui32        depthHere = reader->depth;

   record.abstractId = -1;
   record.linkAt     = 0;
   record.delegate   = -1;
   for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) NumClearLevel(record.levels + level);
   NumParseValue(XmlAttribute(reader, XML_NS_W, "abstractNumId"), &record.abstractId);
   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(reader, XML_NS_W, "lvl")) {
         NUM_LEVEL level;
         si32      at = -1;

         NumClearLevel(&level);
         if(!NumReadLevel(reader, &level, &at)) return false;
         // A level outside 0 to 8 is not a level this schema has, and a duplicate is the first-wins case
         // every other lookup in this project takes with a repeated identifier.
         if(at >= 0 && at < si32(NUM_MAX_LEVELS) && record.levels[at].format == NUM_FORMAT_ABSENT) {
            record.levels[at] = level;
         }
         continue;
      }
      // w:styleLink is the other end of the arrow w:numStyleLink draws and is never followed: it says
      // "I am the definition behind numbering style X" rather than "use style X's numbering".
      if(XmlIsElement(reader, XML_NS_W, "numStyleLink")) {
         cXML_TEXT value = XmlAttribute(reader, XML_NS_W, "val");
         cui64     kept  = (value.length < NUM_MAX_NAME_BYTES ? value.length : NUM_MAX_NAME_BYTES - 1u);

         if(value.bytes && !record.linkAt && !NumHeapAdd(model, value.bytes, kept, &record.linkAt)) return false;
      }
      if(!XmlSkipElement(reader)) return false;
   }

   cui64 wanted = ui64(model->abstractCount) + 1u;

   if(!NumReserve((ptrptrc)&model->abstracts, &model->abstractCapacity, wanted, sizeof(NUM_ABSTRACT))) return false;
   model->abstracts[model->abstractCount] = record;
   ++model->abstractCount;
   return true;
}

// Reads one w:lvlOverride into an instance's override arrays, and consumes it.
static cbool NumReadOverride(XML_READERptrc reader, NUM_LEVELptrc levels, si32ptrc starts) {
   cui32 depthHere = reader->depth;
   si32  at        = -1;

   NumParseValue(XmlAttribute(reader, XML_NS_W, "ilvl"), &at);

   cbool usable = (at >= 0 && at < si32(NUM_MAX_LEVELS));

   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == depthHere) return true;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(reader, XML_NS_W, "startOverride")) {
         si32 value = -1;

         if(NumParseValue(XmlAttribute(reader, XML_NS_W, "val"), &value) && usable) {
            if(value < 0) value = 0;
            starts[at] = (value > si32(NUM_MAX_NUMBER) ? si32(NUM_MAX_NUMBER) : value);
         }
      } else if(XmlIsElement(reader, XML_NS_W, "lvl")) {
         NUM_LEVEL level;
         si32      inner = -1;

         NumClearLevel(&level);
         if(!NumReadLevel(reader, &level, &inner)) return false;
         // A w:lvl inside a w:lvlOverride replaces the abstract definition's level entirely rather than
         // merging into it: a full level is a complete definition by construction, and merging would
         // let an abstract's w:numFmt survive under a replacement that names a different one.
         if(usable) levels[at] = level;
         continue;
      }
      if(!XmlSkipElement(reader)) return false;
   }
}

// Reads the w:num the reader is on into the model.
static cbool NumReadNum(NUM_MODELptrc model, XML_READERptrc reader, boolptrc limit) {
   if(model->instanceCount >= NUM_MAX_NUMS) {
      *limit = true;
      return false;
   }

   NUM_INSTANCE record;
   NUM_LEVEL    overrides[NUM_MAX_LEVELS];
   si32         starts[NUM_MAX_LEVELS];
   cui32        depthHere = reader->depth;

   record.numId        = -1;
   record.abstractId   = -1;
   record.counterKey   = -1;
   record.overrideMask = 0;
   for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) {
      NumClearLevel(record.levels + level);
      NumClearLevel(overrides + level);
      starts[level] = -1;
   }
   NumParseValue(XmlAttribute(reader, XML_NS_W, "numId"), &record.numId);
   for(;;) {
      cXML_TOKEN token = XmlNext(reader);

      if(token == XML_TOKEN_ERROR || token == XML_TOKEN_END_OF_INPUT) return false;
      if(token == XML_TOKEN_END_ELEMENT && reader->depth == depthHere) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(XmlIsElement(reader, XML_NS_W, "lvlOverride")) {
         if(!NumReadOverride(reader, overrides, starts)) return false;
         continue;
      }
      if(XmlIsElement(reader, XML_NS_W, "abstractNumId")) {
         NumParseValue(XmlAttribute(reader, XML_NS_W, "val"), &record.abstractId);
      }
      if(!XmlSkipElement(reader)) return false;
   }
   // The overrides ride on the record until the abstract definition is known: NumResolveLevels is the
   // pass that can see both, and it folds whatever these leave undefined over from the definition.
   for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) {
      record.levels[level] = overrides[level];
      if(starts[level] < 0) continue;
      record.overrideMask        = ui16(record.overrideMask | (1u << level));
      record.levels[level].start = starts[level];
   }

   cui64 wanted = ui64(model->instanceCount) + 1u;

   if(!NumReserve((ptrptrc)&model->instances, &model->instanceCapacity, wanted, sizeof(NUM_INSTANCE))) return false;
   model->instances[model->instanceCount] = record;
   ++model->instanceCount;
   return true;
}

//-- Identifier indexes

// Builds an open-addressed index of decimal identifiers onto record numbers. Linear probing over a
// power-of-two table at least twice the record count, so the load factor stays under a half and a probe
// is short. A duplicate identifier keeps the first record, which is what every other lookup in this
// project does with one. A failure is not fatal: both callers fall back to a scan.
static si32ptr NumBuildIndex(csi32ptr keys, cui32 count, ui32ptrc mask) {
   ui64 slots = 8u;

   while(slots < ui64(count) * 2u) slots *= 2u;

   si32ptr buckets = (si32ptr)amalloc(slots * sizeof(si32), 32u);

   if(!buckets) return nullptr;
   for(ui64 slot = 0; slot < slots; ++slot) buckets[slot] = -1;
   *mask = ui32(slots - 1u);
   for(ui32 index = 0; index < count; ++index) {
      if(keys[index] < 0) continue; // An identifier that never parsed can never be looked up

      ui32 slot = NumHash(keys[index]) & *mask;
      bool seen = false;

      while(buckets[slot] >= 0) {
         if(keys[buckets[slot]] == keys[index]) {
            seen = true;
            break;
         }
         slot = (slot + 1u) & *mask;
      }
      if(!seen) buckets[slot] = si32(index);
   }
   return buckets;
}

// Answers one identifier out of an index, or -1. The scan is the fallback for an index that could not be
// allocated, which is a speed measure and never a reason to fail a conversion.
static csi32 NumLookup(csi32ptr keys, cui32 count, csi32ptr buckets, cui32 mask, csi32 wanted) {
   if(wanted < 0 || !keys) return -1;
   if(buckets) {
      ui32 slot = NumHash(wanted) & mask;

      for(ui32 step = 0; step <= mask; ++step) {
         csi32 found = buckets[slot];

         if(found < 0) return -1; // An empty slot ends the probe: nothing past it can be this identifier
         if(keys[found] == wanted) return found;
         slot = (slot + 1u) & mask;
      }
      return -1;
   }
   for(ui32 index = 0; index < count; ++index) {
      if(keys[index] == wanted) return si32(index);
   }
   return -1;
}

//-- Resolution

// Follows every w:numStyleLink to the definition that actually carries the levels.
//
// The chase is CONVERSION_REFERENCE 2.9's, in four hops: the abstract definition names a numbering
// style, that style's own w:pPr/w:numPr names a numId, that numId names a w:num, and that w:num names
// the abstract definition holding the levels -- which carries a w:styleLink saying so.
//
// One guard, and the depth cap is it. A bare cap would leave a two-step loop resolving to an arbitrary
// member of itself, on a parity the cap's own value decides -- but every way of leaving this walk bar
// the one that finds a definition carrying levels leaves delegate at -1, so a loop runs the cap out
// and lands on the same answer a visited set would have given sixteen steps earlier. The comment on
// the cap itself, below, says that from the other side.
static void NumResolveDelegates(NUM_MODELptrc model, cSTYLE_MODELptr styles, csi32ptr keys, csi32ptr buckets, cui32 mask) {
   for(ui32 index = 0; index < model->abstractCount; ++index) {
      si32 walk  = si32(index);
      ui32 steps = 0;

      for(;;) {
         cchptr link = NumHeapText(model, model->abstracts[walk].linkAt);

         if(!link[0]) {
            model->abstracts[index].delegate = walk;
            break;
         }
         // Every way of leaving this walk but the one above leaves delegate at -1, which is what makes
         // a definition that resolves to nothing a bullet at every level. So the cap is the whole cycle
         // guard as well as the length guard: a loop runs it out and lands on the same answer a visited
         // set would give, sixteen steps later on a walk that is bounded at sixteen either way.
         if(steps >= NUM_MAX_DELEGATE) break;
         ++steps;

         si32  level  = -1;
         csi32 target = (styles ? StyleNumberingOf(styles, link, &level) : -1);
         csi32 which  = (target > 0 ? NumFind(model, target) : -1);

         if(which < 0) break;

         csi32 next = NumLookup(keys, model->abstractCount, buckets, mask, model->instances[which].abstractId);

         if(next < 0) break;
         walk = next;
      }
   }
}

// Fills one instance's nine levels from the abstract definition it resolved to, folding its own
// w:lvlOverride replacements over the top and then giving every level still undefined the nearest
// shallower one that is defined.
static void NumResolveLevels(NUM_INSTANCEptrc instance, cNUM_ABSTRACTptr abstract) {
   for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) {
      // An override that carried only a w:startOverride left the start on an otherwise undefined level,
      // so the definition's own level is taken and the overriding start put back over it.
      if(instance->levels[level].format != NUM_FORMAT_ABSENT) continue;

      csi32 start = instance->levels[level].start;

      if(abstract) instance->levels[level] = abstract->levels[level];
      if(start >= 0) instance->levels[level].start = start;
   }
   // A level the definition never declared borrows the nearest shallower one that did, which is what a
   // singleLevel definition needs when a paragraph is written one level in under it: the sub-item is
   // part of the same list and takes its format in every renderer. Only the format and the restart rule
   // are borrowed -- the start is the borrowing level's own question, and an unstated one is 1.
   for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) {
      if(instance->levels[level].format != NUM_FORMAT_ABSENT) continue;

      csi32 start = instance->levels[level].start;

      if(level && instance->levels[level - 1u].format != NUM_FORMAT_ABSENT) {
         instance->levels[level]       = instance->levels[level - 1u];
         instance->levels[level].start = 1;
      } else {
         NumBulletLevel(instance->levels + level);
      }
      if(start >= 0) instance->levels[level].start = start;
   }
}

// Resolves every w:num onto an abstract definition and a counter row.
static void NumResolveInstances(NUM_MODELptrc model, csi32ptr keys, csi32ptr buckets, cui32 mask) {
   for(ui32 index = 0; index < model->instanceCount; ++index) {
      NUM_INSTANCEptr instance = model->instances + index;
      csi32           named    = NumLookup(keys, model->abstractCount, buckets, mask, instance->abstractId);
      csi32           resolved = (named >= 0 ? model->abstracts[named].delegate : -1);

      NumResolveLevels(instance, (resolved >= 0 ? model->abstracts + resolved : nullptr));
      // A definition this model could not reach still gets a counter row of its own, because a bullet
      // list resets the ordered levels beneath it exactly as a numbered one does -- the counters are
      // what makes a sub-list restart under each parent item, whatever that parent's own marker is.
      instance->counterKey = (resolved >= 0 ? resolved : si32(model->abstractCount + index));
   }
   model->counterRows = model->abstractCount + model->instanceCount;
}

//== Entry points

void NumOpen(NUM_MODELptrc model) {
   mzero(model, sizeof(NUM_MODEL));
   model->lastXml = XML_OK;
   model->lastOpc = OPC_OK;
   model->part    = -1;
}

cNUM_RESULT NumLoad(NUM_MODELptrc model, OPC_PACKAGEptrc package, csi32 partIndex, cSTYLE_MODELptr styles) {
   if(partIndex < 0) return NUM_OK; // No numbering part: the document declares no lists
   model->part = partIndex;

   cOPC_RESULT loaded = OpcLoadXmlPart(package, partIndex);

   if(loaded != OPC_OK) {
      model->lastOpc = loaded;
      return (loaded == OPC_ERROR_MEMORY ? NUM_ERROR_MEMORY : NUM_ERROR_PART);
   }
   return NumLoadBytes(model, OpcPartBytes(package, partIndex), OpcPartByteCount(package, partIndex), styles);
}

// Returns a model to the state NumOpen leaves it in, without releasing the arenas it has grown: the
// records are what every accessor reads and a half-built one must never be reachable, while the heap
// and the record arrays are capacity NumClose owns and a second load can write over.
static void NumForget(NUM_MODELptrc model) {
   mdealloc(model->buckets);
   model->buckets       = nullptr;
   model->bucketMask    = 0;
   model->abstractCount = 0;
   model->instanceCount = 0;
   model->counterRows   = 0;
   model->hasPart       = false;
}

// Reads one numbering part into a model that has been cleared. Every failure leaves records behind it,
// which is why its caller is the one that owns the contract rather than this.
static cNUM_RESULT NumLoadPart(NUM_MODELptrc model, cui8ptr bytes, cui64 byteCount, cSTYLE_MODELptr styles) {
   XML_READER reader;
   ui32       empty = 0;

   // Offset 0 is reserved as the empty string, so a definition that declares no w:numStyleLink has a
   // linkAt a lookup reads as "nothing" rather than as whichever identifier happened to be stored first.
   if(!model->heap && !NumHeapAdd(model, "", 0, &empty)) return NUM_ERROR_MEMORY;
   model->lastXml = XmlOpen(&reader, bytes, byteCount);
   if(model->lastXml != XML_OK) {
      XmlClose(&reader);
      return NUM_ERROR_XML;
   }

   NUM_RESULT verdict = NUM_OK;
   bool       limit   = false;
   bool       opened  = false;

   for(;;) {
      cXML_TOKEN token = XmlNext(&reader);

      if(token == XML_TOKEN_END_OF_INPUT || token == XML_TOKEN_ERROR) break;
      if(token != XML_TOKEN_START_ELEMENT) continue;
      if(!opened) {
         // The root has to be w:numbering. A part typed as numbering that holds something else is a
         // defective package, not an optional part that happens to be absent, so it is refused.
         if(!XmlIsElement(&reader, XML_NS_W, "numbering")) {
            verdict = NUM_ERROR_ROOT;
            break;
         }
         opened = true;
         continue;
      }
      if(reader.depth == 2u && XmlIsElement(&reader, XML_NS_W, "abstractNum")) {
         if(!NumReadAbstract(model, &reader, &limit)) {
            verdict = (limit ? NUM_ERROR_LIMIT : NUM_ERROR_MEMORY);
            break;
         }
         continue;
      }
      if(reader.depth == 2u && XmlIsElement(&reader, XML_NS_W, "num")) {
         if(!NumReadNum(model, &reader, &limit)) {
            verdict = (limit ? NUM_ERROR_LIMIT : NUM_ERROR_MEMORY);
            break;
         }
         continue;
      }
      // Anything else at this depth is skipped whole, w:numPicBullet among them: its VML picture is
      // list decoration rather than document content, and this reader keeps no relationship of its own
      // in any case, so a level whose marker is one reads as a bullet by way of w:lvlPicBulletId.
      if(!XmlSkipElement(&reader)) break;
   }

   cXML_RESULT broke = reader.result;

   XmlClose(&reader);
   // A tokenizer failure outranks the reason the walk stopped: it names the rule the part broke, which
   // is what a reader of the message can act on.
   if(broke != XML_OK) {
      model->lastXml = broke;
      return NUM_ERROR_XML;
   }
   if(verdict != NUM_OK) return verdict;
   if(!opened) return NUM_ERROR_ROOT; // No root element at all, so the part names no definitions

   si32ptr keys = nullptr;
   cui32   most = (model->instanceCount > model->abstractCount ? model->instanceCount : model->abstractCount);

   if(most) {
      keys = (si32ptr)amalloc(ui64(most) * sizeof(si32), 32u);
      if(!keys) return NUM_ERROR_MEMORY;
   }
   // The numId index outlives this call and NumFind confirms a probe against the instance itself, so
   // the key array it was built from is scratch and goes as soon as both indexes exist.
   if(model->instanceCount) {
      for(ui32 index = 0; index < model->instanceCount; ++index) keys[index] = model->instances[index].numId;
      model->buckets = NumBuildIndex(keys, model->instanceCount, &model->bucketMask);
   }

   ui32    mask    = 0;
   si32ptr buckets = nullptr;

   if(model->abstractCount) {
      for(ui32 index = 0; index < model->abstractCount; ++index) keys[index] = model->abstracts[index].abstractId;
      buckets = NumBuildIndex(keys, model->abstractCount, &mask);
   }
   // The abstract index is load-time only -- nothing above this module ever asks for an abstract by the
   // identifier the part wrote -- so its keys and its buckets are both released before the call returns.
   NumResolveDelegates(model, styles, keys, buckets, mask);
   NumResolveInstances(model, keys, buckets, mask);
   mdealloc(buckets);
   mdealloc(keys);
   model->hasPart = true;
   return NUM_OK;
}

cNUM_RESULT NumLoadBytes(NUM_MODELptrc model, cui8ptr bytes, cui64 byteCount, cSTYLE_MODELptr styles) {
   // A load starts from a clean slate and leaves one behind it when it fails, which is what the header
   // promises: every result but NUM_OK means no definitions were loaded. Half-read records are worse
   // than none, because an instance whose counterKey was never resolved indexes a counter table that a
   // refused load never sized -- so a caller that read past the result would index a null pointer with
   // whatever the record happened to hold. Clearing on the way *in* is the same rule from the other
   // side, and it is what keeps a second load from stranding the first one's numId index.
   NumForget(model);

   cNUM_RESULT verdict = NumLoadPart(model, bytes, byteCount, styles);

   if(verdict != NUM_OK) NumForget(model);
   return verdict;
}

void NumClose(NUM_MODELptrc model) {
   mdealloc(model->buckets);
   mdealloc(model->abstracts);
   mdealloc(model->instances);
   mdealloc(model->heap);
   NumOpen(model);
}

csi32 NumFind(cNUM_MODELptr model, csi32 numId) {
   if(!model || numId < 0 || !model->instanceCount) return -1;
   // Without the index this is a scan, and a lookup happens once per numbered paragraph: a document with
   // many list items and a numbering part with many instances then costs the product of the two, which
   // is the shape M5 found in StyleFind and M7 found twice more. The scan survives only as the fallback
   // for a model whose index could not be allocated.
   if(model->buckets) {
      cui32 mask = model->bucketMask;
      ui32  slot = NumHash(numId) & mask;

      for(ui32 step = 0; step <= mask; ++step) {
         csi32 found = model->buckets[slot];

         if(found < 0) return -1; // An empty slot ends the probe: nothing past it can be this identifier
         if(model->instances[found].numId == numId) return found;
         slot = (slot + 1u) & mask;
      }
      return -1;
   }
   for(ui32 index = 0; index < model->instanceCount; ++index) {
      if(model->instances[index].numId == numId) return si32(index);
   }
   return -1;
}

cui32 NumCount(cNUM_MODELptr model) { return (model ? model->instanceCount : 0u); }

cNUM_LEVEL NumLevelOf(cNUM_MODELptr model, csi32 numId, cui8 level) {
   NUM_LEVEL absent;
   csi32     found = NumFind(model, numId);

   NumClearLevel(&absent);
   if(found < 0) return absent;
   return model->instances[found].levels[level < NUM_MAX_LEVELS ? level : NUM_MAX_LEVELS - 1u];
}

cbool NumAssignMarkers(IR_DOCUMENTptrc document, cNUM_MODELptr model) {
   cui32          blocks   = IrBlockCount(document);
   cui32          rows     = (model ? model->counterRows : 0u);
   cui32          numbers  = (model ? model->instanceCount : 0u);
   NUM_COUNTERptr counters = nullptr;
   ui8ptr         applied  = nullptr;
   si32           lastKey[NUM_MAX_LEVELS];

   if(!blocks) return true;
   if(rows) {
      counters = (NUM_COUNTERptr)amalloc(ui64(rows) * sizeof(NUM_COUNTER), 32u);
      if(!counters) {
         IrFail(document);
         return false;
      }
      for(ui32 row = 0; row < rows; ++row) counters[row].started = 0;
   }
   if(numbers) {
      applied = (ui8ptr)amalloc(numbers, 32u);
      if(!applied) {
         mdealloc(counters);
         IrFail(document);
         return false;
      }
      for(ui32 at = 0; at < numbers; ++at) applied[at] = 0;
   }
   for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) lastKey[level] = -1;
   for(ui32 index = 0; index < blocks; ++index) {
      IR_BLOCKptr block = IrBlockMutable(document, index);

      if(!block) continue;

      csi32 found = NumFind(model, block->listNumId);

      if(found < 0) {
         // Either the paragraph named no list at all, or it named a numId no w:num declares -- which
         // CONVERSION_REFERENCE 2.9 rules is simply not numbered, keeping the text and losing only the
         // marker. Both end the run of items, so whatever follows opens a list of its own.
         block->listNumId = -1;
         block->listLevel = 0;
         block->listFlags = IR_LIST_NONE;
         for(ui32 level = 0; level < NUM_MAX_LEVELS; ++level) lastKey[level] = -1;
         continue;
      }

      NUM_INSTANCEptr instance = model->instances + found;
      cui8            level    = (block->listLevel < NUM_MAX_LEVELS ? block->listLevel : ui8(NUM_MAX_LEVELS - 1u));
      cNUM_LEVEL      defined  = instance->levels[level];

      block->listLevel = level;
      if(defined.format == NUM_FORMAT_PLAIN) {
         // Mapping row 16: an item with no marker at all, indented under the item above it. It touches
         // no counter and clears nothing -- it is modelled as a continuation of that item, and clearing
         // a deeper level here would restart a sub-list the continuation is standing in the middle of.
         block->listFlags  = ui8(IR_LIST_ITEM | IR_LIST_PLAIN);
         block->listNumber = 0;
         continue;
      }

      NUM_COUNTERptr row = counters + instance->counterKey;
      cui16          bit = ui16(1u << level);

      // A w:startOverride is a pending reset keyed by numId and applied the first time that numId is
      // used, which is exactly how Word spells "restart at 1": a new numId over the same abstract
      // definition. Applying every one of an instance's overrides at once keeps the answer independent
      // of which level the document happened to enter the list at.
      if(!applied[found]) {
         applied[found] = 1u;
         for(ui32 at = 0; at < NUM_MAX_LEVELS; ++at) {
            if(!(instance->overrideMask & (1u << at))) continue;
            row->started = ui16(row->started & ~(1u << at));
         }
      }

      si32  value  = defined.start;
      cbool seeded = ((row->started & bit) == 0u);

      if(!seeded) {
         value = (row->value[level] >= si32(NUM_MAX_NUMBER) ? si32(NUM_MAX_NUMBER) : row->value[level] + 1);
      }
      row->started      = ui16(row->started | bit);
      row->value[level] = value;
      for(ui32 deeper = level + 1u; deeper < NUM_MAX_LEVELS; ++deeper) {
         csi32 restart = instance->levels[deeper].restart;

         if(restart == 0) continue;                          // 0 never restarts, whatever runs above it
         if(restart > 0 && si32(level) >= restart) continue; // N restarts only under a level shallower than N
         row->started = ui16(row->started & ~(1u << deeper));
      }

      cbool ordered = (defined.format == NUM_FORMAT_ORDERED);
      // A list the item before it was not part of has to be told apart from one it continues, because
      // two adjacent lists with the same marker merge into one in Markdown and the second one's start
      // number is then discarded (mapping row 17). Either the definition changed, or this level's
      // counter was not running and had to be seeded from the start rather than counted on from a
      // value it held.
      //
      // Seeding is the general fact and a w:startOverride is only one way of arriving at it, which is
      // why the test is about the counter and not about the override: an instance's overrides are all
      // applied together the first time that numId is used, so a numId first used at a deep level
      // restarts its shallow levels at that moment and the item that eventually reaches one of them is
      // beginning a new list there, several blocks later and with no override of its own to point at.
      cbool first = (lastKey[level] != instance->counterKey || seeded);

      block->listNumber = (ordered ? ui32(value) : 0u);
      block->listFlags  = ui8(ui32(IR_LIST_ITEM) | (ordered ? ui32(IR_LIST_ORDERED) : 0u) | (first ? ui32(IR_LIST_FIRST) : 0u));
      lastKey[level]    = instance->counterKey;
      for(ui32 deeper = level + 1u; deeper < NUM_MAX_LEVELS; ++deeper) lastKey[deeper] = -1;
   }
   mdealloc(applied);
   mdealloc(counters);
   return true;
}

cchptr NumResultText(OPC_PACKAGEptrc package, cNUM_MODELptr model, cNUM_RESULT result) {
   if(result == NUM_ERROR_PART && model && model->lastOpc != OPC_OK) return OpcResultText(package, model->lastOpc);
   // A sentence about the bytes of a part names the part, as the walk's does: "a part ends in the middle
   // of an element" does not say which, and the numbering part is found through a relationship, not by name.
   if(result == NUM_ERROR_XML && model && model->lastXml != XML_OK) return OpcMessageIn(package, XmlResultText(model->lastXml), model->part);
   if(result < 0 || result >= NUM_RESULT_COUNT) return "the numbering part could not be read";
   // So does every other sentence about the part's content -- a root that is not w:numbering, and more
   // definitions than the caps allow, which M11's generated fixtures found saying neither which part nor
   // where. A failed allocation is this program's problem rather than the part's, and names nothing.
   if(result != NUM_OK && result != NUM_ERROR_MEMORY && model) return OpcMessageIn(package, NUM_RESULT_TEXT[result], model->part);
   return NUM_RESULT_TEXT[result];
}
