/*
 * File: TestNumberingModel.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-10
 * Last Modified: 2026-09-22
 * Description: Unit tests for the numbering part: indirection, overrides, delegation and the counters.
 * To Do: 1) Drive a second part's counters once M10 walks footnotes, which is where the question of
 *           whether they share one table becomes answerable rather than merely stated.
 *        2) Drive a w:lvlOverride carrying a full w:lvl replacement *and* a w:startOverride at once,
 *           which Word writes for a hybrid list and which no case below separates from either alone.
 * Dependencies: BuildGuards.h, Check.h, Ir.h, NumberingModel.h, StyleModel.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "Ir.h"
#include "NumberingModel.h"
#include "StyleModel.h"

//-- Helpers

// The root element every part below is wrapped in, kept out of the helpers so no line reaches the
// column limit once the formatter has joined what it can.
static constexpr cchptr NUM_HEAD  = "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
static constexpr cchptr NUM_TAIL  = "</w:numbering>";
static constexpr cchptr NUM_STYLE = "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";

// One bullet definition and one decimal one, named rather than repeated.
#define BULLET_0  "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>"
#define DECIMAL_1 "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"3\"/><w:numFmt w:val=\"decimal\"/></w:lvl>"
#define DECIMAL_2 "<w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"lowerLetter\"/></w:lvl></w:abstractNum>"

// Bytes before the terminator.
static cui64 NumTestLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Appends a NUL-terminated literal to a buffer.
static void NumTestAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr text) {
   cui64 length = NumTestLength(text);

   for(ui64 index = 0; index < length && *used + 1u < destBytes; ++index) dest[(*used)++] = text[index];
   dest[*used] = 0;
}

// Wraps a body in a root element and loads it into a prepared model.
static cNUM_RESULT NumTestLoad(NUM_MODELptrc model, cchptr body, cSTYLE_MODELptr styles) {
   char part[4096];
   ui64 used = 0;

   part[0] = 0;
   NumTestAppend(part, sizeof(part), &used, NUM_HEAD);
   NumTestAppend(part, sizeof(part), &used, body);
   NumTestAppend(part, sizeof(part), &used, NUM_TAIL);
   return NumLoadBytes(model, (cui8ptr)part, used, styles);
}

// Loads a styles part from a body, for the delegation cases that need one.
static cbool NumTestStyles(STYLE_MODELptrc styles, cchptr body) {
   char part[2048];
   ui64 used = 0;

   part[0] = 0;
   NumTestAppend(part, sizeof(part), &used, NUM_STYLE);
   NumTestAppend(part, sizeof(part), &used, body);
   NumTestAppend(part, sizeof(part), &used, "</w:styles>");
   return StyleLoadBytes(styles, (cui8ptr)part, used) == STYLE_OK;
}

// Whether two NUL-terminated strings are the same bytes.
static cbool NumTestSame(cchptr produced, cchptr wanted) {
   ui64 index = 0;

   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// One step of a document: which numId a paragraph names and at which level. A numId of -1 is a
// paragraph that is not a list item at all, which is what breaks a run of them.
struct NUM_STEP {
   si32 numId; ///< The w:numId the paragraph carried, or -1 for an ordinary paragraph
   ui32 level; ///< The w:ilvl it carried
};

typedef const NUM_STEP        cNUM_STEP;
typedef const NUM_STEP *const cNUM_STEPptrc;

// Builds a document out of those steps, runs the counter pass over it, and renders the markers it
// assigned into one compact trace: "-" for a bullet, the number for an ordered item, "." for a
// marker-less continuation and "p" for a paragraph that is not an item, each prefixed by its level and
// suffixed with "!" where the item opens a list the one before it was not part of.
//
// Driving the pass over a built document rather than over a string literal is deliberate: the counters
// are a property of document *order*, and the shortest way to say "these paragraphs, in this order" is
// a table of references rather than a body of WordprocessingML.
static cbool NumTestMarkers(cNUM_MODELptr model, cNUM_STEPptrc steps, cui32 count, cchptr wanted) {
   IR_DOCUMENT document;
   char        trace[256];
   ui64        used = 0;

   IrOpen(&document);
   for(ui32 index = 0; index < count; ++index) {
      cIR_MARK mark = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

      if(mark.block < 0) {
         IrClose(&document);
         return false;
      }
      if(steps[index].numId >= 0) IrSetListRef(&document, mark, steps[index].numId, steps[index].level);
      IrAddSpan(&document, IR_SPAN_TEXT, IR_FMT_NONE);
      IrAppendText(&document, "x", 1u);
      IrEndBlock(&document, mark);
   }

   cbool ran = NumAssignMarkers(&document, model);

   trace[0] = 0;
   for(ui32 index = 0; ran && index < IrBlockCount(&document); ++index) {
      cIR_BLOCKptr block = IrBlockAt(&document, index);
      char         one[24];
      ui64         at = 0;

      one[at++] = char('0' + block->listLevel);
      if(!(block->listFlags & IR_LIST_ITEM)) one[at++] = 'p';
      else if(block->listFlags & IR_LIST_PLAIN) one[at++] = '.';
      else if(!(block->listFlags & IR_LIST_ORDERED)) one[at++] = '-';
      else {
         char digits[12];
         ui64 shown = 0;
         ui32 value = block->listNumber;

         do {
            digits[shown++] = char('0' + (value % 10u));
            value /= 10u;
         } while(value && shown < sizeof(digits));
         while(shown) one[at++] = digits[--shown];
      }
      if(block->listFlags & IR_LIST_FIRST) one[at++] = '!';
      one[at++] = ' ';
      one[at]   = 0;
      NumTestAppend(trace, sizeof(trace), &used, one);
   }
   IrClose(&document);
   return ran && NumTestSame(trace, wanted);
}

//== The suite

void TestNumberingModel(void);

void TestNumberingModel(void) {
   CheckGroup("NumberingModel: the part and its two indirections");
   {
      NUM_MODEL model;

      NumOpen(&model);
      CHECK(NumTestLoad(&model, BULLET_0 DECIMAL_1 DECIMAL_2 "<w:num w:numId=\"7\"><w:abstractNumId w:val=\"1\"/></w:num>", nullptr) == NUM_OK);
      CHECK(NumCount(&model) == 1u);
      CHECK(NumFind(&model, 7) == 0);
      CHECK(NumFind(&model, 1) < 0); // The abstract identifier is not a numId, and must not resolve as one
      CHECK(NumLevelOf(&model, 7, 0u).format == NUM_FORMAT_ORDERED);
      CHECK(NumLevelOf(&model, 7, 0u).start == 3);
      // lowerLetter is a counting format, and GitHub-Flavored Markdown has no letters: row 15 says so.
      CHECK(NumLevelOf(&model, 7, 1u).format == NUM_FORMAT_ORDERED);
      CHECK(NumLevelOf(&model, 99, 0u).format == NUM_FORMAT_ABSENT);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // Every level a definition does not declare borrows the nearest shallower one that did, which a
      // singleLevel definition needs the moment a paragraph is written one level in under it.
      NumOpen(&model);
      CHECK(NumTestLoad(&model, BULLET_0 "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>", nullptr) == NUM_OK);
      CHECK(NumLevelOf(&model, 1, 0u).format == NUM_FORMAT_BULLET);
      CHECK(NumLevelOf(&model, 1, 5u).format == NUM_FORMAT_BULLET);
      // A borrowed level's start is its own question, and an unstated one is 1 rather than the lender's.
      CHECK(NumLevelOf(&model, 1, 5u).start == 1);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A w:num naming an abstract definition the part does not declare is a list whose format alone is
      // unknown, so it degrades to a bullet rather than to a plain paragraph: what the document said --
      // that the paragraph is an item -- is kept, and only what it could not say is invented.
      NumOpen(&model);
      CHECK(NumTestLoad(&model, "<w:num w:numId=\"4\"><w:abstractNumId w:val=\"9\"/></w:num>", nullptr) == NUM_OK);
      CHECK(NumLevelOf(&model, 4, 0u).format == NUM_FORMAT_BULLET);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // An unrecognised w:numFmt counts, and so does an absent one. Every token ST_NumberFormat defines
      // but bullet and none is a counting format, so a token this build has never heard of is far more
      // likely to be one than to be a bullet -- and degrading it to a bullet would throw away ordering
      // the counter already has.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"hindiCounting\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"1\"/><w:lvl w:ilvl=\"2\"><w:numFmt w:val=\"none\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      CHECK(NumLevelOf(&model, 1, 0u).format == NUM_FORMAT_ORDERED);
      CHECK(NumLevelOf(&model, 1, 1u).format == NUM_FORMAT_ORDERED);
      CHECK(NumLevelOf(&model, 1, 2u).format == NUM_FORMAT_PLAIN);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A level whose marker is a picture cannot count, so it is a bullet whatever w:numFmt says.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/>"
                        "<w:lvlPicBulletId w:val=\"0\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      CHECK(NumLevelOf(&model, 1, 0u).format == NUM_FORMAT_BULLET);
      NumClose(&model);
   }

   CheckGroup("NumberingModel: what it refuses and what it accepts");
   {
      NUM_MODEL model;

      // An absent part is legal and common -- most documents carry no lists at all -- and yields a model
      // in which every reference is dangling.
      NumOpen(&model);
      CHECK(NumLoad(&model, nullptr, -1, nullptr) == NUM_OK);
      CHECK(NumCount(&model) == 0u);
      CHECK(NumFind(&model, 1) < 0);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      NumOpen(&model);
      CHECK(NumLoadBytes(&model, (cui8ptr) "<w:numbering", 12u, nullptr) == NUM_ERROR_XML);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A part that is present and holds something else is a defective document, not an optional part
      // that happens to be absent, so it is refused on StyleLoad's own reasoning.
      NumOpen(&model);
      cchptr notNumbering = "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>";

      CHECK(NumLoadBytes(&model, (cui8ptr)notNumbering, NumTestLength(notNumbering), nullptr) == NUM_ERROR_ROOT);
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A prefix this build has never seen resolves by URI like every other element (correctness rule 2).
      NumOpen(&model);
      CHECK(NumLoadBytes(&model,
                         (cui8ptr) "<x:numbering xmlns:x=\"http://purl.oclc.org/ooxml/wordprocessingml/main\">"
                                   "<x:num x:numId=\"2\"><x:abstractNumId x:val=\"0\"/></x:num></x:numbering>",
                         141u, nullptr) == NUM_OK);
      CHECK(NumFind(&model, 2) == 0);
      NumClose(&model);
   }
   {
      NUM_MODEL model;
      // Every numeric attribute padded with leading zeros, which ST_DecimalNumber allows because it is
      // an xsd:integer. A cap on the value's *length* discarded all of them at once and said nothing:
      // the w:num lost its w:abstractNumId, so the definition behind it went missing and 5.4 degraded
      // every level to a bullet. DocWalker's twin reader had the same defect on a w:pPr.
      cchptr padded = "<w:abstractNum w:abstractNumId=\"0000000001\"><w:lvl w:ilvl=\"000\">"
                      "<w:start w:val=\"0007\"/><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                      "<w:num w:numId=\"000000000004\"><w:abstractNumId w:val=\"0000000001\"/></w:num>";

      NumOpen(&model);
      CHECK(NumTestLoad(&model, padded, nullptr) == NUM_OK);
      CHECK(NumFind(&model, 4) >= 0);
      {
         cNUM_STEP steps[] = {{4, 0}, {4, 0}};

         CHECK(NumTestMarkers(&model, steps, 2u, "07! 08 "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;
      // The sign branch is why this reader cannot simply be DocReadDecimal, and a padded negative is
      // where the branch and the cap meet: thirteen characters, so the old cap refused it outright and
      // the override never fired at all. NumReadOverride clamps a negative start to zero, so the item
      // numbers from 0 rather than from the definition's own 7.
      cchptr signed_ = "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\">"
                       "<w:start w:val=\"7\"/><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                       "<w:num w:numId=\"5\"><w:abstractNumId w:val=\"1\"/>"
                       "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"-000000000003\"/>"
                       "</w:lvlOverride></w:num>";

      NumOpen(&model);
      CHECK(NumTestLoad(&model, signed_, nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{5, 0}, {5, 0}};

         CHECK(NumTestMarkers(&model, steps, 2u, "00! 01 "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;
      cchptr    torn = "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                       "<w:num w:numId=\"4\"><w:abstractNumId w:val=\"0\"/></w:num><w:num";

      // A refusal leaves *no* definitions behind it, which the header states and which is a safety rule
      // rather than tidiness: an instance read before the part broke has no counterKey, and a refused
      // load never sizes the counter table those keys index. The part below is well-formed up to its
      // last two bytes, so it is refused with one w:num already read.
      NumOpen(&model);
      CHECK(NumLoadBytes(&model, (cui8ptr)torn, NumTestLength(torn), nullptr) == NUM_ERROR_XML);
      CHECK(NumCount(&model) == 0u);
      CHECK(NumFind(&model, 4) < 0);
      CHECK(NumLevelOf(&model, 4, 0u).format == NUM_FORMAT_ABSENT);
      // And a model is loadable again afterwards, which is the same rule from the other side: a second
      // load starts from the clean slate the first one left rather than adding to it.
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\">"
                        "<w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"9\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      CHECK(NumCount(&model) == 1u);
      CHECK(NumFind(&model, 4) < 0);
      CHECK(NumLevelOf(&model, 9, 0u).format == NUM_FORMAT_BULLET);
      // A third load, this time over a model that already carries definitions rather than over one a
      // refusal emptied. It must replace them and not add to them -- and it is also where the numId
      // index the load before it built would be stranded, which nothing but a leak detector would see.
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\">"
                        "<w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"11\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      CHECK(NumCount(&model) == 1u);
      CHECK(NumFind(&model, 9) < 0);
      CHECK(NumLevelOf(&model, 11, 0u).format == NUM_FORMAT_ORDERED);
      NumClose(&model);
   }
   // The sentence table is pinned against the enumeration by exact text, because a table and the
   // enumeration indexing it drift apart silently -- which is a defect M4 found in a table of this shape.
   CHECK(NumTestSame(NumResultText(nullptr, nullptr, NUM_OK), "the numbering part was read"));
   CHECK(NumTestSame(NumResultText(nullptr, nullptr, NUM_ERROR_ROOT), "the numbering part's root element is not w:numbering"));
   CHECK(NumTestSame(NumResultText(nullptr, nullptr, NUM_ERROR_LIMIT), "the numbering part declares more definitions than this reader accepts"));
   CHECK(NumTestSame(NumResultText(nullptr, nullptr, NUM_RESULT_COUNT), "the numbering part could not be read"));

   CheckGroup("NumberingModel: w:numStyleLink delegation");
   {
      NUM_MODEL   model;
      STYLE_MODEL styles;

      // The whole four-hop chase of CONVERSION_REFERENCE 2.9: the abstract definition names a numbering
      // style, that style's own w:numPr names a numId, that numId names a w:num, and that w:num names
      // the definition carrying the levels.
      StyleOpen(&styles);
      NumOpen(&model);
      CHECK(NumTestStyles(&styles, "<w:style w:type=\"numbering\" w:styleId=\"ListBullet\"><w:name w:val=\"List Bullet\"/>"
                                   "<w:pPr><w:numPr><w:numId w:val=\"20\"/></w:numPr></w:pPr></w:style>"));
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"5\"><w:numStyleLink w:val=\"ListBullet\"/></w:abstractNum>"
                        "<w:abstractNum w:abstractNumId=\"6\"><w:styleLink w:val=\"ListBullet\"/>"
                        "<w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"bullet\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"20\"><w:abstractNumId w:val=\"6\"/></w:num>"
                        "<w:num w:numId=\"21\"><w:abstractNumId w:val=\"5\"/></w:num>",
                        &styles) == NUM_OK);
      CHECK(NumLevelOf(&model, 21, 0u).format == NUM_FORMAT_BULLET);
      // Both numIds resolve to the one definition, so they share a counter and continue one sequence.
      {
         cNUM_STEP steps[] = {{20, 0}, {21, 0}};

         CHECK(NumTestMarkers(&model, steps, 2u, "0-! 0- "));
      }
      NumClose(&model);
      StyleClose(&styles);
   }
   {
      NUM_MODEL   model;
      STYLE_MODEL styles;

      // A loop, which CONVERSION_REFERENCE 5.4 says occurs and must not crash. The cap on the walk is
      // the whole guard: a chase that does not end at a definition carrying levels leaves the
      // delegation unresolved however it stopped, and a definition that resolves to nothing is a
      // bullet at every level.
      StyleOpen(&styles);
      NumOpen(&model);
      CHECK(NumTestStyles(&styles, "<w:style w:type=\"numbering\" w:styleId=\"A\">"
                                   "<w:pPr><w:numPr><w:numId w:val=\"1\"/></w:numPr></w:pPr></w:style>"
                                   "<w:style w:type=\"numbering\" w:styleId=\"B\">"
                                   "<w:pPr><w:numPr><w:numId w:val=\"2\"/></w:numPr></w:pPr></w:style>"));
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:numStyleLink w:val=\"B\"/>"
                        "<w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                        "<w:abstractNum w:abstractNumId=\"1\"><w:numStyleLink w:val=\"A\"/>"
                        "<w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
                        "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>",
                        &styles) == NUM_OK);
      CHECK(NumLevelOf(&model, 1, 0u).format == NUM_FORMAT_BULLET);
      CHECK(NumLevelOf(&model, 2, 0u).format == NUM_FORMAT_BULLET);
      NumClose(&model);
      StyleClose(&styles);
   }
   {
      NUM_MODEL model;

      // A delegation with no style model behind it, and one naming a style that does not exist, are the
      // same broken chain and take the same degradation.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:numStyleLink w:val=\"Missing\"/>"
                        "<w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      CHECK(NumLevelOf(&model, 1, 0u).format == NUM_FORMAT_BULLET);
      NumClose(&model);
   }

   CheckGroup("NumberingModel: the counters");
   {
      NUM_MODEL model;
      cchptr part = BULLET_0 DECIMAL_1 DECIMAL_2 "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
                                                 "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
                                                 "<w:num w:numId=\"3\"><w:abstractNumId w:val=\"1\"/>"
                                                 "<w:lvlOverride w:ilvl=\"0\"><w:startOverride w:val=\"1\"/></w:lvlOverride></w:num>";

      NumOpen(&model);
      CHECK(NumTestLoad(&model, part, nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{2, 0}, {2, 0}, {2, 0}};

         // w:start seeds the first item and the rest follow it.
         CHECK(NumTestMarkers(&model, steps, 3u, "03! 04 05 "));
      }
      {
         cNUM_STEP steps[] = {{2, 0}, {-1, 0}, {2, 0}};

         // An interleaved paragraph does not end the sequence: the counters are the converter's own and
         // there is no "list ended" event in the model at all.
         CHECK(NumTestMarkers(&model, steps, 3u, "03! 0p 04! "));
      }
      {
         cNUM_STEP steps[] = {{2, 0}, {2, 1}, {2, 1}, {2, 0}, {2, 1}};

         // A deeper level counts on its own and is cleared every time a shallower one runs.
         CHECK(NumTestMarkers(&model, steps, 5u, "03! 11! 12 04 11! "));
      }
      {
         cNUM_STEP steps[] = {{2, 0}, {2, 0}, {3, 0}, {3, 0}};

         // Two numIds over one abstract definition share a counter, and a w:startOverride restarts it --
         // once, at the first use of the numId that carries it, which is how Word spells "restart".
         CHECK(NumTestMarkers(&model, steps, 4u, "03! 04 01! 02 "));
      }
      {
         cNUM_STEP steps[] = {{2, 0}, {1, 0}, {2, 0}};

         // Two definitions count independently, and a change of definition opens a new list.
         CHECK(NumTestMarkers(&model, steps, 3u, "03! 0-! 04! "));
      }
      {
         cNUM_STEP steps[] = {{2, 0}, {3, 1}, {3, 0}};

         // An instance's overrides are all applied the first time its numId is used, so a numId first
         // used at a deeper level restarts its shallow levels *there*. The item that eventually reaches
         // one of them begins a new list, several blocks later and with no override of its own to point
         // at -- so the question the flag asks has to be whether the counter had to be seeded and not
         // whether an override just fired. Left as the second, the third item below is not marked first,
         // the emitter writes no separator, and a reader's renderer merges it into the list above and
         // renumbers it from that list's own start: the "1." the document asked for reaches the page
         // as 4.
         CHECK(NumTestMarkers(&model, steps, 3u, "03! 11! 01! "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // w:lvlRestart: 0 never restarts, and N restarts only under a level shallower than N. Both
      // directions need a case in one document, or the test passes with the rule unimplemented -- the
      // default is the common behaviour and would carry a suite on its own.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"decimal\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"2\"><w:numFmt w:val=\"decimal\"/><w:lvlRestart w:val=\"0\"/></w:lvl></w:abstractNum>"
                        "<w:abstractNum w:abstractNumId=\"1\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"decimal\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"2\"><w:numFmt w:val=\"decimal\"/><w:lvlRestart w:val=\"1\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>"
                        "<w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>",
                        nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{1, 2}, {1, 1}, {1, 2}};

         CHECK(NumTestMarkers(&model, steps, 3u, "21! 11! 22! ")); // val 0: never restarted
      }
      {
         cNUM_STEP steps[] = {{2, 2}, {2, 1}, {2, 2}, {2, 0}, {2, 2}};

         // val 1: a level 1 above it does not restart it, a level 0 does.
         CHECK(NumTestMarkers(&model, steps, 5u, "21! 11! 22! 01! 21! "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A marker-less continuation touches no counter and clears nothing: it is modelled as part of the
      // item above it, and clearing a deeper level here would restart a sub-list it stands in.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"decimal\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"none\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{1, 0}, {1, 1}, {1, 0}};

         CHECK(NumTestMarkers(&model, steps, 3u, "01! 1. 02 "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A hostile w:start saturates rather than wrapping, and stops at the nine digits a CommonMark
      // marker may carry -- a tenth would stop the line being a list at all.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:start w:val=\"2147483647\"/>"
                        "<w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      CHECK(NumLevelOf(&model, 1, 0u).start == 999999999);
      {
         cNUM_STEP steps[] = {{1, 0}, {1, 0}};

         CHECK(NumTestMarkers(&model, steps, 2u, "0999999999! 0999999999 "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A dangling numId is not numbered, which CONVERSION_REFERENCE 2.9 states outright, and the pass
      // clears the reference so the block is an ordinary paragraph from there on.
      NumOpen(&model);
      CHECK(NumTestLoad(&model, BULLET_0 "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>", nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{1, 0}, {77, 0}, {1, 0}};

         CHECK(NumTestMarkers(&model, steps, 3u, "0-! 0p 0-! "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // A w:ilvl past the nine levels the schema has is clamped rather than refused, which is the whole
      // family's treatment: degrade a malformed numbering value, never reject the document for one.
      NumOpen(&model);
      CHECK(NumTestLoad(&model, BULLET_0 "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>", nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{1, 40}};

         CHECK(NumTestMarkers(&model, steps, 1u, "8-! "));
      }
      NumClose(&model);
   }
   {
      NUM_MODEL model;

      // Every counter is cleared by a shallower level whatever that level's own marker is: a bullet list
      // has to restart the numbered sub-list under each of its items exactly as a numbered one does.
      NumOpen(&model);
      CHECK(NumTestLoad(&model,
                        "<w:abstractNum w:abstractNumId=\"0\"><w:lvl w:ilvl=\"0\"><w:numFmt w:val=\"bullet\"/></w:lvl>"
                        "<w:lvl w:ilvl=\"1\"><w:numFmt w:val=\"decimal\"/></w:lvl></w:abstractNum>"
                        "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num>",
                        nullptr) == NUM_OK);
      {
         cNUM_STEP steps[] = {{1, 0}, {1, 1}, {1, 1}, {1, 0}, {1, 1}};

         CHECK(NumTestMarkers(&model, steps, 5u, "0-! 11! 12 0- 11! "));
      }
      NumClose(&model);
   }
}
