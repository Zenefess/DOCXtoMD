/*
 * File: TestLinkResolver.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: The GFM slugger and the anchor-name sanitiser, driven from string literals.
 * To Do: 1) Drive the reference lookup itself once a package can be built without an archive.
 *        2) Add the cases a multi-character lower-case mapping needs, once the fold table carries one.
 * Dependencies: BuildGuards.h, Check.h, LinkResolver.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
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
   CHECK(SlugsTo("   ", "---"));

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
