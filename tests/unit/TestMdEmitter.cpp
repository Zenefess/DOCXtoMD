/*
 * File: TestMdEmitter.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: Unit tests for the arena, the blank-line discipline, line assembly and hard breaks.
 * To Do: 1) Add the delimiter cases when M6 gives the emitter something to wrap a span in.
 *        2) Add the blockquote prefix cases when M6 detects a quote style.
 * Dependencies: BuildGuards.h, Check.h, DocWalker.h, Ir.h, MdEmitter.h, StyleModel.h, typedefs.h
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
#include "StyleModel.h"

//-- Helpers

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

// Converts one body, with no styles part, straight through to Markdown.
static cbool ConvertsTo(cchptr body, cchptr wanted, cHARD_BREAK hardBreak) {
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
   IrOpen(&document);
   MdOpen(&emitter, hardBreak);

   cWALK_STATUS status  = DocWalkBytes(&document, &styles, (cui8ptr)part, used);
   bool         matched = false;

   if(status.result == WALK_OK && MdEmitDocument(&emitter, &document) == MD_OK) matched = EmittedIs(&emitter, wanted);
   MdClose(&emitter);
   IrClose(&document);
   StyleClose(&styles);
   return matched;
}

// Converts one body with the default hard-break policy, which is what all but one case wants.
static cbool Converts(cchptr body, cchptr wanted) { return ConvertsTo(body, wanted, HARD_BREAK_BACKSLASH); }

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
}
