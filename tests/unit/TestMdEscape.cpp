/*
 * File: TestMdEscape.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: Unit tests for the context-aware escaping writer and the line-start and heading passes.
 * To Do: 1) Add the table-cell pipe cases against a real table once M9 emits one.
 *        2) Check the link-destination rule against the targets real producers write, at M7.
 * Dependencies: BuildGuards.h, Check.h, MdEscape.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "MdEscape.h"

//-- Helpers

// Escapes a NUL-terminated literal and compares the result with another. Measuring and writing are both
// exercised, and the two are checked against each other: a disagreement between them would let the
// emitter reserve one length and write a different one.
static cbool EscapedIs(cchptr text, cMD_CONTEXT context, cchptr wanted) {
   char produced[512];
   ui64 length = 0;
   ui64 index  = 0;

   while(text[length]) ++length;

   cui64 measured = MdEscapeMeasure(text, length, context);
   cui64 written  = MdEscapeWrite(produced, sizeof(produced) - 1u, text, length, context);

   produced[written] = 0;
   if(measured != written) return false;
   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// The offset the line-start pass reports for a NUL-terminated literal at the head of a block.
static csi64 LineStartOf(cchptr line) {
   ui64 length = 0;

   while(line[length]) ++length;
   return MdEscapeLineStartAt(line, length, false);
}

// The same, for a line standing under another line of the same block.
static csi64 ContinuedStartOf(cchptr line) {
   ui64 length = 0;

   while(line[length]) ++length;
   return MdEscapeLineStartAt(line, length, true);
}

// The offset the heading-tail pass reports for a NUL-terminated literal.
static csi64 HeadingTailOf(cchptr content) {
   ui64 length = 0;

   while(content[length]) ++length;
   return MdEscapeHeadingTailAt(content, length);
}

//== The suite

void TestMdEscape(void);

void TestMdEscape(void) {
   CheckGroup("MdEscape: the unconditional inline set");
   CHECK(EscapedIs("plain text", MD_CONTEXT_INLINE, "plain text"));
   CHECK(EscapedIs("a*b", MD_CONTEXT_INLINE, "a\\*b"));
   CHECK(EscapedIs("a_b", MD_CONTEXT_INLINE, "a\\_b"));
   CHECK(EscapedIs("a`b", MD_CONTEXT_INLINE, "a\\`b"));
   CHECK(EscapedIs("a~b", MD_CONTEXT_INLINE, "a\\~b"));
   CHECK(EscapedIs("[a]", MD_CONTEXT_INLINE, "\\[a\\]"));
   CHECK(EscapedIs("a\\b", MD_CONTEXT_INLINE, "a\\\\b"));
   CHECK(EscapedIs("trailing\\", MD_CONTEXT_INLINE, "trailing\\\\"));
   CHECK(EscapedIs("**", MD_CONTEXT_INLINE, "\\*\\*"));
   CHECK(EscapedIs("", MD_CONTEXT_INLINE, ""));
   // A pipe is only special inside a table cell, and an exclamation mark never is: the bracket that
   // would complete an image marker is escaped unconditionally, so the marker cannot form.
   CHECK(EscapedIs("a|b", MD_CONTEXT_INLINE, "a|b"));
   CHECK(EscapedIs("![x]", MD_CONTEXT_INLINE, "!\\[x\\]"));
   CHECK(EscapedIs("a>b", MD_CONTEXT_INLINE, "a>b"));
   CHECK(EscapedIs("a#b", MD_CONTEXT_INLINE, "a#b"));
   CHECK(EscapedIs("a-b", MD_CONTEXT_INLINE, "a-b"));

   CheckGroup("MdEscape: the conditional less-than rule");
   CHECK(EscapedIs("<b>", MD_CONTEXT_INLINE, "\\<b>"));
   CHECK(EscapedIs("</b>", MD_CONTEXT_INLINE, "\\</b>"));
   CHECK(EscapedIs("<!--", MD_CONTEXT_INLINE, "\\<!--"));
   CHECK(EscapedIs("<?pi", MD_CONTEXT_INLINE, "\\<?pi"));
   CHECK(EscapedIs("<https://x>", MD_CONTEXT_INLINE, "\\<https://x>"));
   CHECK(EscapedIs("5 < 6", MD_CONTEXT_INLINE, "5 < 6"));
   CHECK(EscapedIs("ends with <", MD_CONTEXT_INLINE, "ends with <"));
   CHECK(EscapedIs("<1", MD_CONTEXT_INLINE, "<1"));

   CheckGroup("MdEscape: the conditional ampersand rule");
   CHECK(EscapedIs("&amp;", MD_CONTEXT_INLINE, "&amp;amp;"));
   CHECK(EscapedIs("&#65;", MD_CONTEXT_INLINE, "&amp;#65;"));
   CHECK(EscapedIs("&#x41;", MD_CONTEXT_INLINE, "&amp;#x41;"));
   CHECK(EscapedIs("&#X41;", MD_CONTEXT_INLINE, "&amp;#X41;"));
   CHECK(EscapedIs("A & B", MD_CONTEXT_INLINE, "A & B"));
   CHECK(EscapedIs("&notanentity", MD_CONTEXT_INLINE, "&notanentity"));
   CHECK(EscapedIs("&;", MD_CONTEXT_INLINE, "&;"));
   CHECK(EscapedIs("&#;", MD_CONTEXT_INLINE, "&#;"));
   CHECK(EscapedIs("&#x;", MD_CONTEXT_INLINE, "&#x;"));
   CHECK(EscapedIs("&", MD_CONTEXT_INLINE, "&"));
   // Thirty-one bytes is the longest name CommonMark knows, so a semicolon further out than the scan
   // reaches is not an entity and the ampersand stays as it is.
   CHECK(EscapedIs("&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;", MD_CONTEXT_INLINE, "&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;"));

   CheckGroup("MdEscape: the other contexts");
   CHECK(EscapedIs("a|b", MD_CONTEXT_TABLE_CELL, "a\\|b"));
   CHECK(EscapedIs("a*b|c", MD_CONTEXT_TABLE_CELL, "a\\*b\\|c"));
   CHECK(EscapedIs("[a]", MD_CONTEXT_LINK_TEXT, "\\[a\\]"));
   CHECK(EscapedIs("[a]", MD_CONTEXT_ALT_TEXT, "\\[a\\]"));
   CHECK(EscapedIs("a*b`c\\d", MD_CONTEXT_CODE_SPAN, "a*b`c\\d"));
   CHECK(EscapedIs("a*b`c\\d", MD_CONTEXT_CODE_BLOCK, "a*b`c\\d"));
   // A raw-HTML fallback still has its inner text parsed as inline content, so the inline set applies
   // there too; what changes is that the two bytes that could open markup become entities unguessed.
   CHECK(EscapedIs("a<b>&c", MD_CONTEXT_HTML, "a&lt;b>&amp;c"));
   CHECK(EscapedIs("plain", MD_CONTEXT_HTML, "plain"));
   CHECK(EscapedIs("*n*", MD_CONTEXT_HTML, "\\*n\\*"));
   CHECK(EscapedIs("5 < 6", MD_CONTEXT_HTML, "5 &lt; 6"));
   CHECK(EscapedIs("A & B", MD_CONTEXT_HTML, "A &amp; B"));

   CheckGroup("MdEscape: link destinations percent-encode");
   CHECK(EscapedIs("http://x/y", MD_CONTEXT_LINK_DEST, "http://x/y"));
   CHECK(EscapedIs("a b", MD_CONTEXT_LINK_DEST, "a%20b"));
   CHECK(EscapedIs("a(b)c", MD_CONTEXT_LINK_DEST, "a%28b%29c"));
   CHECK(EscapedIs("a<b>c", MD_CONTEXT_LINK_DEST, "a%3Cb%3Ec"));
   // An already-encoded target is left alone, or a working %20 would turn into a broken %2520.
   CHECK(EscapedIs("a%20b", MD_CONTEXT_LINK_DEST, "a%20b"));
   CHECK(EscapedIs("a\"b", MD_CONTEXT_LINK_DEST, "a%22b"));

   CheckGroup("MdEscape: the line-start pass");
   CHECK(LineStartOf("# heading") == 0);
   CHECK(LineStartOf("###### deep") == 0);
   CHECK(LineStartOf("####### seven is not a heading") < 0);
   CHECK(LineStartOf("#") == 0);
   CHECK(LineStartOf("#no space") < 0);
   CHECK(LineStartOf("> quote") == 0);
   CHECK(LineStartOf(">no space is still a quote") == 0);
   CHECK(LineStartOf("- bullet") == 0);
   CHECK(LineStartOf("-") == 0);
   CHECK(LineStartOf("-not a bullet") < 0);
   CHECK(LineStartOf("+ bullet") == 0);
   CHECK(LineStartOf("+plus") < 0);
   CHECK(LineStartOf("---") == 0);
   CHECK(LineStartOf("- - -") == 0);
   CHECK(LineStartOf("=x=") < 0);
   // A thematic break allows any amount of space or tab between and after its hyphens, so all of these
   // are one and none of them is a line of text.
   CHECK(LineStartOf("--- -") == 0);
   CHECK(LineStartOf("--- ---") == 0);
   CHECK(LineStartOf("-  -  -") == 0);
   CHECK(LineStartOf("---\t-") == 0);
   CHECK(LineStartOf("-- -") == 0); // Three hyphens in two runs is still three hyphens
   CHECK(LineStartOf("--- x") < 0);
   CHECK(LineStartOf("x ---") < 0);
   CHECK(LineStartOf("1. item") == 1);
   CHECK(LineStartOf("1998. a year") == 4);
   CHECK(LineStartOf("1998) a year") == 4);
   CHECK(LineStartOf("1998.no space") < 0);
   CHECK(LineStartOf("1234567890. ten digits is not a marker") < 0);
   CHECK(LineStartOf("2.") == 1);
   CHECK(LineStartOf("ordinary text") < 0);
   CHECK(LineStartOf("") < 0);
   // The always-escaped inline set has already run by the time a line reaches this pass, so an asterisk
   // or an underscore can never still be standing at the head of one.
   CHECK(LineStartOf("\\* bullet") < 0);

   CheckGroup("MdEscape: a setext underline needs a line above it");
   // Three or more hyphens are a thematic break wherever they stand, so they are always escaped.
   CHECK(LineStartOf("---") == 0);
   CHECK(ContinuedStartOf("---") == 0);
   // Everything else here only underlines the line above it, and a block's first line has none.
   CHECK(LineStartOf("===") < 0);
   CHECK(ContinuedStartOf("===") == 0);
   CHECK(LineStartOf("=") < 0);
   CHECK(ContinuedStartOf("=") == 0);
   CHECK(LineStartOf("--") < 0);
   CHECK(ContinuedStartOf("--") == 0);
   CHECK(ContinuedStartOf("=== ") == 0);
   // A setext underline may carry trailing whitespace and nothing else, so an interior space kills it.
   CHECK(ContinuedStartOf("= =") < 0);
   CHECK(ContinuedStartOf("- -") == 0); // A bullet, not a setext underline, but escaped either way
   CHECK(LineStartOf("-- --") == 0);    // Four hyphens in two runs: a thematic break, not a paragraph
   CHECK(ContinuedStartOf("ordinary") < 0);

   CheckGroup("MdEscape: a delimiter row cannot attach to the line above it");
   // A GFM table is a header row and a delimiter row, and a hard break supplies both inside one
   // paragraph. Escaping the head of anything shaped like a delimiter row is what stops the pair.
   CHECK(LineStartOf("-|-") == 0);
   CHECK(LineStartOf("|---|---|") == 0);
   CHECK(LineStartOf("| --- | :---: |") == 0);
   CHECK(LineStartOf(":-|-:") == 0);
   CHECK(LineStartOf("|") < 0);     // No hyphen, so no delimiter row and no table
   CHECK(LineStartOf("| x |") < 0); // A header row is harmless on its own
   CHECK(LineStartOf("a|b") < 0);   // And so is one that does not begin with a table byte

   CheckGroup("MdEscape: the heading closing sequence");
   CHECK(HeadingTailOf("Sharp #") == 6);
   CHECK(HeadingTailOf("Sharp ###") == 6);
   CHECK(HeadingTailOf("###") == 0);
   CHECK(HeadingTailOf("C#") < 0);
   CHECK(HeadingTailOf("no hashes") < 0);
   CHECK(HeadingTailOf("") < 0);
   CHECK(HeadingTailOf("tab\t#") == 4);
}
