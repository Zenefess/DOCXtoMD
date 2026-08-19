/*
 * File: main.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Entry point: console UTF-8 setup, option parsing, the container probe and exit-code mapping.
 * To Do: 1) Replace the container probe with the real pipeline as M4 through M11 build it.
 *        2) Replace the per-input loop with a Batch call when M13 adds the bounded worker pool (D6/D7a).
 *        3) Return exit code 6 once several inputs can succeed and fail independently (D7c).
 * Dependencies: BuildGuards.h, CliOptions.h, Diag.h, ZipReader.h, typedefs.h, memory management.h, windows.h, stdio.h
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
#include "ZipReader.h"

//-- Package entry points

// The only two part names ISO/IEC 29500-2 guarantees. Every other part, the main document included, is
// found through relationships rather than by name -- which is M4's OpcPackage, not this file's business.
constexpr cchptr PART_CONTENT_TYPES = "[Content_Types].xml";
constexpr cchptr PART_RELS          = "_rels/.rels";

// Where every producer in the wild puts the body. M3 reads it, when it is there, only to exercise the
// inflater on a real deflate stream; nothing depends on the name, and M4 resolves the part properly.
constexpr cchptr PART_DOCUMENT = "word/document.xml";

// The note one verified container produces, split so neither call runs past the column limit.
constexpr cchptr NOTE_CONTAINER = "container verified: %u entries; %s %llu bytes; %s %llu bytes";
constexpr cchptr NOTE_PART      = "; %s %llu bytes";

//-- Container probe

// Inflates one part and discards it. At M3 the verification is the point, not the bytes: the entry is
// found, inflated inside the bomb caps, and matched against the CRC-32 its directory entry declares.
static cZIP_RESULT ProbeReadPart(ZIP_READERptrc reader, cchptr name, ui64ptrc byteCount, boolptrc found) {
   csi32 index = ZipFindEntry(reader, name);

   *byteCount = 0;
   *found     = (index >= 0);
   if(index < 0) return ZIP_OK;

   ui8ptr      bytes = nullptr;
   cZIP_RESULT read  = ZipReadEntry(reader, ui32(index), &bytes, byteCount);

   mdealloc(bytes);
   return read;
}

// Reports a container failure and maps it onto the process exit code. Only the results that are not the
// file's own fault escape exit code 3: an unopenable path is 2, and a failed allocation or a bad entry
// index -- this program's mistake rather than the document's -- is 5.
static cEXIT_CODE ProbeFailed(ZIP_READERptrc reader, cwchptr path, cZIP_RESULT result) {
   DiagErrorText(ZipResultText(reader, result), path);
   ZipClose(reader);
   if(result == ZIP_ERROR_OPEN) return EXIT_INPUT;
   if(result == ZIP_ERROR_MEMORY || result == ZIP_ERROR_RANGE) return EXIT_INTERNAL;
   return EXIT_NOT_DOCX;
}

// Opens one input as an OPC package and verifies as much of it as M3 can. Returns EXIT_ALL_CONVERTED when
// the container is sound -- which is not the same as the input being converted; wmain still reports that
// nothing was written.
static cEXIT_CODE ProbeInput(cwchptr path, cbool quiet) {
   ZIP_READER  reader;
   cZIP_RESULT opened = ZipOpen(&reader, path, &ZIP_DEFAULT_LIMITS);

   if(opened != ZIP_OK) return ProbeFailed(&reader, path, opened);

   // Presence first, so a package missing an entry point is reported as that rather than as whatever the
   // next part it does have turns out to be.
   if(ZipFindEntry(&reader, PART_CONTENT_TYPES) < 0) {
      DiagErrorText("not a valid DOCX; the package has no [Content_Types].xml", path);
      ZipClose(&reader);
      return EXIT_NOT_DOCX;
   }
   if(ZipFindEntry(&reader, PART_RELS) < 0) {
      DiagErrorText("not a valid DOCX; the package has no _rels/.rels", path);
      ZipClose(&reader);
      return EXIT_NOT_DOCX;
   }

   ui64 contentBytes = 0, relsBytes = 0, documentBytes = 0;
   bool present = false, hasDocument = false;

   cZIP_RESULT readTypes = ProbeReadPart(&reader, PART_CONTENT_TYPES, &contentBytes, &present);

   if(readTypes != ZIP_OK) return ProbeFailed(&reader, path, readTypes);

   cZIP_RESULT readRels = ProbeReadPart(&reader, PART_RELS, &relsBytes, &present);

   if(readRels != ZIP_OK) return ProbeFailed(&reader, path, readRels);

   cZIP_RESULT readDocument = ProbeReadPart(&reader, PART_DOCUMENT, &documentBytes, &hasDocument);

   if(readDocument != ZIP_OK) return ProbeFailed(&reader, path, readDocument);

   if(!quiet) {
      char message[256];

      // snprintf reports what it would have written, so a head at or past the buffer means the first
      // clause already filled it and the second must not be appended.
      csi32 head = snprintf(message, sizeof(message), NOTE_CONTAINER, reader.entryCount, PART_CONTENT_TYPES, contentBytes, PART_RELS, relsBytes);
      cui64 room = (head > 0 && ui64(head) < sizeof(message) ? sizeof(message) - ui64(head) : 0);

      if(hasDocument && room) snprintf(message + head, room, NOTE_PART, PART_DOCUMENT, documentBytes);
      DiagNoteText(message, path);
   }
   ZipClose(&reader);
   return EXIT_ALL_CONVERTED;
}

//== Entry point

// r11 does not reach this name: the entry point is spelled by the language, not chosen here. Wide argv is
// the only way a path outside the active code page survives, so every path stays UTF-16 until Win32 or Diag.
si32 wmain(si32 argc, wchptrptr argv) {
   // The tool's whole output contract is UTF-8, and the console has to be told once, before anything prints.
   SetConsoleOutputCP(CP_UTF8);

   CLI_OPTIONS options;

   cEXIT_CODE parsed = CliParse(argc, argv, &options);

   if(parsed != EXIT_ALL_CONVERTED) {
      // Only a usage error earns the usage text; an allocation failure is not the user's mistake.
      if(parsed == EXIT_USAGE) CliWriteUsage(true);
      return parsed;
   }
   if(options.showHelp) {
      CliWriteUsage(false);
      CliFree(&options);
      return EXIT_ALL_CONVERTED;
   }
   if(options.showVersion) {
      CliWriteVersion();
      CliFree(&options);
      return EXIT_ALL_CONVERTED;
   }

   ui32 failures = 0;
   si32 worst    = si32(EXIT_ALL_CONVERTED);

   // RULE-DEV:a2 single-threaded by owner ruling (D5, narrowed by D6): the bounded worker pool that walks
   // this list belongs to Batch, which M13 adds. Until then the driver visits each input in argument order.
   for(ui32 i = 0; i < options.inputCount; ++i) {
      cEXIT_CODE code = ProbeInput(options.inputs[i], options.quiet);

      if(code == EXIT_ALL_CONVERTED) continue;
      ++failures;
      if(si32(code) > worst) worst = si32(code);
   }
   if(failures) {
      // Not exit code 6: D7c reserves that for a run in which something was converted, and nothing is
      // converted in this build. The highest per-file verdict is what a run of pure failures returns.
      CliFree(&options);
      return worst;
   }

   // M3 gets the container open and verified; the XML, package and conversion stages land at M4 through
   // M11. Returning 0 here would claim exit code 0's contract -- every input converted -- for work this
   // build cannot do.
   DiagError("conversion is not implemented in this build; no output was written");
   CliFree(&options);
   return EXIT_INTERNAL;
}
