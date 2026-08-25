/*
 * File: TestDocWalker.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: Unit tests for the body walk: wrappers, run content, and the formatting bits on a span.
 * To Do: 1) Add table and hyperlink cases as M7 and M9 give the walker something to build from them.
 *        2) Drive the field state machine's traces once M10 replaces today's skip-it-whole handling.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, StyleModel.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "DocWalker.h"
#include "Ir.h"
#include "StyleModel.h"

//-- Helpers

// The root element every body below is wrapped in, with the two namespaces the cases need bound on it.
static constexpr cchptr WALK_HEAD = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
                                    " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\"><w:body>";
static constexpr cchptr WALK_TAIL = "</w:body></w:document>";

// Bytes before the terminator.
static cui64 WalkLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Appends a NUL-terminated literal to a buffer.
static void WalkAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr text) {
   cui64 length = WalkLength(text);

   for(ui64 index = 0; index < length && *used + 1u < destBytes; ++index) dest[(*used)++] = text[index];
   dest[*used] = 0;
}

// Renders the whole intermediate representation into one compact trace, so a case is one string
// comparison rather than ten assertions. A heading is H<level>{...} and a paragraph is P{...}; inside
// a block, [text] is a text span, | is a hard break, and the letters before a bracket are its
// formatting: b bold, i italic, s strike, ^ superscript and v subscript.
static void WalkTrace(cIR_DOCUMENTptr document, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   dest[0] = 0;
   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);
      char         head[8];

      if(block->kind == IR_BLOCK_HEADING) {
         head[0] = 'H';
         head[1] = char('0' + block->headingLevel);
         head[2] = '{';
         head[3] = 0;
      } else {
         head[0] = 'P';
         head[1] = '{';
         head[2] = 0;
      }
      WalkAppend(dest, destBytes, &used, head);
      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(span->kind == IR_SPAN_BREAK) {
            WalkAppend(dest, destBytes, &used, "|");
            continue;
         }
         if(span->fmt & IR_FMT_BOLD) WalkAppend(dest, destBytes, &used, "b");
         if(span->fmt & IR_FMT_ITALIC) WalkAppend(dest, destBytes, &used, "i");
         if(span->fmt & IR_FMT_STRIKE) WalkAppend(dest, destBytes, &used, "s");
         if(span->fmt & IR_FMT_SUPER) WalkAppend(dest, destBytes, &used, "^");
         if(span->fmt & IR_FMT_SUB) WalkAppend(dest, destBytes, &used, "v");
         WalkAppend(dest, destBytes, &used, "[");
         for(ui32 byte = 0; byte < span->textBytes && used + 1u < destBytes; ++byte) {
            dest[used++] = IrText(document, span->textAt)[byte];
         }
         dest[used] = 0;
         WalkAppend(dest, destBytes, &used, "]");
      }
      WalkAppend(dest, destBytes, &used, "}");
   }
}

// Walks one body, with an optional styles part in front of it, and compares the trace with a literal.
static cbool TracedAs(cchptr styleBody, cchptr body, cchptr wanted) {
   char part[8192];
   char trace[2048];
   ui64 used = 0;

   part[0] = 0;
   WalkAppend(part, sizeof(part), &used, WALK_HEAD);
   WalkAppend(part, sizeof(part), &used, body);
   WalkAppend(part, sizeof(part), &used, WALK_TAIL);

   STYLE_MODEL styles;
   IR_DOCUMENT document;

   StyleOpen(&styles);
   if(styleBody) {
      char stylePart[4096];
      ui64 styleUsed = 0;

      stylePart[0] = 0;
      WalkAppend(stylePart, sizeof(stylePart), &styleUsed, "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");
      WalkAppend(stylePart, sizeof(stylePart), &styleUsed, styleBody);
      WalkAppend(stylePart, sizeof(stylePart), &styleUsed, "</w:styles>");
      if(StyleLoadBytes(&styles, (cui8ptr)stylePart, styleUsed) != STYLE_OK) {
         StyleClose(&styles);
         return false;
      }
   }
   IrOpen(&document);

   cWALK_STATUS status = DocWalkBytes(&document, &styles, (cui8ptr)part, used);

   if(status.result != WALK_OK) {
      IrClose(&document);
      StyleClose(&styles);
      return false;
   }
   WalkTrace(&document, trace, sizeof(trace));
   IrClose(&document);
   StyleClose(&styles);

   ui64 index = 0;

   while(trace[index] && trace[index] == wanted[index]) ++index;
   return trace[index] == wanted[index];
}

// Walks one body and reports only why it stopped.
static cWALK_RESULT WalkedTo(cchptr part) {
   STYLE_MODEL styles;
   IR_DOCUMENT document;

   StyleOpen(&styles);
   IrOpen(&document);

   cWALK_STATUS status = DocWalkBytes(&document, &styles, (cui8ptr)part, WalkLength(part));

   IrClose(&document);
   StyleClose(&styles);
   return status.result;
}

//== The suite

void TestDocWalker(void);

void TestDocWalker(void) {
   CheckGroup("DocWalker: paragraphs and run content");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>one</w:t></w:r></w:p>", "P{[one]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t></w:r><w:r><w:t>b</w:t></w:r></w:p>", "P{[a][b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:t>b</w:t></w:r></w:p>", "P{[ab]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:tab/><w:t>b</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>e</w:t><w:noBreakHyphen/><w:t>mail</w:t></w:r></w:p>", "P{[e-mail]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>hy</w:t><w:softHyphen/><w:t>phen</w:t></w:r></w:p>", "P{[hyphen]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:cr/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br w:type=\"textWrapping\"/><w:t>b</w:t></w:r></w:p>", "P{[a]|[b]}"));
   // A page or column break maps to nothing at all, so the text on either side of it joins up.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br w:type=\"page\"/><w:t>b</w:t></w:r></w:p>", "P{[ab]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a</w:t><w:br w:type=\"column\"/><w:t>b</w:t></w:r></w:p>", "P{[ab]}"));
   // A soft hyphen written as a literal U+00AD goes the same way as the element, per row 34.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>hy\xC2\xADphen</w:t></w:r></w:p>", "P{[hyphen]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>keep \xC2\xA0 the nbsp</w:t></w:r></w:p>", "P{[keep \xC2\xA0 the nbsp]}"));

   CheckGroup("DocWalker: a line end inside a w:t is interior whitespace");
   // WordprocessingML spells a break w:br. A newline character inside a w:t is whitespace, and emitting
   // it would end the Markdown block the text stands in -- a paragraph would become two.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#10;b</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#13;b</w:t></w:r></w:p>", "P{[a b]}"));
   // A carriage return and a line feed together are one line end, so they become one space.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#13;&#10;b</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a&#10;&#10;b</w:t></w:r></w:p>", "P{[a  b]}"));
   // A literal newline inside the element goes the same way as one written as a reference.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>a\nb</w:t></w:r></w:p>", "P{[a b]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>a&#10;b</w:t></w:r></w:p>", "P{[A B]}"));

   CheckGroup("DocWalker: empty and whitespace-only blocks are dropped");
   CHECK(TracedAs(nullptr, "<w:p/>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r/></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t/></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p/><w:p/><w:p><w:r><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:br/></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t>x</w:t><w:br/></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:br/><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   // A block that is dropped must give its arena back, or the next block's text starts in the wrong place.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:t xml:space=\"preserve\"> </w:t></w:r></w:p><w:p><w:r><w:t>after</w:t></w:r></w:p>", "P{[after]}"));

   CheckGroup("DocWalker: headings come from the style model");
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"H1\"><w:name w:val=\"heading 1\"/></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H1{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"2\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H3{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"9\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "P{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:outlineLvl w:val=\"x\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "P{[t]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:pPr><w:pStyle w:val=\"Absent\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "P{[t]}"));

   CheckGroup("DocWalker: the formatting bits on a span");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{bi[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:strike/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{s[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:dstrike/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{s[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{^[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vertAlign w:val=\"subscript\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{v[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b w:val=\"0\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:u w:val=\"single\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"B\"><w:name w:val=\"B\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"B\"/></w:pPr><w:r><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"B\"><w:name w:val=\"B\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"B\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs("<w:style w:type=\"character\" w:styleId=\"C\"><w:name w:val=\"C\"/><w:rPr><w:i/></w:rPr></w:style>",
                  "<w:p><w:r><w:rPr><w:rStyle w:val=\"C\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{i[x]}"));

   CheckGroup("DocWalker: a heading's bold is style-borne and is not carried as formatting");
   // CLAUDE.md's mapping row 1: heading text is never additionally bolded. IR_FMT is the only channel
   // the emitter has, so the walker clears the bit rather than leaving M6 to guess where it came from.
   CHECK(TracedAs("<w:style w:styleId=\"H1\"><w:name w:val=\"heading 1\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "H1{[t]}"));
   CHECK(TracedAs("<w:style w:styleId=\"H1\"><w:name w:val=\"heading 1\"/></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H1\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr>"
                  "<w:t>t</w:t></w:r></w:p>",
                  "H1{[t]}"));
   // Italic and strike are not what the ruling names, so they survive into the heading's spans.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:rPr><w:i/></w:rPr>"
                  "<w:t>t</w:t></w:r></w:p>",
                  "H1{i[t]}"));
   // An ordinary paragraph is untouched by the rule.
   CHECK(TracedAs("<w:style w:styleId=\"B\"><w:name w:val=\"B\"/><w:rPr><w:b/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"B\"/></w:pPr>"
                  "<w:r><w:t>t</w:t></w:r></w:p>",
                  "P{b[t]}"));

   CheckGroup("DocWalker: hidden runs are dropped whole");
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vanish/></w:rPr><w:t>x</w:t></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:vanish/></w:rPr><w:t>gone</w:t></w:r><w:r><w:t>kept</w:t></w:r></w:p>", "P{[kept]}"));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:vanish/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:t>x</w:t></w:r></w:p>", ""));
   CHECK(TracedAs("<w:style w:type=\"paragraph\" w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:vanish/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:rPr><w:vanish w:val=\"0\"/></w:rPr>"
                  "<w:t>x</w:t></w:r></w:p>",
                  "P{[x]}"));

   CheckGroup("DocWalker: transparent wrappers and dropped revisions");
   CHECK(TracedAs(nullptr, "<w:p><w:ins><w:r><w:t>x</w:t></w:r></w:ins></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:moveTo><w:r><w:t>x</w:t></w:r></w:moveTo></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:del><w:r><w:delText>x</w:delText></w:r></w:del></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:moveFrom><w:r><w:t>x</w:t></w:r></w:moveFrom></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:ins><w:p><w:r><w:t>x</w:t></w:r></w:p></w:ins>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:del><w:p><w:r><w:delText>x</w:delText></w:r></w:p></w:del>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:smartTag><w:r><w:t>x</w:t></w:r></w:smartTag></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:customXml><w:r><w:t>x</w:t></w:r></w:customXml></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:sdt><w:sdtPr/><w:sdtContent><w:p><w:r><w:t>x</w:t></w:r></w:p></w:sdtContent></w:sdt>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:sdt><w:sdtPr/><w:sdtContent><w:r><w:t>x</w:t></w:r></w:sdtContent></w:sdt></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:sdt><w:sdtPr><w:alias w:val=\"a\"/></w:sdtPr></w:sdt>", ""));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:bookmarkStart w:id=\"0\" w:name=\"n\"/><w:r><w:t>x</w:t></w:r>"
                  "<w:bookmarkEnd w:id=\"0\"/></w:p>",
                  "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:proofErr w:type=\"spellStart\"/><w:r><w:t>x</w:t></w:r></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:hyperlink><w:r><w:t>x</w:t></w:r></w:hyperlink></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:fldSimple w:instr=\" PAGE \"><w:r><w:t>7</w:t></w:r></w:fldSimple></w:p>", "P{[7]}"));

   CheckGroup("DocWalker: field instructions never reach the output");
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:fldChar w:fldCharType=\"begin\"/></w:r>"
                  "<w:r><w:instrText> TOC \\o </w:instrText></w:r>"
                  "<w:r><w:fldChar w:fldCharType=\"separate\"/></w:r>"
                  "<w:r><w:t>result</w:t></w:r>"
                  "<w:r><w:fldChar w:fldCharType=\"end\"/></w:r></w:p>",
                  "P{[result]}"));

   CheckGroup("DocWalker: mc:AlternateContent takes the fallback");
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:r><w:t>choice</w:t></w:r></mc:Choice>"
                  "<mc:Fallback><w:r><w:t>fallback</w:t></w:r></mc:Fallback></mc:AlternateContent></w:p>",
                  "P{[fallback]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"wps\"><w:r><w:t>choice</w:t></w:r></mc:Choice>"
                  "</mc:AlternateContent></w:p>",
                  "P{[choice]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Choice Requires=\"a\"><w:r><w:t>one</w:t></w:r></mc:Choice>"
                  "<mc:Choice Requires=\"b\"><w:r><w:t>two</w:t></w:r></mc:Choice></mc:AlternateContent></w:p>",
                  "P{[one]}"));
   CHECK(TracedAs(nullptr,
                  "<mc:AlternateContent><mc:Choice Requires=\"wps\"><w:p><w:r><w:t>choice</w:t></w:r></w:p></mc:Choice>"
                  "<mc:Fallback><w:p><w:r><w:t>fallback</w:t></w:r></w:p></mc:Fallback></mc:AlternateContent>",
                  "P{[fallback]}"));

   CHECK(TracedAs(nullptr,
                  "<w:p><mc:AlternateContent><mc:Fallback><w:r><w:t>only a fallback</w:t></w:r></mc:Fallback>"
                  "</mc:AlternateContent></w:p>",
                  "P{[only a fallback]}"));

   CheckGroup("DocWalker: caps uppercases the text, and hidden runs go either way");
   // Mapping row 37: caps uppercases, smallCaps leaves the text as typed.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>chapter one</w:t></w:r></w:p>", "P{[CHAPTER ONE]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:smallCaps/></w:rPr><w:t>chapter one</w:t></w:r></w:p>", "P{[chapter one]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>caf\xC3\xA9 na\xC3\xAFve</w:t></w:r></w:p>", "P{[CAF\xC3\x89 NA\xC3\x8FVE]}"));
   // Two exclusions, both because 0x20 is not their distance: the sharp s grows to two letters when it
   // is uppercased, and y-diaeresis's uppercase form is nowhere near it.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>stra\xC3\x9F"
                  "e</w:t></w:r></w:p>",
                  "P{[STRA\xC3\x9F"
                  "E]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>1 \xC3\xB7 2</w:t></w:r></w:p>", "P{[1 \xC3\xB7 2]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:caps/></w:rPr><w:t>hy</w:t><w:softHyphen/>"
                  "<w:t>phen</w:t></w:r></w:p>",
                  "P{[HYPHEN]}"));
   // caps is a toggle, so two specifications of it across the two style chains cancel the way any
   // other pair does -- while a run naming it itself is direct formatting, which is final.
   CHECK(TracedAs("<w:style w:styleId=\"C\"><w:name w:val=\"C\"/><w:rPr><w:caps/></w:rPr></w:style>"
                  "<w:style w:type=\"character\" w:styleId=\"K\"><w:name w:val=\"K\"/>"
                  "<w:rPr><w:caps/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"C\"/></w:pPr><w:r><w:rPr><w:rStyle w:val=\"K\"/></w:rPr>"
                  "<w:t>quiet</w:t></w:r></w:p>",
                  "P{[quiet]}"));
   CHECK(TracedAs("<w:style w:styleId=\"C\"><w:name w:val=\"C\"/><w:rPr><w:caps/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"C\"/></w:pPr><w:r><w:rPr><w:caps/></w:rPr>"
                  "<w:t>loud</w:t></w:r></w:p>",
                  "P{[LOUD]}"));
   // w:webHidden is not a toggle -- 17.7.3 does not list it -- but a run it hides is dropped all the same.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:r><w:rPr><w:webHidden/></w:rPr><w:t>gone</w:t></w:r>"
                  "<w:r><w:t>kept</w:t></w:r></w:p>",
                  "P{[kept]}"));
   CHECK(TracedAs("<w:style w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:webHidden/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:rPr><w:webHidden w:val=\"0\"/></w:rPr>"
                  "<w:t>shown</w:t></w:r></w:p>",
                  "P{[shown]}"));
   // Nearest-wins, not XOR: two specifications of true stay true where two toggles would cancel.
   CHECK(TracedAs("<w:style w:styleId=\"H\"><w:name w:val=\"H\"/><w:rPr><w:webHidden/></w:rPr></w:style>",
                  "<w:p><w:pPr><w:pStyle w:val=\"H\"/></w:pPr><w:r><w:rPr><w:webHidden/></w:rPr>"
                  "<w:t>gone</w:t></w:r></w:p>",
                  ""));

   CheckGroup("DocWalker: run containers whose text is content");
   CHECK(TracedAs(nullptr, "<w:p><w:dir w:val=\"rtl\"><w:r><w:t>x</w:t></w:r></w:dir></w:p>", "P{[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:bdo w:val=\"ltr\"><w:r><w:t>x</w:t></w:r></w:bdo></w:p>", "P{[x]}"));
   // A ruby annotation is printed above its base text, which Markdown has nowhere to put; the base is
   // the sentence, so it is what survives.
   CHECK(TracedAs(nullptr,
                  "<w:p><w:ruby><w:rubyPr/><w:rt><w:r><w:t>anno</w:t></w:r></w:rt>"
                  "<w:rubyBase><w:r><w:t>base</w:t></w:r></w:rubyBase></w:ruby></w:p>",
                  "P{[base]}"));

   CheckGroup("DocWalker: what is skipped whole");
   CHECK(TracedAs(nullptr, "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>x</w:t></w:r></w:p></w:tc></w:tr></w:tbl>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:drawing><w:t>x</w:t></w:drawing></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:sym w:font=\"Symbol\" w:char=\"F0B7\"/></w:r></w:p>", ""));
   CHECK(TracedAs(nullptr, "<w:sectPr><w:pgSz w:w=\"1\"/></w:sectPr>", ""));
   CHECK(TracedAs(nullptr, "<w:unheardOf><w:p><w:r><w:t>x</w:t></w:r></w:p></w:unheardOf>", ""));

   CheckGroup("DocWalker: a skip does not eat the siblings that follow it");
   CHECK(TracedAs(nullptr,
                  "<w:tbl><w:tr><w:tc><w:p><w:r><w:t>in a cell</w:t></w:r></w:p></w:tc></w:tr></w:tbl>"
                  "<w:p><w:r><w:t>after</w:t></w:r></w:p>",
                  "P{[after]}"));
   CHECK(TracedAs(nullptr,
                  "<w:unheardOf><w:p><w:r><w:t>inside</w:t></w:r></w:p></w:unheardOf>"
                  "<w:p><w:r><w:t>after</w:t></w:r></w:p>",
                  "P{[after]}"));
   CHECK(TracedAs(nullptr,
                  "<w:p><w:unheardOf><w:r><w:t>inside</w:t></w:r></w:unheardOf>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "P{[after]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:unheardOf><w:t>inside</w:t></w:unheardOf><w:t>after</w:t></w:r></w:p>", "P{[after]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:unheardOf><w:b/></w:unheardOf></w:rPr><w:t>x</w:t></w:r></w:p>", "P{[x]}"));

   CheckGroup("DocWalker: a pretty-printed part leaks no indentation");
   // Whitespace between elements is character data too. Only what stands inside a w:t is content, and
   // a part a producer indented for a human must convert to the same bytes as one it did not.
   CHECK(TracedAs(nullptr,
                  "\n  <w:p>\n    <w:pPr>\n      <w:pStyle w:val=\"None\"/>\n    </w:pPr>\n"
                  "    <w:r>\n      <w:t>one</w:t>\n    </w:r>\n"
                  "    <w:r>\n      <w:t xml:space=\"preserve\"> two</w:t>\n    </w:r>\n  </w:p>\n",
                  "P{[one][ two]}"));

   CheckGroup("DocWalker: complex-script bold and italic fold into one bit each");
   // Word writes w:b and w:bCs together whenever anything is bolded, so a run carrying only the
   // complex-script twin is complex-script text that really is bold.
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:bCs/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:iCs/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{i[x]}"));
   CHECK(TracedAs(nullptr, "<w:p><w:r><w:rPr><w:b/><w:bCs/></w:rPr><w:t>x</w:t></w:r></w:p>", "P{b[x]}"));

   CheckGroup("DocWalker: refusals");
   CHECK(WalkedTo("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                  "<w:body/></w:document>") == WALK_OK);
   CHECK(WalkedTo("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == WALK_ERROR_ROOT);
   CHECK(WalkedTo("<w:other xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == WALK_ERROR_ROOT);
   CHECK(WalkedTo("") == WALK_ERROR_XML);
   CHECK(WalkedTo("<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                  "<w:body><w:p>") == WALK_ERROR_XML);
   // ISO 29500 Strict spells the namespace differently and must walk exactly the same code.
   CHECK(WalkedTo("<w:document xmlns:w=\"http://purl.oclc.org/ooxml/wordprocessingml/main\">"
                  "<w:body><w:p><w:r><w:t>x</w:t></w:r></w:p></w:body></w:document>") == WALK_OK);

   CheckGroup("DocWalker: the result sentences track their enumeration");

   WALK_STATUS root = {WALK_ERROR_ROOT, XML_OK, OPC_OK};
   WALK_STATUS none = {WALK_OK, XML_OK, OPC_OK};
   ui64        at   = 0;
   cchptr      said = DocWalkResultText(nullptr, root);
   cchptr      want = "the main document part\'s root element is not w:document";

   while(said[at] && said[at] == want[at]) ++at;
   CHECK(said[at] == want[at]);
   CHECK(DocWalkResultText(nullptr, none)[0] != 0);
}
