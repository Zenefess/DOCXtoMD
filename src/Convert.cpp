/*
 * File: Convert.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: One document end to end: container, package, styles, walk, emit, and the output write.
 * To Do: 1) Report the offset UtfValidate found, which the package records and nothing prints yet.
 *        2) Write through a temporary file and rename over the target, once a partial write costs more.
 * Dependencies: BuildGuards.h, CliOptions.h, Convert.h, Diag.h, DocWalker.h, Ir.h, MdEmitter.h,
 *               OpcPackage.h, StyleModel.h, ZipReader.h, typedefs.h, memory management.h, windows.h,
 *               stdio.h
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
#include "MdEmitter.h"
#include "OpcPackage.h"
#include "StyleModel.h"
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

   cui64 length = ConvertLength(inputPath);
   ui64  leafAt = 0;

   for(ui64 index = 0; index < length; ++index) {
      if(ConvertIsSeparator(inputPath[index])) leafAt = index + 1u;
   }

   ui64 stemEnd = length;

   // The dot has to stand after the first character of the leaf, so a name that is nothing but an
   // extension -- ".docx" -- keeps its whole name and gains ".md" rather than becoming ".md".
   for(ui64 index = leafAt + 1u; index < length; ++index) {
      if(inputPath[index] == L'.') stemEnd = index;
   }
   if(stemEnd <= leafAt) stemEnd = length;
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

// Turns one opened package into Markdown. The package and the emitter are the caller's; this is only the
// middle of the pipeline, so that every allocation has exactly one owner and one release path.
static cEXIT_CODE ConvertPackage(OPC_PACKAGEptrc package, cwchptr inputPath, MD_EMITTERptrc emitter) {
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

   cMD_RESULT emitted = MdEmitDocument(emitter, &document);

   IrClose(&document);
   if(emitted != MD_OK) {
      DiagErrorText("not enough memory to hold the converted document", inputPath);
      return EXIT_INTERNAL;
   }
   return EXIT_ALL_CONVERTED;
}

//== Entry points

cEXIT_CODE ConvertFile(cCLI_OPTIONSptr options, cwchptr inputPath) {
   wchar outputPath[CONVERT_MAX_PATH];

   outputPath[0] = 0;
   if(!options->toStdout) {
      cbool derived = ConvertOutputPath(inputPath, options->outputPath, options->inputCount > 1u, outputPath, CONVERT_MAX_PATH);

      if(!derived) {
         DiagErrorText("cannot work out an output path for", inputPath);
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
   EXIT_CODE   verdict = EXIT_ALL_CONVERTED;

   MdOpen(&emitter, options->hardBreak);

   // The package borrows the reader, so both are released together on one path: a per-branch close would
   // turn the next branch anyone adds here into a use-after-free.
   cOPC_RESULT built = OpcOpen(&package, &reader);

   if(built != OPC_OK) {
      DiagErrorText(OpcResultText(&package, built), inputPath);
      verdict = OpcExitCode(&package, built);
   } else {
      verdict = ConvertPackage(&package, inputPath, &emitter);
   }
   OpcClose(&package);
   ZipClose(&reader);
   if(verdict != EXIT_ALL_CONVERTED) {
      MdClose(&emitter);
      return verdict;
   }
   if(options->toStdout) {
      DiagWriteOutBytes((cui8ptr)MdBytes(&emitter), MdByteCount(&emitter));
   } else {
      verdict = ConvertWriteFile(outputPath, MdBytes(&emitter), MdByteCount(&emitter));
      if(verdict == EXIT_ALL_CONVERTED && !options->quiet) {
         char note[64];

         snprintf(note, sizeof(note), "wrote %llu bytes", (unsigned long long)MdByteCount(&emitter));
         DiagNoteText(note, outputPath);
      }
   }
   MdClose(&emitter);
   return verdict;
}
