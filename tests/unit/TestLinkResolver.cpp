/*
 * File: TestLinkResolver.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: The GFM slugger and the anchor-name sanitiser, driven from string literals.
 * To Do: 1) Drive the reference lookup itself once a package can be built without an archive.
 *        2) Add the cases a multi-character lower-case mapping needs, once the fold table carries one.
 * Dependencies: BuildGuards.h, Check.h, Ir.h, LinkResolver.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "Ir.h"
#include "LinkResolver.h"

//-- Helpers

// The length of a NUL-terminated literal, so a case can be one comparison.
static cui64 LinkTestLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Whether two NUL-terminated strings are the same bytes.
static cbool LinkTestSame(cchptr produced, cchptr wanted) {
   ui64 index = 0;

   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// Whether one heading's text slugs to exactly what a renderer would generate for it.
static cbool SlugsTo(cchptr text, cchptr wanted) {
   char slug[256];

   LinkSlug(text, LinkTestLength(text), slug, sizeof(slug));
   return LinkTestSame(slug, wanted);
}

// Runs the anchor pass over a document of one heading repeated, each carrying a bookmark, and reports
// the slug the last of them resolved to.
//
// The heading text is built rather than written out because its length is the whole point: a slug that
// fills the name buffer leaves a numbered form with nowhere to go, and the counter used to be written
// with no bounds check at all -- one byte past a 512-byte stack array for a 511-byte slug, and up to ten
// past it once the counter reached two digits. The reserved margin is what this drives.
static cbool NumbersLongSlugs(cui64 letters, cui32 headings, chptrc last, cui64 lastBytes) {
   IR_DOCUMENT document;
   char        name[32];

   IrOpen(&document);
   for(ui32 index = 0; index < headings; ++index) {
      cIR_MARK mark = IrBeginBlock(&document, IR_BLOCK_HEADING, 1u);
      ui64     used = 0;

      name[used++] = 'b';
      if(index >= 10u) name[used++] = char('0' + (index / 10u));
      name[used++] = char('0' + (index % 10u));
      IrAddSpan(&document, IR_SPAN_ANCHOR, IR_FMT_NONE);
      IrAppendDest(&document, name, used);
      IrAddSpan(&document, IR_SPAN_TEXT, IR_FMT_NONE);
      for(ui64 at = 0; at < letters; ++at) IrAppendText(&document, "a", 1u);
      IrEndBlock(&document, mark);
   }
   // Every anchor needs a link at it, or LinkResolveAnchors mutes the lot and never numbers anything.
   cIR_MARK mark = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   for(ui32 index = 0; index < headings; ++index) {
      ui64 used = 0;

      name[used++] = '#';
      name[used++] = 'b';
      if(index >= 10u) name[used++] = char('0' + (index / 10u));
      name[used++] = char('0' + (index % 10u));
      IrAddSpan(&document, IR_SPAN_LINK_START, IR_FMT_NONE);
      IrAppendDest(&document, name, used);
      IrAddSpan(&document, IR_SPAN_TEXT, IR_FMT_NONE);
      IrAppendText(&document, "go", 2u);
      IrAddSpan(&document, IR_SPAN_LINK_END, IR_FMT_NONE);
   }
   IrEndBlock(&document, mark);

   cbool resolved = LinkResolveAnchors(&document);
   ui64  produced = 0;

   last[0] = 0;
   for(ui32 index = 0; index < IrSpanCount(&document); ++index) {
      cIR_SPANptr span = IrSpanAt(&document, index);

      if(!span || span->kind != IR_SPAN_LINK_START || !span->destBytes) continue;
      produced = (span->destBytes < lastBytes ? span->destBytes : lastBytes - 1u);
      for(ui64 at = 0; at < produced; ++at) last[at] = IrDest(&document, span->destAt)[at];
   }
   last[produced] = 0;
   IrClose(&document);
   return resolved;
}

// Whether one bookmark name sanitises to exactly the anchor it should.
static cbool AnchorsTo(cchptr name, cchptr wanted) {
   char safe[256];

   LinkAnchorName(name, LinkTestLength(name), safe, sizeof(safe));
   return LinkTestSame(safe, wanted);
}

//== The suite

void TestLinkResolver(void);

void TestLinkResolver(void) {
   CheckGroup("LinkResolver: the GFM heading slug");
   // CLAUDE.md's mapping row 22: the anchor is the renderer's to generate, so the slug has to be the
   // renderer's rule exactly -- lower case, spaces to hyphens, and everything that is not a letter, a
   // mark, a number or connector punctuation dropped.
   CHECK(SlugsTo("Introduction", "introduction"));
   CHECK(SlugsTo("Getting Started", "getting-started"));
   CHECK(SlugsTo("UPPER CASE", "upper-case"));
   CHECK(SlugsTo("A Section 2 Heading", "a-section-2-heading"));
   CHECK(SlugsTo("snake_case_survives", "snake_case_survives"));
   CHECK(SlugsTo("already-hyphenated", "already-hyphenated"));
   CHECK(SlugsTo("", ""));

   CheckGroup("LinkResolver: the padding a heading's own ends lose");
   // An ATX heading's content is its line stripped of the whitespace at both ends, so a renderer never
   // sees the padding a producer left there and never slugs it. Emitting "#intro-" for "# Intro " is
   // a fragment the page does not answer to, and a heading of nothing but spaces has no anchor at all.
   CHECK(SlugsTo(" Intro ", "intro"));
   CHECK(SlugsTo("   ", ""));
   CHECK(SlugsTo("\tIntro\t", "intro"));
   // Interior padding is kept, one hyphen per space, and a character the keep test drops does not
   // break the run: a renderer removes it and leaves the two spaces adjacent.
   CHECK(SlugsTo("A  B", "a--b"));
   CHECK(SlugsTo("A ! B", "a--b"));
   CHECK(SlugsTo("A\tB", "ab"));
   CHECK(SlugsTo(" ! A", "-a"));

   CheckGroup("LinkResolver: the digits a slug keeps and the numbers it does not");
   // Nd and not N. github-slugger's own removal class settles it inside Latin-1: it removes the
   // superscripts and the vulgar fractions, every one of them category No, while leaving the feminine
   // ordinal, the micro sign and the masculine ordinal standing in the gaps between those ranges. A
   // table built from all of N keeps "1/2" in a fragment the page does not have.
   CHECK(SlugsTo("a\xC2\xB2", "a"));               // U+00B2 SUPERSCRIPT TWO, No
   CHECK(SlugsTo("a\xC2\xBD", "a"));               // U+00BD VULGAR FRACTION ONE HALF, No
   CHECK(SlugsTo("a\xE2\x85\xA8", "a"));           // U+2168 ROMAN NUMERAL NINE, Nl
   CHECK(SlugsTo("a\xC2\xAA", "a\xC2\xAA"));       // U+00AA FEMININE ORDINAL INDICATOR, Lo
   CHECK(SlugsTo("a\xC2\xB5", "a\xC2\xB5"));       // U+00B5 MICRO SIGN, Ll
   CHECK(SlugsTo("\xE0\xA5\xA9", "\xE0\xA5\xA9")); // U+0969 DEVANAGARI DIGIT THREE, Nd

   CheckGroup("LinkResolver: the punctuation a slug drops");
   // The apostrophe is the case that makes the whole table worth generating: "Don't Panic" is
   // "dont-panic", and a converter that kept the quotation mark would write a fragment no heading
   // answers to. Both spellings of it behave the same way, which is the point of using the database.
   CHECK(SlugsTo("Don't Panic!", "dont-panic"));
   CHECK(SlugsTo("Don\xE2\x80\x99t Panic!", "dont-panic"));
   CHECK(SlugsTo("What? Why: How.", "what-why-how"));
   CHECK(SlugsTo("(parentheses) [brackets]", "parentheses-brackets"));
   CHECK(SlugsTo("100% of the time", "100-of-the-time"));
   CHECK(SlugsTo("a + b = c", "a--b--c"));
   // An em dash is punctuation and goes; the hyphen-minus beside it is the one dash that is kept.
   CHECK(SlugsTo("before \xE2\x80\x94 after", "before--after"));

   CheckGroup("LinkResolver: a slug beyond ASCII");
   // Every letter survives and folds by the character database's own mapping, which is what makes a
   // Cyrillic or a Greek heading reach the same fragment the renderer will generate for it.
   CHECK(SlugsTo("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
   CHECK(SlugsTo("\xCE\x91\xCE\xBB\xCF\x86\xCE\xB1", "\xCE\xB1\xCE\xBB\xCF\x86\xCE\xB1"));
   CHECK(SlugsTo("Caf\xC3\x89", "caf\xC3\xA9"));
   // A fold run that steps by two holds only the capitals: the code points between them are already
   // lower case, and folding those by the same delta would push each one past the letter it belongs to.
   CHECK(SlugsTo("\xC4\x80", "\xC4\x81")); // U+0100 folds to U+0101
   CHECK(SlugsTo("\xC4\x81", "\xC4\x81")); // U+0101 is already there and must not move to U+0102
   CHECK(SlugsTo("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"));
   // An ideographic space is a Zs and not the space character, so it is dropped rather than hyphenated
   // -- which is what github-slugger does, because only U+0020 is in its replacement.
   CHECK(SlugsTo("\xE6\x97\xA5\xE3\x80\x80\xE6\x9C\xAC", "\xE6\x97\xA5\xE6\x9C\xAC"));
   // An Arabic full stop and a Devanagari danda are punctuation, and a Latin-centric table would keep
   // both -- the same trap M6 found in its flanking table, one milestone on.
   CHECK(SlugsTo("a\xD8\x9F"
                 "b",
                 "ab"));
   CHECK(SlugsTo("a\xE0\xA5\xA4"
                 "b",
                 "ab"));

   CheckGroup("LinkResolver: a slug long enough to leave its counter nowhere to go");
   // The dedup counter used to be written into the candidate buffer with no bounds check, so a heading
   // whose slug filled that buffer put the digits one byte past a 512-byte stack array -- and up to ten
   // past it once the counter reached two digits. The base slug now stops short by the width of the
   // longest counter, so every numbered form of a slug this module accepts has somewhere to go.
   {
      char produced[LINK_MAX_NAME_BYTES + 64u];

      // 511 is what LinkSlugAppend used to saturate at: its guard is "used + width + 1 > destBytes".
      CHECK(NumbersLongSlugs(511u, 2u, produced, sizeof(produced)));
      CHECK(LinkTestLength(produced) == 1u + LINK_MAX_SLUG_BYTES - 1u + 2u); // '#', the slug, "-1"
      CHECK(produced[LinkTestLength(produced) - 2u] == '-' && produced[LinkTestLength(produced) - 1u] == '1');
      // Twelve of them takes the counter to two digits, which is where a one-byte margin would fail.
      CHECK(NumbersLongSlugs(511u, 12u, produced, sizeof(produced)));
      CHECK(produced[LinkTestLength(produced) - 3u] == '-' && produced[LinkTestLength(produced) - 2u] == '1');
      CHECK(produced[LinkTestLength(produced) - 1u] == '1');
   }

   CheckGroup("LinkResolver: the anchor name an <a id> and its link share");
   // Word allows a bookmark only letters, digits and the underscore, so for every document a word
   // processor wrote this is the identity. It is not the identity for a hand-built part, and that is
   // what it is for: the name reaches an HTML attribute and a URL fragment at once.
   CHECK(AnchorsTo("_Toc123456", "_Toc123456"));
   CHECK(AnchorsTo("mark", "mark"));
   CHECK(AnchorsTo("with space", "with-space"));
   CHECK(AnchorsTo("quote\"and'apostrophe", "quote-and-apostrophe"));
   CHECK(AnchorsTo("angle<bracket>", "angle-bracket-"));
   CHECK(AnchorsTo("dotted.name", "dotted.name"));
   CHECK(AnchorsTo("", ""));
   CHECK(AnchorsTo("\xD0\x9F", "--"));
}
