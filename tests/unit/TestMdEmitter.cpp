/*
 * File: TestMdEmitter.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: Unit tests for the arena, the blank-line discipline, delimiters and every block kind.
 * To Do: 1) Add the link and image cases when M7 gives the emitter a destination to write.
 *        2) Add the pipe-table cases when M9 gives a cell a line of its own.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, MdEmitter.h, RunCoalescer.h,
 *               StyleModel.h, typedefs.h
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
#include "MdEmitter.h"
#include "RunCoalescer.h"
#include "StyleModel.h"

//-- Helpers

// The three style parts the cases below reach for, named rather than repeated: a quote style, a
// code paragraph style and a code character style, each the shortest form that carries its role.
static constexpr cchptr STYLE_QUOTE = "<w:style w:type=\"paragraph\" w:styleId=\"Q\"><w:name w:val=\"Quote\"/></w:style>";
static constexpr cchptr STYLE_CODE  = "<w:style w:type=\"paragraph\" w:styleId=\"SC\"><w:name w:val=\"Source Code\"/></w:style>";
// Property bags and a paragraph reference the cases below carry often enough that spelling them
// out lands a line past e2's 150 columns once the formatter has joined what it can.
#define SUPER      "<w:vertAlign w:val=\"superscript\"/>"
#define SUB        "<w:vertAlign w:val=\"subscript\"/>"
#define CODE_STYLE "<w:rStyle w:val=\"CC\"/>"
#define IN_CODE    "<w:pPr><w:pStyle w:val=\"SC\"/></w:pPr>"

static constexpr cchptr STYLE_SPAN = "<w:style w:type=\"character\" w:styleId=\"CC\"><w:name w:val=\"Code\"/></w:style>";

// The root element every body below is wrapped in, kept out of the helper so no line reaches the
// column limit once the formatter has joined what it can.
static constexpr cchptr EMIT_HEAD = "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"><w:body>";
static constexpr cchptr EMIT_TAIL = "</w:body></w:document>";

// Bytes before the terminator.
static cui64 EmitLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Whether an emitter's bytes are exactly a literal, terminator excluded.
static cbool EmittedIs(cMD_EMITTERptr emitter, cchptr wanted) {
   cui64 length = EmitLength(wanted);

   if(MdByteCount(emitter) != length) return false;
   for(ui64 index = 0; index < length; ++index) {
      if(MdBytes(emitter)[index] != wanted[index]) return false;
   }
   return true;
}

// Adds one text span carrying a NUL-terminated literal.
static cbool EmitText(IR_DOCUMENTptrc document, cchptr text) {
   if(!IrAddSpan(document, IR_SPAN_TEXT, IR_FMT_NONE)) return false;
   return IrAppendText(document, text, EmitLength(text));
}

// Converts one body, with an optional styles part in front of it, straight through to Markdown.
static cbool ConvertsWith(cchptr styleBody, cchptr body, cchptr wanted, cHARD_BREAK hardBreak) {
   char part[4096];
   ui64 used = 0;

   cchptr pieces[3] = {EMIT_HEAD, body, EMIT_TAIL};

   for(ui32 index = 0; index < 3u; ++index) {
      cui64 length = EmitLength(pieces[index]);

      for(ui64 at = 0; at < length && used + 1u < sizeof(part); ++at) part[used++] = pieces[index][at];
   }
   part[used] = 0;

   STYLE_MODEL styles;
   IR_DOCUMENT document;
   MD_EMITTER  emitter;

   StyleOpen(&styles);
   if(styleBody) {
      char stylePart[2048];
      ui64 styleUsed = 0;

      cchptr wrap[3] = {"<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">", styleBody, "</w:styles>"};

      for(ui32 index = 0; index < 3u; ++index) {
         cui64 length = EmitLength(wrap[index]);

         for(ui64 at = 0; at < length && styleUsed + 1u < sizeof(stylePart); ++at) stylePart[styleUsed++] = wrap[index][at];
      }
      stylePart[styleUsed] = 0;
      if(StyleLoadBytes(&styles, (cui8ptr)stylePart, styleUsed) != STYLE_OK) {
         StyleClose(&styles);
         return false;
      }
   }
   IrOpen(&document);
   MdOpen(&emitter, hardBreak);

   cWALK_STATUS status  = DocWalkBytes(&document, &styles, (cui8ptr)part, used);
   bool         matched = false;

   // The coalescing pass stands between the walk and the emitter in Convert.cpp, so it stands there
   // here too: the emitter's contract since M6 is that every formatted span it is handed is already
   // merged and already trimmed, and testing it against an unmerged document would test a shape the
   // program never produces.
   if(status.result == WALK_OK && RunCoalesce(&document) && MdEmitDocument(&emitter, &document) == MD_OK) {
      matched = EmittedIs(&emitter, wanted);
   }
   MdClose(&emitter);
   IrClose(&document);
   StyleClose(&styles);
   return matched;
}

// Converts one body with no styles part, under a chosen hard-break policy.
static cbool ConvertsTo(cchptr body, cchptr wanted, cHARD_BREAK hardBreak) { return ConvertsWith(nullptr, body, wanted, hardBreak); }

// Converts one body with the default hard-break policy, which is what all but one case wants.
static cbool Converts(cchptr body, cchptr wanted) { return ConvertsTo(body, wanted, HARD_BREAK_BACKSLASH); }

// Converts one body against a styles part, which is the only way to reach a quote or a code block.
static cbool Styled(cchptr styleBody, cchptr body, cchptr wanted) { return ConvertsWith(styleBody, body, wanted, HARD_BREAK_BACKSLASH); }

//== The suite

void TestMdEmitter(void);

void TestMdEmitter(void) {
   CheckGroup("Ir: blocks, spans and the arena");
   IR_DOCUMENT document;

   IrOpen(&document);
   CHECK(IrBlockCount(&document) == 0);
   CHECK(IrBlockAt(&document, 0) == nullptr);
   CHECK(IrSpanAt(&document, 0) == nullptr);
   CHECK(!IrFailed(&document));

   cIR_MARK first = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(first.block == 0);
   CHECK(EmitText(&document, "hello"));
   CHECK(IrEndBlock(&document, first));
   CHECK(IrBlockCount(&document) == 1u);
   CHECK(IrBlockAt(&document, 0)->spanCount == 1u);
   CHECK(IrSpanAt(&document, 0)->textBytes == 5u);

   // A block holding nothing but ASCII whitespace is unwound completely, arena included.
   cui64    before = document.heapUsed;
   cIR_MARK blank  = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "   "));
   CHECK(!IrEndBlock(&document, blank));
   CHECK(IrBlockCount(&document) == 1u);
   CHECK(document.heapUsed == before);

   // A non-breaking space is content, and a paragraph written to hold one keeps it.
   cIR_MARK nbsp = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "\xC2\xA0"));
   CHECK(IrEndBlock(&document, nbsp));
   CHECK(IrBlockCount(&document) == 2u);

   // A mark taken between blocks unwinds every block added after it.
   cIR_MARK here  = IrMark(&document);
   cIR_MARK extra = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "gone"));
   CHECK(IrEndBlock(&document, extra));
   CHECK(IrBlockCount(&document) == 3u);
   IrRewind(&document, here);
   CHECK(IrBlockCount(&document) == 2u);
   IrClose(&document);
   CHECK(IrBlockCount(&document) == 0);

   CheckGroup("MdEmitter: the blank-line discipline");
   MD_EMITTER emitter;

   IrOpen(&document);
   MdOpen(&emitter, HARD_BREAK_BACKSLASH);
   CHECK(MdEmitDocument(&emitter, &document) == MD_OK);
   CHECK(MdByteCount(&emitter) == 0);

   cIR_MARK one = IrBeginBlock(&document, IR_BLOCK_PARAGRAPH, 0);

   CHECK(EmitText(&document, "a"));
   CHECK(IrEndBlock(&document, one));
   CHECK(MdEmitDocument(&emitter, &document) == MD_OK);
   CHECK(EmittedIs(&emitter, "a\n"));
   MdClose(&emitter);

   cIR_MARK two = IrBeginBlock(&document, IR_BLOCK_HEADING, 2u);

   CHECK(EmitText(&document, "b"));
   CHECK(IrEndBlock(&document, two));
   MdOpen(&emitter, HARD_BREAK_BACKSLASH);
   CHECK(MdEmitDocument(&emitter, &document) == MD_OK);
   CHECK(EmittedIs(&emitter, "a\n\n## b\n"));
   MdClose(&emitter);
   IrClose(&document);

   CheckGroup("MdEmitter: paragraphs, trimming and line starts");
   CHECK(Converts("<w:p><w:r><w:t>plain</w:t></w:r></w:p>", "plain\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">  padded  </w:t></w:r></w:p>", "padded\n"));
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">  a</w:t></w:r><w:r><w:t xml:space=\"preserve\">b  </w:t></w:r></w:p>", "ab\n"));
   CHECK(Converts("<w:p><w:r><w:t># head</w:t></w:r></w:p>", "\\# head\n"));
   CHECK(Converts("<w:p><w:r><w:t>1. item</w:t></w:r></w:p>", "1\\. item\n"));
   CHECK(Converts("<w:p><w:r><w:t>a*b</w:t></w:r></w:p>", "a\\*b\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r></w:p><w:p><w:r><w:t>b</w:t></w:r></w:p>", "a\n\nb\n"));

   CheckGroup("MdEmitter: hard breaks");
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "a\\\nb\n"));
   CHECK(ConvertsTo("<w:p><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "a  \nb\n", HARD_BREAK_SPACES));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>- b</w:t></w:r></w:p>", "a\\\n\\- b\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:br/><w:t>b</w:t></w:r></w:p>", "a\\\nb\n"));
   CHECK(Converts("<w:p><w:r><w:t>a\\</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "a\\\\\\\nb\n"));
   // A break whose next line turns out to hold nothing but padding is dropped rather than left dangling.
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>", "a\n"));

   CheckGroup("MdEmitter: the whole output obeys its own invariants");
   {
      IR_DOCUMENT many;
      MD_EMITTER  writer;

      IrOpen(&many);
      for(ui32 index = 0; index < 6u; ++index) {
         cIR_MARK at = IrBeginBlock(&many, (index & 1u ? IR_BLOCK_HEADING : IR_BLOCK_PARAGRAPH), ui8(index & 1u ? 3u : 0u));

         CHECK(EmitText(&many, "  text  "));
         CHECK(IrEndBlock(&many, at));
      }
      MdOpen(&writer, HARD_BREAK_BACKSLASH);
      CHECK(MdEmitDocument(&writer, &many) == MD_OK);
      CHECK(MdByteCount(&writer) > 0);
      CHECK(MdBytes(&writer)[0] != '\n');
      CHECK(MdBytes(&writer)[MdByteCount(&writer) - 1u] == '\n');

      bool clean = true;

      for(ui64 index = 0; index < MdByteCount(&writer); ++index) {
         cchar byte = MdBytes(&writer)[index];

         if(byte == '\r' || !byte) clean = false;
         if(byte == '\n' && index + 2u < MdByteCount(&writer) && MdBytes(&writer)[index + 1u] == '\n' && MdBytes(&writer)[index + 2u] == '\n') {
            clean = false;
         }
         if(index + 1u == MdByteCount(&writer)) continue;
         if(byte == ' ' && MdBytes(&writer)[index + 1u] == '\n') clean = false;
      }
      CHECK(clean);
      MdClose(&writer);
      IrClose(&many);
   }

   CheckGroup("MdEmitter: a line is escaped once, not span by span");
   // Word fragments a run wherever a revision or spellcheck boundary falls, so the output must not
   // depend on where the split landed. Both spellings of the same text produce the same bytes.
   CHECK(Converts("<w:p><w:r><w:t>A&amp;amp;B</w:t></w:r></w:p>", "A&amp;amp;B\n"));
   CHECK(Converts("<w:p><w:r><w:t>A&amp;</w:t></w:r><w:r><w:t>amp;B</w:t></w:r></w:p>", "A&amp;amp;B\n"));
   CHECK(Converts("<w:p><w:r><w:t>AT&amp;T</w:t></w:r></w:p>", "AT&T\n"));
   CHECK(Converts("<w:p><w:r><w:t>AT&amp;</w:t></w:r><w:r><w:t>T</w:t></w:r></w:p>", "AT&T\n"));
   // The same for a construct the line-start pass judges, which needs the whole line to judge it.
   CHECK(Converts("<w:p><w:r><w:t>19</w:t></w:r><w:r><w:t>98. a year</w:t></w:r></w:p>", "1998\\. a year\n"));
   CHECK(Converts("<w:p><w:r><w:t>-</w:t></w:r><w:r><w:t>--</w:t></w:r></w:p>", "\\---\n"));
   CHECK(Converts("<w:p><w:r><w:t>a&lt;</w:t></w:r><w:r><w:t>b&gt;</w:t></w:r></w:p>", "a\\<b>\n"));
   CHECK(Converts("<w:p><w:r><w:t>a&lt;b&gt;</w:t></w:r></w:p>", "a\\<b>\n"));

   CheckGroup("MdEmitter: a setext underline needs a line above it");
   CHECK(Converts("<w:p><w:r><w:t>===</w:t></w:r></w:p>", "===\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>===</w:t></w:r></w:p>", "a\\\n\\===\n"));
   CHECK(Converts("<w:p><w:r><w:t>--</w:t></w:r></w:p>", "--\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t><w:br/><w:t>--</w:t></w:r></w:p>", "a\\\n\\--\n"));

   CheckGroup("MdEmitter: headings");
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "# t\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"5\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "###### t\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"8\"/></w:pPr><w:r><w:t>t</w:t></w:r></w:p>", "###### t\n"));
   // A heading is one line, so a break inside it becomes a space, and its content is not a line start.
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "# a b\n"));
   // Exactly one space, whatever padding stood on either side of the break: two would render as one
   // and would only put an invisible difference into a file that is compared byte for byte.
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
                  "<w:r><w:t xml:space=\"preserve\">a </w:t><w:br/>"
                  "<w:t xml:space=\"preserve\"> b</w:t></w:r></w:p>",
                  "# a b\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>- b</w:t></w:r></w:p>", "# - b\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>Sharp #</w:t></w:r></w:p>", "# Sharp \\#\n"));
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr><w:r><w:t>C#</w:t></w:r></w:p>", "# C#\n"));

   CheckGroup("MdEmitter: the inline delimiters");
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r></w:p>", "**a**\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:i/></w:rPr><w:t>a</w:t></w:r></w:p>", "*a*\n"));
   // Mapping row 5 fixes the order, so no document depends on which of the two a producer named first.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>a</w:t></w:r></w:p>", "***a***\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:i/><w:b/></w:rPr><w:t>a</w:t></w:r></w:p>", "***a***\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:strike/></w:rPr><w:t>a</w:t></w:r></w:p>", "~~a~~\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:dstrike/></w:rPr><w:t>a</w:t></w:r></w:p>", "~~a~~\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "<sup>a</sup>\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:vertAlign w:val=\"subscript\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "<sub>a</sub>\n"));
   // The nesting is fixed outermost first: the HTML wrapper, then the strike, then the emphasis.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:i/><w:strike/></w:rPr><w:t>a</w:t></w:r></w:p>", "<del>***a***</del>\n"));
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:strike/>" SUPER "</w:rPr><w:t>a</w:t></w:r></w:p>", "<sup><del>**a**</del></sup>\n"));
   // Underline, highlight and colour have no Markdown equivalent and are dropped (rows 8 and 9).
   CHECK(Converts("<w:p><w:r><w:rPr><w:u w:val=\"single\"/><w:highlight w:val=\"yellow\"/></w:rPr><w:t>a</w:t></w:r></w:p>", "a\n"));
   // Inside a raw-HTML wrapper the text still needs its two entities, which no other context adds.
   CHECK(Converts("<w:p><w:r><w:rPr>" SUB "</w:rPr><w:t>a &amp; &lt;b&gt;</w:t></w:r></w:p>", "<sub>a &amp; &lt;b></sub>\n"));
   // Two adjacent formatted spans put their delimiter runs side by side, which CommonMark resolves the
   // way the source meant: "**bo**" then "***ld***" is bold then bold-italic, not one run of five.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bo</w:t></w:r>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>ld</w:t></w:r></w:p>",
                  "**bo*****ld***\n"));
   // A literal asterisk inside a formatted span is escaped, so it cannot join the emitted delimiter.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>*a*</w:t></w:r></w:p>", "**\\*a\\***\n"));
   // A heading's bold is style-borne (row 1), but its italic is not and survives.
   CHECK(Converts("<w:p><w:pPr><w:outlineLvl w:val=\"0\"/></w:pPr>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>a</w:t></w:r></w:p>",
                  "# *a*\n"));

   CheckGroup("MdEmitter: the flanking fallback to raw HTML");
   // CommonMark refuses a delimiter run that is both preceded by a letter and followed by punctuation,
   // and the mirror image at the closing end -- so "word**(a)**after" would emit four literal asterisks
   // and lose the emphasis. An HTML element has no flanking rule at all, so it takes over there.
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>(a)</w:t></w:r>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "word<strong>(a)</strong>after\n"));
   // A space on either side is all it takes for the Markdown spelling to be safe again, which is why
   // the fallback is rare: it needs punctuation at the very edge of the span and no space outside it.
   CHECK(Converts("<w:p><w:r><w:t xml:space=\"preserve\">word </w:t></w:r><w:r><w:rPr><w:b/></w:rPr><w:t>(a)</w:t></w:r>"
                  "<w:r><w:t xml:space=\"preserve\"> after</w:t></w:r></w:p>",
                  "word **(a)** after\n"));
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r><w:r><w:rPr><w:i/></w:rPr><w:t>a</w:t></w:r>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "word*a*after\n"));
   // A strikethrough is the one that cannot survive wrapping anything: its opening "~~" is then always
   // followed by punctuation, so a letter in front of it is enough on its own.
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r><w:r><w:rPr><w:b/><w:strike/></w:rPr><w:t>struck</w:t></w:r>"
                  "<w:r><w:t>after</w:t></w:r></w:p>",
                  "word<del>**struck**</del>after\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:strike/></w:rPr><w:t>#hash</w:t></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "a<del>#hash</del>b\n"));
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r><w:r><w:rPr><w:strike/></w:rPr><w:t>plain</w:t></w:r>"
                  "<w:r><w:t>b</w:t></w:r></w:p>",
                  "a~~plain~~b\n"));
   // Two delimiter runs that meet are one run to a parser, so what stands in front of the first is what
   // the flanking rules look at: "***T***" then "**=eq=**" is a run of five preceded by a letter.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>T</w:t></w:r>"
                  "<w:r><w:rPr><w:b/></w:rPr><w:t>=eq=</w:t></w:r></w:p>",
                  "***T***<strong>=eq=</strong>\n"));
   // The same shape with no punctuation at the seam needs no fallback, and CommonMark reads the run of
   // five the way the source meant it.
   CHECK(Converts("<w:p><w:r><w:rPr><w:b/></w:rPr><w:t>bo</w:t></w:r>"
                  "<w:r><w:rPr><w:b/><w:i/></w:rPr><w:t>ld</w:t></w:r></w:p>",
                  "**bo*****ld***\n"));
   // An element already shields whatever is inside it, so a superscript never needs the fallback.
   CHECK(Converts("<w:p><w:r><w:t>word</w:t></w:r>"
                  "<w:r><w:rPr><w:b/>" SUPER "</w:rPr><w:t>(a)</w:t></w:r><w:r><w:t>after</w:t></w:r></w:p>",
                  "word<sup>**(a)**</sup>after\n"));
   // A code span has no flanking rule of its own, so it keeps its backticks wherever it stands.
   CHECK(Styled(STYLE_SPAN,
                "<w:p><w:r><w:t>word</w:t></w:r>"
                "<w:r><w:rPr>" CODE_STYLE "</w:rPr><w:t>(a)</w:t></w:r><w:r><w:t>after</w:t></w:r></w:p>",
                "word`(a)`after\n"));

   CheckGroup("MdEmitter: code spans and their delimiter sizing");
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "`x`\n"));
   // A backtick inside a code span cannot be escaped, so the delimiter grows past the longest run.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>a`b</w:t></w:r></w:p>", "``a`b``\n"));
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>a``b</w:t></w:r></w:p>", "```a``b```\n"));
   // Content that begins or ends with a backtick is padded, and CommonMark strips exactly that pair.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>`x</w:t></w:r></w:p>", "`` `x ``\n"));
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>x`</w:t></w:r></w:p>", "`` x` ``\n"));
   // Nothing at all is escaped inside a code span.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:rStyle w:val=\"CC\"/></w:rPr><w:t>*a* &amp; &lt;b&gt;</w:t></w:r></w:p>", "`*a* & <b>`\n"));
   // Row 11's ruling on the collision: code drops bold and italic, and keeps what wraps it cleanly.
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:b/><w:i/><w:rStyle w:val=\"CC\"/></w:rPr><w:t>x</w:t></w:r></w:p>", "`x`\n"));
   CHECK(Styled(STYLE_SPAN, "<w:p><w:r><w:rPr><w:strike/>" SUPER CODE_STYLE "</w:rPr><w:t>x</w:t></w:r></w:p>", "<sup><del>`x`</del></sup>\n"));

   CheckGroup("MdEmitter: fenced code blocks");
   CHECK(Styled(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "```\na\n```\n"));
   // Consecutive code paragraphs merge into one fence, and the leading whitespace is the indentation.
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t xml:space=\"preserve\">  b</w:t></w:r></w:p>",
                "```\na\n  b\n```\n"));
   // An empty code paragraph is a blank line of the fence, and nothing at either end of one.
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>",
                "```\na\n\nb\n```\n"));
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>",
                "```\na\n```\n"));
   // A fence of nothing but blank code paragraphs is no fence at all, and leaves no separator behind.
   CHECK(Styled(STYLE_CODE,
                "<w:p><w:r><w:t>x</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr></w:p>"
                "<w:p><w:r><w:t>y</w:t></w:r></w:p>",
                "x\n\ny\n"));
   // The fence is longer than the longest backtick run inside it, or the content would close it.
   CHECK(Styled(STYLE_CODE, "<w:p><w:pPr><w:pStyle w:val=\"SC\"/></w:pPr><w:r><w:t>a ``` b</w:t></w:r></w:p>", "````\na ``` b\n````\n"));
   // Nothing is escaped in a fence, and a hard break inside one is simply the next line.
   CHECK(Styled(STYLE_CODE, "<w:p>" IN_CODE "<w:r><w:t>*a*</w:t><w:br/><w:t># b</w:t></w:r></w:p>", "```\n*a*\n# b\n```\n"));
   // Row 12's second detection: every run monospace makes the paragraph a fence with no style at all.
   CHECK(Converts("<w:p><w:r><w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr><w:t>a();</w:t></w:r></w:p>", "```\na();\n```\n"));

   CheckGroup("MdEmitter: blockquotes");
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "> a\n"));
   // Every line of the block takes the prefix, the continuation line included.
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t><w:br/><w:t>b</w:t></w:r></w:p>", "> a\\\n> b\n"));
   // Two consecutive quote paragraphs are one quotation: a blank line would close the blockquote and
   // open a second, so the separator is a bare marker instead. That is the one exception to the rule.
   CHECK(Styled(STYLE_QUOTE,
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>",
                "> a\n>\n> b\n"));
   // A paragraph between them really does end the quotation.
   CHECK(Styled(STYLE_QUOTE,
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>a</w:t></w:r></w:p>"
                "<w:p><w:r><w:t>x</w:t></w:r></w:p>"
                "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>b</w:t></w:r></w:p>",
                "> a\n\nx\n\n> b\n"));
   // The line-start pass runs on the content after the prefix, not on the marker the emitter wrote.
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:t>- a</w:t></w:r></w:p>", "> \\- a\n"));
   CHECK(Styled(STYLE_QUOTE, "<w:p><w:pPr><w:pStyle w:val=\"Q\"/></w:pPr><w:r><w:rPr><w:b/></w:rPr><w:t>a</w:t></w:r></w:p>", "> **a**\n"));

   CheckGroup("MdEmitter: horizontal rules");
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "---\n"));
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:between w:val=\"single\"/></w:pBdr></w:pPr></w:p>", "---\n"));
   // The blank lines row 25 asks for on either side are the block separator's own doing, and they are
   // what keeps the rule from being read as a setext underline for the paragraph above it.
   CHECK(Converts("<w:p><w:r><w:t>a</w:t></w:r></w:p>"
                  "<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>"
                  "<w:p><w:r><w:t>b</w:t></w:r></w:p>",
                  "a\n\n---\n\nb\n"));
   // "Lone" is enforced at both ends: a box is not a rule, and neither is a bordered paragraph of text.
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:top w:val=\"single\"/><w:bottom w:val=\"single\"/></w:pBdr></w:pPr></w:p>", ""));
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"none\"/></w:pBdr></w:pPr></w:p>", ""));
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr><w:r><w:t>a</w:t></w:r></w:p>", "a\n"));
   // A paragraph of nothing but whitespace still came to nothing, so its border still means a rule.
   CHECK(Converts("<w:p><w:pPr><w:pBdr><w:bottom w:val=\"single\"/></w:pBdr></w:pPr>"
                  "<w:r><w:t xml:space=\"preserve\">   </w:t></w:r></w:p>",
                  "---\n"));
}
