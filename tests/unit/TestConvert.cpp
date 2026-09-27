/*
 * File: TestConvert.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-27
 * Description: Unit tests for the output-path derivation D7b's operand grammar rests on, and the name comparison.
 * To Do: 1) Take the media-directory cases back from TestMediaExtractor if a reader ever looks for them
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

   CheckGroup("Convert: a drive letter's colon ends the directory part");
   // "C:report.docx" is report.docx in C:'s current directory, so -o naming a directory gains "report.md":
   // a colon in the leaf would be an NTFS alternate data stream on a file called "C".
   CHECK(DerivesTo(L"C:report.docx", L"out", true, L"out\\report.md"));
   CHECK(DerivesTo(L"c:sub\\report.docx", L"out", true, L"out\\report.md"));
   CHECK(DerivesTo(L"C:report.docx", nullptr, false, L"C:report.md"));
   // A name that is nothing but an extension still keeps it, drive or not.
   CHECK(DerivesTo(L"C:.docx", L"out", true, L"out\\.docx.md"));
   // Only a letter before the colon is a drive: anything else is part of the name.
   CHECK(DerivesTo(L"1:report.docx", L"out", true, L"out\\1:report.md"));

   CheckGroup("Convert: two paths are one name when they fold together as NTFS folds them");
   // CompareStringOrdinal's ignore-case form reads the system's upper-case table, so an accented or a
   // Cyrillic name differing only in case is one name -- which folding A to Z alone would miss.
   CHECK(ConvertSamePath(L"p\\Report.MD", L"P\\report.md"));
   CHECK(ConvertSamePath(L"\u00C9t\u00E9.md", L"\u00E9t\u00C9.md"));
   CHECK(ConvertSamePath(L"\u041E\u0442\u0447\u0451\u0442.md", L"\u043E\u0442\u0447\u0401\u0442.md"));
   CHECK(!ConvertSamePath(L"\u00E9.md", L"\u00E8.md"));
   CHECK(!ConvertSamePath(L"a.md", L"a.md.md"));
   // A string comparison, not an identity test: two spellings of one file are two names until normalised.
   CHECK(!ConvertSamePath(L".\\a.md", L"a.md"));
   CHECK(!ConvertSamePath(L"a\\b.md", L"a/b.md"));
}
