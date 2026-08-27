/*
 * File: Convert.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-27
 * Description: One document end to end: container, package, styles, walk, resolve, emit and write.
 * To Do: 1) Report the offset UtfValidate found, which the package records and nothing prints yet.
 *        2) Write through a temporary file and rename over the target, once a partial write costs more.
 *        3) Derive a media directory that is relative to the document rather than to the working
 *           directory when --media-dir names one, which today is the user's own business.
 * Dependencies: BuildGuards.h, CliOptions.h, Convert.h, Diag.h, DocWalker.h, Ir.h, LinkResolver.h,
 *               MdEmitter.h, MediaExtractor.h, OpcPackage.h, RunCoalescer.h, StyleModel.h, Utf.h,
 *               ZipReader.h, typedefs.h, memory management.h, stdio.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros, and memory management.h pulls those two in that order itself.
#include <windows.h>
#include <stdio.h>
#include "typedefs.h"
#include "memory management.h"
#include "CliOptions.h"
#include "Diag.h"
#include "DocWalker.h"
#include "Ir.h"
#include "LinkResolver.h"
#include "MdEmitter.h"
#include "MediaExtractor.h"
#include "OpcPackage.h"
#include "RunCoalescer.h"
#include "StyleModel.h"
#include "Utf.h"
#include "ZipReader.h"
#include "Convert.h"

//-- Limits

// Bytes per WriteFile call. A single call takes a DWORD count, and a smaller ceiling keeps a short write
// on a slow device to something the loop can absorb.
constexpr cui64 CONVERT_WRITE_BYTES = 1u << 20;

// What a derived output file is called.
static constexpr cwchptr CONVERT_EXTENSION = L".md";

//-- Paths

// Characters before the terminator.
static cui64 ConvertLength(cwchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Whether a character separates path components. Windows accepts both, and a command line carries either.
static cbool ConvertIsSeparator(cwchar value) { return value == L'\\' || value == L'/'; }

// Appends one character, reporting whether it fitted.
static cbool ConvertPut(wchptrc dest, cui64 destChars, ui64ptrc used, cwchar value) {
   if(*used + 1u >= destChars) return false;
   dest[*used] = value;
   *used += 1u;
   return true;
}

// Appends a run of characters.
static cbool ConvertPutRun(wchptrc dest, cui64 destChars, ui64ptrc used, cwchptr text, cui64 count) {
   for(ui64 index = 0; index < count; ++index) {
      if(!ConvertPut(dest, destChars, used, text[index])) return false;
   }
   return true;
}

// Where one path's own leaf begins and where its stem ends, which is what both a derived .md name and
// a derived media directory are built from. One function rather than two, because the two names
// have to agree: a document written as "report.md" must find its pictures in "report_media".
//
// The dot has to stand after the first character of the leaf, so a name that is nothing but an
// extension -- ".docx" -- keeps its whole name and gains ".md" rather than becoming ".md".
static void ConvertSplitPath(cwchptr path, ui64ptrc leafAt, ui64ptrc stemEnd) {
   cui64 length = ConvertLength(path);
   ui64  leaf   = 0;
   ui64  stem   = length;

   for(ui64 index = 0; index < length; ++index) {
      if(ConvertIsSeparator(path[index])) leaf = index + 1u;
   }
   for(ui64 index = leaf + 1u; index < length; ++index) {
      if(path[index] == L'.') stem = index;
   }
   *leafAt  = leaf;
   *stemEnd = (stem > leaf ? stem : length);
}

cbool ConvertOutputPath(cwchptr inputPath, cwchptr outputPath, cbool outputIsDirectory, wchptrc dest, cui64 destChars) {
   ui64 used = 0;

   if(!dest || destChars < 2u) return false;
   dest[0] = 0;

   cui64 named = (outputPath ? ConvertLength(outputPath) : 0u);
   // A trailing separator can only mean a directory: no Windows file name may end in one, so reading
   // "-o out\\" as a file name would name something that cannot exist. That is a refinement of D7d's
   // by-input-count rule rather than a departure from it -- with several inputs it is a directory
   // either way, and with one it is what the user plainly wrote.
   cbool directory = outputIsDirectory || (named && ConvertIsSeparator(outputPath[named - 1u]));

   if(outputPath && !directory) {
      // One input and an -o: the value is the output file's name and is used exactly as it was written.
      if(!ConvertPutRun(dest, destChars, &used, outputPath, ConvertLength(outputPath))) return false;
      dest[used] = 0;
      return used > 0;
   }
   if(!inputPath || !inputPath[0]) return false;

   ui64 leafAt  = 0;
   ui64 stemEnd = 0;

   ConvertSplitPath(inputPath, &leafAt, &stemEnd);
   if(outputPath) {
      if(!ConvertPutRun(dest, destChars, &used, outputPath, named)) return false;
      // A separator is added only when the directory does not already end in one. A trailing colon is
      // left alone: "C:" means the current directory on that drive, and "C:\" means its root.
      if(named && !ConvertIsSeparator(outputPath[named - 1u]) && outputPath[named - 1u] != L':') {
         if(!ConvertPut(dest, destChars, &used, L'\\')) return false;
      }
      if(!ConvertPutRun(dest, destChars, &used, inputPath + leafAt, stemEnd - leafAt)) return false;
   } else if(!ConvertPutRun(dest, destChars, &used, inputPath, stemEnd)) {
      return false;
   }
   if(!ConvertPutRun(dest, destChars, &used, CONVERT_EXTENSION, ConvertLength(CONVERT_EXTENSION))) return false;
   dest[used] = 0;
   return true;
}

// Whether two paths are the same string, folding ASCII case the way a Windows file system does. This is
// a literal comparison and not an identity test: ".\a.md" and "a.md" name one file and do not match here.
static cbool ConvertSamePath(cwchptr a, cwchptr b) {
   ui64 index = 0;

   for(;;) {
      cwchar left  = (a[index] >= L'A' && a[index] <= L'Z' ? wchar(a[index] - L'A' + L'a') : a[index]);
      cwchar right = (b[index] >= L'A' && b[index] <= L'Z' ? wchar(b[index] - L'A' + L'a') : b[index]);

      if(left != right) return false;
      if(!left) return true;
      ++index;
   }
}

//-- The media directory

// What is appended to a document's own stem to make the directory its pictures go in.
static constexpr cwchptr CONVERT_MEDIA_SUFFIX = L"_media";

// Transcodes a wide path into the UTF-8 form a Markdown destination is written in, folding the
// Windows separator to the one a URL uses. A path that will not fit yields nothing, and the caller
// then keeps the pictures out of the document rather than writing half a path into it.
static cbool ConvertPathToUtf8(cwchptr path, chptrc dest, cui64 destBytes) {
   ui64 produced = 0;

   dest[0] = 0;
   if(UtfFromWide(path, (ui8ptr)dest, destBytes - 1u, &produced) != UTF8_OK) return false;
   dest[produced] = 0;
   for(ui64 index = 0; index < produced; ++index) {
      if(dest[index] == '\\') dest[index] = '/';
   }
   return true;
}

cbool ConvertMediaDir(cwchptr documentPath, cwchptr named, wchptrc dir, cui64 dirChars, chptrc prefix, cui64 prefixChars) {
   ui64 used = 0;

   dir[0]    = 0;
   prefix[0] = 0;
   if(named) {
      if(!ConvertPutRun(dir, dirChars, &used, named, ConvertLength(named))) return false;
      dir[used] = 0;
      return used > 0 && ConvertPathToUtf8(dir, prefix, prefixChars);
   }

   ui64 leafAt  = 0;
   ui64 stemEnd = 0;

   ConvertSplitPath(documentPath, &leafAt, &stemEnd);
   if(stemEnd <= leafAt) return false;
   if(!ConvertPutRun(dir, dirChars, &used, documentPath, stemEnd)) return false;
   if(!ConvertPutRun(dir, dirChars, &used, CONVERT_MEDIA_SUFFIX, ConvertLength(CONVERT_MEDIA_SUFFIX))) return false;
   dir[used] = 0;
   // The path written into the document is the leaf alone, because the directory sits beside the .md.
   return ConvertPathToUtf8(dir + leafAt, prefix, prefixChars);
}

//-- Output

// Writes the converted document to a file, removing a partial one if the write does not finish. A
// half-written .md that looks converted is worse than none at all.
static cEXIT_CODE ConvertWriteFile(cwchptr path, cchptr bytes, cui64 byteCount) {
   cHANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

   if(file == INVALID_HANDLE_VALUE) {
      DiagErrorText("cannot create output file", path);
      return EXIT_OUTPUT;
   }

   ui64 done = 0;

   while(done < byteCount) {
      cui64 remaining = byteCount - done;
      cui64 want      = (remaining > CONVERT_WRITE_BYTES ? CONVERT_WRITE_BYTES : remaining);
      DWORD put       = 0;

      if(!WriteFile(file, bytes + done, DWORD(want), &put, nullptr) || !put) {
         CloseHandle(file);
         DeleteFileW(path);
         DiagErrorText("cannot write output file", path);
         return EXIT_OUTPUT;
      }
      done += put;
   }
   if(!CloseHandle(file)) {
      DeleteFileW(path);
      DiagErrorText("cannot write output file", path);
      return EXIT_OUTPUT;
   }
   return EXIT_ALL_CONVERTED;
}

//-- Pipeline

// The part index of the styles part, found through the main part's own relationships rather than by
// name (correctness rule 1). A document with no styles part is legal, and so is one whose styles
// relationship points outside the package; both come back as -1 and the model stays empty.
static csi32 ConvertStylesPart(OPC_PACKAGEptrc package, csi32 mainPart) {
   csi32 relation = OpcFindRelByKind(package, mainPart, OPC_REL_STYLES);

   if(relation < 0) return -1;

   cOPC_REL_VIEW record = OpcRel(package, relation);

   if(record.external || !record.part || !record.part[0]) return -1;
   return OpcFindPart(package, record.part);
}

// Turns one opened package into Markdown, and plans the media that goes beside it. The package, the
// emitter and the plan are the caller's; this is only the middle of the pipeline, so that every
// allocation has exactly one owner and one release path.
static cEXIT_CODE ConvertPackage(OPC_PACKAGEptrc package, cwchptr inputPath, MD_EMITTERptrc emitter, // The document
                                 MEDIA_SETptrc media, cchptr mediaPrefix, cbool emitImages) {        // Its pictures
   csi32 mainPart = OpcMainPart(package);

   // The main part's relationships are read before anything else needs them, so that a malformed
   // relationships part is a refusal whether or not the styles part turns out to exist.
   cOPC_RESULT related = OpcLoadRels(package, mainPart);

   if(related != OPC_OK) {
      DiagErrorText(OpcResultText(package, related), inputPath);
      return OpcExitCode(package, related);
   }

   STYLE_MODEL styles;
   csi32       stylesPart = ConvertStylesPart(package, mainPart);

   StyleOpen(&styles);

   cSTYLE_RESULT styled = StyleLoad(&styles, package, stylesPart);

   if(styled != STYLE_OK) {
      DiagErrorText(StyleResultText(package, &styles, styled), inputPath);

      // A container or encoding refusal keeps the package's own verdict, so a failed allocation while
      // reading a part is this program's fault and not the document's.
      cEXIT_CODE fallback = (styled == STYLE_ERROR_MEMORY ? EXIT_INTERNAL : EXIT_NOT_DOCX);
      cEXIT_CODE verdict  = (styled == STYLE_ERROR_PART ? OpcExitCode(package, styles.lastOpc) : fallback);

      StyleClose(&styles);
      return verdict;
   }

   IR_DOCUMENT document;

   IrOpen(&document);

   cWALK_STATUS walked = DocWalk(&document, package, &styles, mainPart);

   StyleClose(&styles);
   if(walked.result != WALK_OK) {
      DiagErrorText(DocWalkResultText(package, walked), inputPath);
      IrClose(&document);
      if(walked.result == WALK_ERROR_PART) return OpcExitCode(package, walked.opc);
      return (walked.result == WALK_ERROR_MEMORY ? EXIT_INTERNAL : EXIT_NOT_DOCX);
   }

   // Merging adjacent runs and hoisting whitespace out of the formatted ones stands between the walk and
   // the emitter, and not inside either: the walker must not merge, because the fragmentation is what M6
   // is measured against, and the emitter must be able to assume a delimiter is safe around every
   // formatted span it is handed rather than testing each one.
   //
   // Then the four M7 passes, in the one order that works. References resolve against the part they
   // were read in; anchors resolve once every reference is a destination, because a heading's slug is
   // numbered over the whole document; the media plan turns a part name into a path and can turn a
   // picture back into its alt text; and dropping the emptied blocks last is what restores the
   // invariant the emitter rests on, that every block it is handed produces at least one byte.
   bool ready = RunCoalesce(&document);

   if(ready) ready = LinkResolveRefs(&document, package, mainPart);
   if(ready) ready = LinkResolveAnchors(&document);
   if(ready) ready = MediaPlan(media, &document, package, mediaPrefix, emitImages);
   if(ready) IrDropEmptyBlocks(&document);

   cMD_RESULT emitted = (ready ? MdEmitDocument(emitter, &document) : MD_ERROR_MEMORY);

   IrClose(&document);
   if(emitted != MD_OK) {
      DiagErrorText("not enough memory to hold the converted document", inputPath);
      return EXIT_INTERNAL;
   }
   return EXIT_ALL_CONVERTED;
}

//== Entry points

cCONVERT_TARGET ConvertTargetTaken(cCLI_OPTIONSptr options, cui32 index) {
   wchar mine[CONVERT_MAX_PATH];
   wchar theirs[CONVERT_MAX_PATH];

   // --stdout writes no file, and a run that cannot derive a path fails on its own in ConvertFile.
   if(!options || options->toStdout || index >= options->inputCount) return CONVERT_TARGET_FREE;

   cbool several = options->inputCount > 1u;

   if(!ConvertOutputPath(options->inputs[index], options->outputPath, several, mine, CONVERT_MAX_PATH)) {
      return CONVERT_TARGET_FREE;
   }
   for(ui32 other = 0; other < options->inputCount; ++other) {
      // An input that is its own output is ConvertFile's case and carries ConvertFile's message.
      if(other != index && ConvertSamePath(mine, options->inputs[other])) return CONVERT_TARGET_IS_INPUT;
      if(other >= index) continue;
      // One input named twice is not a collision: the second conversion writes the same bytes over
      // its own, which loses nothing. Only two different inputs claiming one output destroy a document.
      if(ConvertSamePath(options->inputs[index], options->inputs[other])) continue;
      if(!ConvertOutputPath(options->inputs[other], options->outputPath, several, theirs, CONVERT_MAX_PATH)) {
         continue;
      }
      if(ConvertSamePath(mine, theirs)) return CONVERT_TARGET_CLAIMED;
   }
   return CONVERT_TARGET_FREE;
}

cEXIT_CODE ConvertFile(cCLI_OPTIONSptr options, cwchptr inputPath) {
   wchar outputPath[CONVERT_MAX_PATH];

   outputPath[0] = 0;
   if(!options->toStdout) {
      cbool derived = ConvertOutputPath(inputPath, options->outputPath, options->inputCount > 1u, outputPath, CONVERT_MAX_PATH);

      if(!derived) {
         // The buffer is always CONVERT_MAX_PATH here and an operand is never empty, so length is the
         // only way this fails. Saying so beats a sentence that reads like the converter gave up.
         DiagErrorText("the output path would be longer than this converter builds", inputPath);
         return EXIT_OUTPUT;
      }
      if(ConvertSamePath(outputPath, inputPath)) {
         DiagErrorText("the output path is the input file", inputPath);
         return EXIT_OUTPUT;
      }
   }

   ZIP_READER  reader;
   cZIP_RESULT opened = ZipOpen(&reader, inputPath, &ZIP_DEFAULT_LIMITS);

   if(opened != ZIP_OK) {
      DiagErrorText(ZipResultText(&reader, opened), inputPath);
      ZipClose(&reader);
      if(opened == ZIP_ERROR_OPEN) return EXIT_INPUT;
      if(opened == ZIP_ERROR_MEMORY || opened == ZIP_ERROR_RANGE) return EXIT_INTERNAL;
      return EXIT_NOT_DOCX;
   }

   OPC_PACKAGE package;
   MD_EMITTER  emitter;
   MEDIA_SET   media;
   wchar       mediaDir[CONVERT_MAX_PATH];
   char        mediaPrefix[CONVERT_MAX_PATH];
   EXIT_CODE   verdict = EXIT_ALL_CONVERTED;

   MdOpen(&emitter, options->hardBreak);
   MediaOpen(&media);

   // Decision D13, recommended and not yet ruled: --stdout extracts its pictures too, beside the input,
   // where the document itself would have gone. The alternative -- writing nothing to disk, which is
   // the promise --stdout otherwise makes -- would make the two output paths produce *different*
   // documents, and that they produce the same one is a property this converter is measured on.
   // A user who does not want the files has --no-images; a note says where they went, on stderr, so
   // that it cannot reach the pipe.
   cwchptr beside = (options->toStdout ? inputPath : outputPath); // What the pictures sit next to
   cbool   sited  = options->emitImages &&                        // The one long call: named arguments keep it inside e2
                 ConvertMediaDir(beside, options->mediaDir, mediaDir, CONVERT_MAX_PATH, mediaPrefix, CONVERT_MAX_PATH);
   // The package borrows the reader, so both are released together on one path: a per-branch close would
   // turn the next branch anyone adds here into a use-after-free.
   cOPC_RESULT built = OpcOpen(&package, &reader);

   if(built != OPC_OK) {
      DiagErrorText(OpcResultText(&package, built), inputPath);
      verdict = OpcExitCode(&package, built);
   } else {
      verdict = ConvertPackage(&package, inputPath, &emitter, &media, mediaPrefix, sited);
   }
   if(verdict != EXIT_ALL_CONVERTED) {
      OpcClose(&package);
      ZipClose(&reader);
      MediaClose(&media);
      MdClose(&emitter);
      return verdict;
   }
   if(options->toStdout) {
      // stdout is the only copy of the document, so a short write loses it. The file path deletes a
      // half-written .md for the same reason; this one can only report, and must not report success.
      if(!DiagWriteOutBytes((cui8ptr)MdBytes(&emitter), MdByteCount(&emitter))) {
         DiagError("cannot write the converted document to standard output");
         verdict = EXIT_OUTPUT;
      }
   } else {
      verdict = ConvertWriteFile(outputPath, MdBytes(&emitter), MdByteCount(&emitter));
      if(verdict == EXIT_ALL_CONVERTED && !options->quiet) {
         char note[64];

         snprintf(note, sizeof(note), "wrote %llu bytes", MdByteCount(&emitter));
         DiagNoteText(note, outputPath);
      }
   }
   // The pictures go out after the document, so a conversion that failed leaves nothing behind at all.
   // They need the package still open, which is why it is closed here rather than above.
   if(verdict == EXIT_ALL_CONVERTED) verdict = MediaWrite(&media, &package, mediaDir);
   if(verdict == EXIT_ALL_CONVERTED && media.count && !options->quiet) {
      char note[64];

      snprintf(note, sizeof(note), "extracted %u image%s into", media.count, (media.count == 1u ? "" : "s"));
      DiagNoteText(note, mediaDir);
   }
   OpcClose(&package);
   ZipClose(&reader);
   MediaClose(&media);
   MdClose(&emitter);
   return verdict;
}
