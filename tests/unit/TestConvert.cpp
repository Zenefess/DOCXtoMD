/*
 * File: TestConvert.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: Unit tests for the output-path derivation D7b's operand grammar rests on.
 * To Do: 1) Add a case per CONVERT_TARGET reason once M13's Batch owns the pre-flight loop.
 *        2) Take the media-directory cases back from TestMediaExtractor if a reader ever looks for them
 *           here: ConvertMediaDir is Convert's, but what it derives is the media layer's business.
 * Dependencies: BuildGuards.h, Check.h, Convert.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "Convert.h"

//-- Helpers

// Derives one output path and compares it with a literal. A null wanted means the derivation must fail.
static cbool DerivesTo(cwchptr input, cwchptr output, cbool directory, cwchptr wanted) {
   wchar produced[CONVERT_MAX_PATH];
   ui64  index = 0;

   cbool derived = ConvertOutputPath(input, output, directory, produced, CONVERT_MAX_PATH);

   if(!wanted) return !derived;
   if(!derived) return false;
   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// Runs the duplicate-target pre-flight over a whole input list and reports one input's verdict.
static cCONVERT_TARGET TargetOf(cwchptrptr inputs, cui32 count, cwchptr output, cui32 index) {
   CLI_OPTIONS options = {};

   options.inputs     = inputs;
   options.inputCount = count;
   options.outputPath = output;
   return ConvertTargetTaken(&options, index);
}

//== The suite

void TestConvert(void);

void TestConvert(void) {
   CheckGroup("Convert: an output path derived from the input");
   CHECK(DerivesTo(L"report.docx", nullptr, false, L"report.md"));
   CHECK(DerivesTo(L"folder\\report.docx", nullptr, false, L"folder\\report.md"));
   CHECK(DerivesTo(L"folder/report.docx", nullptr, false, L"folder/report.md"));
   CHECK(DerivesTo(L"C:\\a\\b\\report.docx", nullptr, false, L"C:\\a\\b\\report.md"));
   CHECK(DerivesTo(L"\\\\server\\share\\report.docx", nullptr, false, L"\\\\server\\share\\report.md"));
   CHECK(DerivesTo(L"\\\\?\\C:\\long\\report.docx", nullptr, false, L"\\\\?\\C:\\long\\report.md"));
   CHECK(DerivesTo(L"report", nullptr, false, L"report.md"));
   CHECK(DerivesTo(L"a.b.docx", nullptr, false, L"a.b.md"));
   CHECK(DerivesTo(L"C:report.docx", nullptr, false, L"C:report.md"));
   // A leaf that is nothing but an extension keeps its whole name: ".docx" is a file called ".docx".
   CHECK(DerivesTo(L".docx", nullptr, false, L".docx.md"));
   CHECK(DerivesTo(L"folder\\.docx", nullptr, false, L"folder\\.docx.md"));
   CHECK(DerivesTo(L"folder.with.dots\\report", nullptr, false, L"folder.with.dots\\report.md"));
   CHECK(DerivesTo(L"", nullptr, false, nullptr));
   CHECK(DerivesTo(nullptr, nullptr, false, nullptr));

   CheckGroup("Convert: -o naming a file, which is D7b's single-input case");
   CHECK(DerivesTo(L"report.docx", L"out.md", false, L"out.md"));
   CHECK(DerivesTo(L"report.docx", L"a\\b\\out.md", false, L"a\\b\\out.md"));
   // The value is used exactly as written, extension and all: it is a name and not a stem.
   CHECK(DerivesTo(L"report.docx", L"out.txt", false, L"out.txt"));
   CHECK(DerivesTo(L"report.docx", L"", false, nullptr));

   CheckGroup("Convert: -o naming a directory, which is D7b's several-input case");
   CHECK(DerivesTo(L"report.docx", L"out", true, L"out\\report.md"));
   CHECK(DerivesTo(L"a\\b\\report.docx", L"out", true, L"out\\report.md"));
   CHECK(DerivesTo(L"a/b/report.docx", L"out", true, L"out\\report.md"));
   CHECK(DerivesTo(L"report.docx", L"out\\", true, L"out\\report.md"));
   CHECK(DerivesTo(L"report.docx", L"out/", true, L"out/report.md"));
   CHECK(DerivesTo(L"report.docx", L"C:\\out", true, L"C:\\out\\report.md"));
   // A trailing colon is drive-relative: adding a separator there would name a different directory.
   CHECK(DerivesTo(L"report.docx", L"C:", true, L"C:report.md"));
   CHECK(DerivesTo(L"report.docx", L"", true, L"report.md"));

   CheckGroup("Convert: a trailing separator names a directory whatever the input count");
   // No Windows file name may end in a separator, so "-o out\\" with one input can only have meant a
   // directory. That is a refinement of D7d's by-input-count rule, not a departure from it.
   CHECK(DerivesTo(L"report.docx", L"out\\", false, L"out\\report.md"));
   CHECK(DerivesTo(L"a\\b\\report.docx", L"out/", false, L"out/report.md"));
   CHECK(DerivesTo(L"report.docx", L"out", false, L"out"));

   CheckGroup("Convert: a path that will not fit");
   wchar produced[8];

   CHECK(!ConvertOutputPath(L"report.docx", nullptr, false, produced, 8u));
   CHECK(!ConvertOutputPath(L"report.docx", nullptr, false, produced, 1u));
   CHECK(ConvertOutputPath(L"a.docx", nullptr, false, produced, 8u));

   CheckGroup("Convert: two inputs that would write one output file");
   // D7b derives every name from an input's own leaf, so two report.docx in two directories both
   // target one report.md. Argument order decides: the first keeps the path, the later ones are
   // refused, because converting both and keeping the second is silent data loss reported as success.
   cwchptr SAME_LEAF[]  = {L"p\\report.docx", L"q\\report.docx"};
   cwchptr DISTINCT[]   = {L"p\\report.docx", L"q\\summary.docx"};
   cwchptr OUTPUT_IN[]  = {L"c.docx", L"c.md"};
   cwchptr THREE_SAME[] = {L"a\\r.docx", L"b\\r.docx", L"c\\r.docx"};

   CHECK(TargetOf(SAME_LEAF, 2u, L"dst\\", 0) == CONVERT_TARGET_FREE);
   CHECK(TargetOf(SAME_LEAF, 2u, L"dst\\", 1) == CONVERT_TARGET_CLAIMED);
   CHECK(TargetOf(DISTINCT, 2u, L"dst\\", 0) == CONVERT_TARGET_FREE);
   CHECK(TargetOf(DISTINCT, 2u, L"dst\\", 1) == CONVERT_TARGET_FREE);
   // Only the first of three keeps the path; the second and third are both refused, not just one.
   CHECK(TargetOf(THREE_SAME, 3u, L"dst\\", 0) == CONVERT_TARGET_FREE);
   CHECK(TargetOf(THREE_SAME, 3u, L"dst\\", 1) == CONVERT_TARGET_CLAIMED);
   CHECK(TargetOf(THREE_SAME, 3u, L"dst\\", 2) == CONVERT_TARGET_CLAIMED);
   // c.docx derives c.md, which the run also lists as an input: converting it would destroy that
   // file before it is read, so the conversion is refused rather than the file lost.
   CHECK(TargetOf(OUTPUT_IN, 2u, nullptr, 0) == CONVERT_TARGET_IS_INPUT);
   // Nothing collides with itself: one input's own output is ConvertFile's case, with its own message.
   CHECK(TargetOf(DISTINCT, 1u, nullptr, 0) == CONVERT_TARGET_FREE);
   // One input named twice writes the same bytes over its own output, so it is not a collision.
   cwchptr TWICE[] = {L"p\\report.docx", L"p\\report.docx"};

   CHECK(TargetOf(TWICE, 2u, L"dst\\", 1) == CONVERT_TARGET_FREE);
   CHECK(TargetOf(TWICE, 2u, nullptr, 1) == CONVERT_TARGET_FREE);
   CHECK(ConvertTargetTaken(nullptr, 0) == CONVERT_TARGET_FREE);
   CHECK(TargetOf(SAME_LEAF, 2u, L"dst\\", 9) == CONVERT_TARGET_FREE);
}
