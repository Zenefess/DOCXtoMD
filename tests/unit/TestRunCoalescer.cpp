/*
 * File: TestRunCoalescer.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-26
 * Last Modified: 2026-08-27
 * Description: Unit tests for adjacent-run merging, whitespace hoisting and the order of the two.
 * To Do: 1) Add the hyperlink and field-result barriers when M7 and M10 stop a merge crossing one.
 *        2) Drive a document straight from IrAddSpan once a case needs a shape no body part produces.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, RunCoalescer.h, StyleModel.h, typedefs.h
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
#include "RunCoalescer.h"
#include "StyleModel.h"

//-- Helpers

// The three style parts the cases below reach for, named rather than repeated: a quote style, a
// code paragraph style and a code character style, each the shortest form that carries its role.
static constexpr cchptr STYLE_QUOTE = "<w:style w:type=\"paragraph\" w:styleId=\"Q\"><w:name w:val=\"Quote\"/></w:style>";
static constexpr cchptr STYLE_CODE  = "<w:style w:type=\"paragraph\" w:styleId=\"SC\"><w:name w:val=\"Source Code\"/></w:style>";
static constexpr cchptr STYLE_SPAN  = "<w:style w:type=\"character\" w:styleId=\"CC\"><w:name w:val=\"Code\"/></w:style>";

// The root element every body below is wrapped in, kept out of the helper so no line reaches the column
// limit once the formatter has joined what it can.
static constexpr cchptr COALESCE_HEAD = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
                                        " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
                                        " xmlns:v=\"urn:schemas-microsoft-com:vml\"><w:body>";
static constexpr cchptr COALESCE_TAIL = "</w:body></w:document>";

// The wrapper a styles part goes inside, for the cases that need one.
static constexpr cchptr STYLES_HEAD = "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
static constexpr cchptr STYLES_TAIL = "</w:styles>";

// Bytes before the terminator.
static cui64 CoalesceLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Appends a NUL-terminated literal to a buffer.
static void CoalesceAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr text) {
   cui64 length = CoalesceLength(text);

   for(ui64 index = 0; index < length && *used + 1u < destBytes; ++index) dest[(*used)++] = text[index];
   dest[*used] = 0;
}

// Renders the coalesced representation into one compact trace, in the notation TestDocWalker uses and
// two letters wider: c is a code span and the block letters are P, H<level>, Q, C and R. M7's span
// kinds render the same way here: L(dest) and L) for a link, I(source)[alt] for an image, N(name) for
// a bookmark anchor.
static void CoalesceTrace(cIR_DOCUMENTptr document, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   dest[0] = 0;
   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);
      char         head[8];
      ui64         at = 0;

      if(block->kind == IR_BLOCK_HEADING) {
         head[at++] = 'H';
         head[at++] = char('0' + block->headingLevel);
      } else if(block->kind == IR_BLOCK_QUOTE) head[at++] = 'Q';
      else if(block->kind == IR_BLOCK_CODE) head[at++] = 'C';
      else if(block->kind == IR_BLOCK_RULE) head[at++] = 'R';
      else head[at++] = 'P';
      head[at++] = '{';
      head[at]   = 0;
      CoalesceAppend(dest, destBytes, &used, head);
      for(ui32 span = 0; span < block->spanCount; ++span) {
         cIR_SPANptr one = IrSpanAt(document, block->spanAt + span);

         if(one->kind == IR_SPAN_BREAK) {
            CoalesceAppend(dest, destBytes, &used, "|");
            continue;
         }
         if(one->kind == IR_SPAN_LINK_END) {
            CoalesceAppend(dest, destBytes, &used, "L)");
            continue;
         }
         if(one->kind == IR_SPAN_LINK_START || one->kind == IR_SPAN_IMAGE || one->kind == IR_SPAN_ANCHOR) {
            CoalesceAppend(dest, destBytes, &used, (one->kind == IR_SPAN_IMAGE ? "I" : (one->kind == IR_SPAN_ANCHOR ? "N" : "L")));
            if(one->flags & IR_SPAN_FLAG_MUTE) CoalesceAppend(dest, destBytes, &used, "-");
            CoalesceAppend(dest, destBytes, &used, "(");
            for(ui32 byte = 0; byte < one->destBytes && used + 1u < destBytes; ++byte) {
               dest[used++] = IrDest(document, one->destAt)[byte];
            }
            dest[used] = 0;
            CoalesceAppend(dest, destBytes, &used, ")");
            if(one->kind != IR_SPAN_IMAGE) continue;
         }
         if(one->fmt & IR_FMT_BOLD) CoalesceAppend(dest, destBytes, &used, "b");
         if(one->fmt & IR_FMT_ITALIC) CoalesceAppend(dest, destBytes, &used, "i");
         if(one->fmt & IR_FMT_STRIKE) CoalesceAppend(dest, destBytes, &used, "s");
         if(one->fmt & IR_FMT_SUPER) CoalesceAppend(dest, destBytes, &used, "^");
         if(one->fmt & IR_FMT_SUB) CoalesceAppend(dest, destBytes, &used, "v");
         if(one->fmt & IR_FMT_CODE) CoalesceAppend(dest, destBytes, &used, "c");
         CoalesceAppend(dest, destBytes, &used, "[");
         for(ui32 byte = 0; byte < one->textBytes && used + 1u < destBytes; ++byte) {
            dest[used++] = IrText(document, one->textAt)[byte];
         }
         dest[used] = 0;
         CoalesceAppend(dest, destBytes, &used, "]");
      }
      CoalesceAppend(dest, destBytes, &used, "}");
   }
}

// Walks one body, coalesces it, and compares the trace with a literal.
static cbool CoalescesAs(cchptr styleBody, cchptr body, cchptr wanted) {
   char part[8192];
   char trace[2048];
   ui64 used = 0;

   part[0] = 0;
   CoalesceAppend(part, sizeof(part), &used, COALESCE_HEAD);
   CoalesceAppend(part, sizeof(part), &used, body);
   CoalesceAppend(part, sizeof(part), &used, COALESCE_TAIL);

   STYLE_MODEL styles;
   IR_DOCUMENT document;

   StyleOpen(&styles);
   if(styleBody) {
      char stylePart[4096];
      ui64 styleUsed = 0;

      stylePart[0] = 0;
      CoalesceAppend(stylePart, sizeof(stylePart), &styleUsed, STYLES_HEAD);
      CoalesceAppend(stylePart, sizeof(stylePart), &styleUsed, styleBody);
      CoalesceAppend(stylePart, sizeof(stylePart), &styleUsed, STYLES_TAIL);
      if(StyleLoadBytes(&styles, (cui8ptr)stylePart, styleUsed) != STYLE_OK) {
         StyleClose(&styles);
         return false;
      }
   }
   IrOpen(&document);

   cWALK_STATUS status = DocWalkBytes(&document, &styles, (cui8ptr)part, used);

   if(status.result != WALK_OK || !RunCoalesce(&document)) {
      IrClose(&document);
      StyleClose(&styles);
      return false;
   }
   CoalesceTrace(&document, trace, sizeof(trace));
   IrClose(&document);
   StyleClose(&styles);

   ui64 index = 0;

   while(trace[index] && trace[index] == wanted[index]) ++index;
   return trace[index] == wanted[index];
}

// The same with no styles part, which is what most cases want.
static cbool Coalesces(cchptr body, cchptr wanted) { return CoalescesAs(nullptr, body, wanted); }

//== The suite

void TestRunCoalescer(void);

void TestRunCoalescer(void) {
   CheckGroup("RunCoalescer: merging adjacent runs of equal formatting");
   // CONVERSION_REFERENCE 5.1: Word splits a logical run mid-word at every spellcheck and revision
   // boundary, and a delimiter per run would emit "**Hel****lo**", which is not emphasis at all.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Hel</w:t></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>lo</w:t></w:r></w:p>",
                   "P{b[Hello]}"));
   // A proofErr, a bookmark and an accepted insertion all fall between runs and none of them is a
   // barrier: after the accept-all pass the first and the last are not there at all, and the bookmark
   // is a span the merge sees straight through. It comes out after the merged text rather than in the
   // middle of it, because the merge extends the span in front of it -- a link resolves an anchor to
   // the block it stands in, so which end of the paragraph it settles at costs nothing.
   CHECK(Coalesces("<w:p><w:proofErr w:type=\"spellStart\"/><w:r><w:t>a</w:t></w:r>"
                   "<w:bookmarkStart w:id=\"1\" w:name=\"x\"/><w:r><w:t>b</w:t></w:r>"
                   "<w:ins w:id=\"2\" w:author=\"A\"><w:r><w:t>c</w:t></w:r></w:ins></w:p>",
                   "P{[abc]N(x)}"));
   // Unequal formatting is not merged, which is the other half of the rule.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>b</w:t></w:r></w:p>", "P{b[a]i[b]}"));
   // A hard break is a barrier: the two sides are different lines and cannot be one span.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "P{b[a]|b[b]}"));
   // A run carrying properties and no text contributes nothing, and must not separate two that merge.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t/></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>b</w:t></w:r></w:p>",
                   "P{b[ab]}"));
   // The complex-script twins share a bit, so three runs that render identically become one span.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>A</w:t></w:r><w:r><w:rPr><w:b/><w:bCs/></w:rPr><w:t>B</w:t></w:r>"
                   "<w:r><w:rPr><w:bCs/></w:rPr><w:t>C</w:t></w:r></w:p>",
                   "P{b[ABC]}"));

   CheckGroup("RunCoalescer: what a merge sees through and what stops it");
   // An anchor is the one span kind a merge reads straight past, and CONVERSION_REFERENCE 5.1 is why:
   // Word writes _GoBack in the middle of a paragraph, between two fragments of one word, and a merge
   // that stopped there would emit "**Hel****lo**" exactly as the run fragmentation itself does.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Hel</w:t></w:r>"
                   "<w:bookmarkStart w:id=\"1\" w:name=\"_GoBack\"/>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>lo</w:t></w:r></w:p>",
                   "P{b[Hello]N(_GoBack)}"));
   // Several of them in a row is the shape that matters, and not only because a producer writes it:
   // the anchors pile up behind the span every later run merges into, so a pass that looked for its
   // merge target by stepping back over them would be quadratic in the paragraph's own length.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:bookmarkStart w:id=\"1\" w:name=\"p\"/>"
                   "<w:r><w:t>b</w:t></w:r><w:bookmarkStart w:id=\"2\" w:name=\"q\"/>"
                   "<w:r><w:t>c</w:t></w:r><w:bookmarkStart w:id=\"3\" w:name=\"r\"/>"
                   "<w:r><w:t>d</w:t></w:r></w:p>",
                   "P{[abcd]N(p)N(q)N(r)}"));
   // An image is a barrier and an anchor behind one does not reopen the merge across it.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r>"
                   "<w:r><w:pict><v:shape alt=\"i\"><v:imagedata r:id=\"rId2\"/></v:shape></w:pict></w:r>"
                   "<w:bookmarkStart w:id=\"1\" w:name=\"p\"/><w:r><w:t>b</w:t></w:r></w:p>",
                   "P{[a]I(rId2)[i]N(p)[b]}"));
   // A link is the opposite case and must stop one: text on either side of a bracket is not adjacent
   // in the output, and joining it would carry bytes across a boundary the reader can see.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r>"
                   "<w:hyperlink r:id=\"rId5\"><w:r><w:t>b</w:t></w:r></w:hyperlink>"
                   "<w:r><w:t>c</w:t></w:r></w:p>",
                   "P{[a]L(rId5)[b]L)[c]}"));
   // Runs *inside* one link still merge, which is the half of 5.1 the brackets do not touch.
   CHECK(Coalesces("<w:p><w:hyperlink r:id=\"rId5\"><w:r><w:t>a</w:t></w:r>"
                   "<w:r><w:t>b</w:t></w:r></w:hyperlink></w:p>",
                   "P{L(rId5)[ab]L)}"));
   // Whitespace still hoists out of a formatted span inside a link, so the delimiter can parse.
   CHECK(Coalesces("<w:p><w:hyperlink r:id=\"rId5\"><w:r><w:rPr><w:b/></w:rPr>"
                   "<w:t xml:space=\"preserve\">a </w:t></w:r><w:r><w:t>b</w:t></w:r></w:hyperlink></w:p>",
                   "P{L(rId5)b[a][ b]L)}"));

   CheckGroup("RunCoalescer: hoisting whitespace out of a formatted span");
   // CONVERSION_REFERENCE 5.3: "**bold **text" does not parse, so the space moves outside the span.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">bold </w:t></w:r>"
                   "<w:r><w:t>text</w:t></w:r></w:p>",
                   "P{b[bold][ text]}"));
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\"> b</w:t></w:r></w:p>", "P{[a ]b[b]}"));
   // A span that is nothing but whitespace keeps its bytes and loses its formatting, which is 5.5.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t xml:space=\"preserve\"> </w:t></w:r>"
                   "<w:r><w:t>b</w:t></w:r></w:p>",
                   "P{[a b]}"));
   // The non-breaking space hoists like an ASCII one: CommonMark counts every Zs for flanking, so a
   // closing delimiter behind one may not parse. Note the asymmetry with IrEndBlock, where it is content.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">n&#160;</w:t></w:r>"
                   "<w:r><w:t>a</w:t></w:r></w:p>",
                   "P{b[n][\xC2\xA0"
                   "a]}"));
   // A tab hoists too, and both ends of one span hoist at once.
   CHECK(Coalesces("<w:p><w:r><w:t>x</w:t></w:r><w:r><w:rPr><w:b/></w:rPr>"
                   "<w:t xml:space=\"preserve\">&#9;b&#9;</w:t></w:r><w:r><w:t>y</w:t></w:r></w:p>",
                   "P{[x\t]b[b][\ty]}"));
   // An unformatted span is never hoisted from: there is no delimiter for its whitespace to escape.
   CHECK(Coalesces("<w:p><w:r><w:t xml:space=\"preserve\"> a </w:t></w:r></w:p>", "P{[ a ]}"));
   // The set is every Zs, not the ASCII pair and U+00A0 alone, because CommonMark's flanking rules do
   // not distinguish them: a delimiter written hard against an EN SPACE parses no better than one
   // against an ordinary space. U+2002 is one Insert-Symbol away in Word and U+3000 is what a CJK
   // keyboard's space bar produces, so neither is exotic.
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:b/></w:rPr>"
                   "<w:t xml:space=\"preserve\">&#8194;bold</w:t></w:r></w:p>",
                   "P{[a\xE2\x80\x82]b[bold]}"));
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">bold&#8194;</w:t></w:r>"
                   "<w:r><w:t>c</w:t></w:r></w:p>",
                   "P{b[bold][\xE2\x80\x82"
                   "c]}"));
   CHECK(Coalesces("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:i/></w:rPr>"
                   "<w:t xml:space=\"preserve\">&#12288;em&#8239;</w:t></w:r></w:p>",
                   "P{[a\xE3\x80\x80]i[em][\xE2\x80\xAF]}"));
   // U+200B is deliberately not in the set: it is Cf rather than Zs, and CommonMark does not count it.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>b&#8203;</w:t></w:r></w:p>", "P{b[b\xE2\x80\x8B]}"));

   CheckGroup("RunCoalescer: merging happens before hoisting");
   // This is the whole reason the two passes are ordered. Merged first, the pair is one bold span with
   // no whitespace at either end; hoisted first, it would come apart into "**one** **two**".
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">one </w:t></w:r>"
                   "<w:r><w:rPr><w:b/></w:rPr><w:t>two</w:t></w:r></w:p>",
                   "P{b[one two]}"));
   // And the whitespace hoisted out of two neighbours becomes one span again rather than two.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">x </w:t></w:r>"
                   "<w:r><w:rPr><w:i/></w:rPr><w:t xml:space=\"preserve\"> y</w:t></w:r></w:p>",
                   "P{b[x][  ]i[y]}"));

   CheckGroup("RunCoalescer: the block kinds it must leave alone");
   // Nothing is hoisted inside a fenced block: its whitespace is the indentation of the code. The span
   // has to *carry* formatting for this to assert anything -- an unformatted one returns before the
   // guard is read, so the case would pass with the guard deleted.
   CHECK(CoalescesAs(STYLE_CODE,
                     "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"
                     "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr>"
                     "<w:t xml:space=\"preserve\">   indented   </w:t></w:r></w:p>",
                     "C{c[   indented   ]}"));
   CHECK(CoalescesAs(STYLE_CODE,
                     "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"
                     "<w:r><w:t xml:space=\"preserve\">   indented   </w:t></w:r></w:p>",
                     "C{[   indented   ]}"));
   // A rule carries no spans at all, and an empty code paragraph is kept although it holds none either.
   CHECK(Coalesces("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "R{}"));
   CHECK(CoalescesAs(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>", "C{}"));
   // A quote is an ordinary paragraph as far as this pass is concerned, prefix and all.
   CHECK(CoalescesAs(STYLE_QUOTE,
                     "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr>"
                     "<w:r><w:rPr><w:b/></w:rPr><w:t xml:space=\"preserve\">q </w:t></w:r>"
                     "<w:r><w:t>r</w:t></w:r></w:p>",
                     "Q{b[q][ r]}"));

   CheckGroup("RunCoalescer: code spans and the two halves of row 11");
   // Row 11 drops bold and italic from a code run, and the bits are cleared in the *walker* so that two
   // runs which come out as the same code span merge here. Left set, their two backtick delimiters meet
   // and a renderer reads the pair as one span with backticks in it. Nothing else asserts the rule: it
   // is invisible at emission, so its only observable effect is this merge.
   CHECK(CoalescesAs(STYLE_SPAN,
                     "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/><w:b/></w:rPr><w:t>co</w:t></w:r>"
                     "<w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>de</w:t></w:r></w:p>",
                     "P{c[code]}"));
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/><w:i/></w:rPr><w:t>co</w:t></w:r>"
                   "<w:r><w:rPr><w:rFonts w:ascii=\"Menlo\"/></w:rPr><w:t>de</w:t></w:r>"
                   "<w:r><w:t> and prose</w:t></w:r></w:p>",
                   "P{c[code][ and prose]}"));
   // A run that is code because of its character style and one that is code because of its font carry
   // the same bit, so two of them side by side are one code span rather than two.
   CHECK(CoalescesAs(STYLE_SPAN,
                     "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>co</w:t></w:r>"
                     "<w:r><w:rPr><w:rFonts w:ascii=\"Menlo\"/></w:rPr><w:t>de</w:t></w:r></w:p>",
                     "P{c[code]}"));
   // Whitespace hoists out of a code span too: CommonMark strips one pad space of its own, so leaving
   // it inside would change the content depending on what stood at the other end.
   CHECK(CoalescesAs(STYLE_SPAN,
                     "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t xml:space=\"preserve\">run </w:t></w:r>"
                     "<w:r><w:t>after</w:t></w:r></w:p>",
                     "P{c[run][ after]}"));
   // Every run monospace makes the paragraph a fenced block, and one run that is not leaves it a
   // paragraph holding a code span -- which is CONVERSION_REFERENCE row 12 against row 11.
   CHECK(Coalesces("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a();</w:t></w:r></w:p>", "C{c[a();]}"));
   CHECK(Coalesces("<w:p><w:r><w:t xml:space=\"preserve\">x </w:t></w:r>"
                   "<w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a();</w:t></w:r></w:p>",
                   "P{[x ]c[a();]}"));
}
