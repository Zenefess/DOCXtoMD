/*
 * File: TestMediaExtractor.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: The content-type extension table and the media-directory derivation, from string literals.
 * To Do: 1) Drive MediaPlan itself once a package can be built without an archive.
 *        2) Add the audio and video types once anything in the converter emits one.
 * Dependencies: BuildGuards.h, Check.h, Convert.h, MediaExtractor.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"
#include "Convert.h"
#include "MediaExtractor.h"

//-- Helpers

// Whether two NUL-terminated strings are the same bytes.
static cbool MediaTestSame(cchptr produced, cchptr wanted) {
   ui64 index = 0;

   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// The same for wide strings, which is what a derived directory is.
static cbool MediaTestSameWide(cwchptr produced, cwchptr wanted) {
   ui64 index = 0;

   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// Whether one content type and part name together name the extension they should.
static cbool ExtendsTo(cchptr contentType, cchptr partName, cchptr wanted) {
   char extension[32];

   MediaExtension(contentType, partName, extension, sizeof(extension));
   return MediaTestSame(extension, wanted);
}

// Whether one document path and --media-dir value derive the directory and the path they should.
static cbool SitesAt(cwchptr documentPath, cwchptr named, cwchptr wantedDir, cchptr wantedPrefix) {
   wchar dir[CONVERT_MAX_PATH];
   char  prefix[CONVERT_MAX_PATH];

   if(!ConvertMediaDir(documentPath, named, dir, CONVERT_MAX_PATH, prefix, CONVERT_MAX_PATH)) return false;
   return MediaTestSameWide(dir, wantedDir) && MediaTestSame(prefix, wantedPrefix);
}

//== The suite

void TestMediaExtractor(void);

void TestMediaExtractor(void) {
   CheckGroup("MediaExtractor: the extension a content type names");
   // CONVERSION_REFERENCE 1.2: an image part's true type comes from [Content_Types].xml and never from
   // the extension its ZIP entry name happens to carry, which producers get wrong -- Google Docs in
   // particular has emitted media whose name and content disagree.
   CHECK(ExtendsTo("image/png", "word/media/image1.bin", ".png"));
   CHECK(ExtendsTo("image/jpeg", "word/media/image1.png", ".jpg"));
   CHECK(ExtendsTo("image/gif", "word/media/x", ".gif"));
   CHECK(ExtendsTo("image/x-emf", "word/media/x", ".emf"));
   CHECK(ExtendsTo("image/svg+xml", "word/media/x", ".svg"));
   CHECK(ExtendsTo("image/tiff", "word/media/x", ".tif"));
   // A type is case-insensitive and may carry parameters; neither changes which format it names.
   CHECK(ExtendsTo("IMAGE/PNG", "word/media/x", ".png"));
   CHECK(ExtendsTo("image/png; charset=binary", "word/media/x", ".png"));
   CHECK(ExtendsTo("image/png ; q=1", "word/media/x", ".png"));

   CheckGroup("MediaExtractor: the extension nothing names");
   // Nothing this build knows, so the entry name's own extension is the next best answer -- but only
   // when it is short and alphanumeric, because anything else is a name a producer meant as data.
   CHECK(ExtendsTo("", "word/media/thing.dat", ".dat"));
   CHECK(ExtendsTo(nullptr, "word/media/thing.PNG", ".PNG"));
   CHECK(ExtendsTo("application/octet-stream", "word/media/thing.webp", ".webp"));
   CHECK(ExtendsTo("", "word/media/thing.averylongextension", ".bin"));
   CHECK(ExtendsTo("", "word/media/thing.a-b", ".bin"));
   CHECK(ExtendsTo("", "word/media/thing", ".bin"));
   CHECK(ExtendsTo("", "word/media.d/thing", ".bin"));
   CHECK(ExtendsTo(nullptr, nullptr, ".bin"));

   CheckGroup("MediaExtractor: where the pictures go");
   // With no --media-dir the directory is the document's own stem with "_media" on it, and the path a
   // reader follows is one leaf, so the .md and its pictures survive being moved together.
   CHECK(SitesAt(L"C:\\docs\\report.md", nullptr, L"C:\\docs\\report_media", "report_media"));
   CHECK(SitesAt(L"report.md", nullptr, L"report_media", "report_media"));
   CHECK(SitesAt(L"out/report.md", nullptr, L"out/report_media", "report_media"));
   // The stem is the .md's, so the pair always agree: a document written as report.md finds its
   // pictures in report_media whatever the input was called.
   CHECK(SitesAt(L"C:\\docs\\report.docx", nullptr, L"C:\\docs\\report_media", "report_media"));
   // With --media-dir the directory is exactly what was asked for, and the path is the same string
   // with the Windows separator folded to the one a URL uses.
   CHECK(SitesAt(L"C:\\docs\\report.md", L"pics", L"pics", "pics"));
   CHECK(SitesAt(L"C:\\docs\\report.md", L"out\\pics", L"out\\pics", "out/pics"));
   CHECK(SitesAt(L"report.md", L"../shared", L"../shared", "../shared"));
   // A name that is nothing but an extension keeps its whole name, exactly as the output path does.
   CHECK(SitesAt(L".docx", nullptr, L".docx_media", ".docx_media"));
   // A trailing separator is how a person spells "a directory", and every path built from the prefix
   // joins with a separator of its own -- so keeping it writes "pics//image1.png" into the document.
   CHECK(SitesAt(L"report.md", L"pics\\", L"pics", "pics"));
   CHECK(SitesAt(L"report.md", L"pics/", L"pics", "pics"));
   CHECK(SitesAt(L"report.md", L"out\\pics\\\\", L"out\\pics", "out/pics"));
   // A drive letter is the one place the separator is part of the name: trimming "C:\\" would put the
   // pictures in whatever the working directory on C: happens to be.
   CHECK(SitesAt(L"report.md", L"C:\\", L"C:\\", "C:/"));
}
