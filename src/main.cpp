/*
 * File: main.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-24
 * Description: Entry point: console UTF-8 setup, option parsing, the package probe and exit-code mapping.
 * To Do: 1) Replace the package probe with the real pipeline as M5 through M11 build it.
 *        2) Replace the per-input loop with a Batch call when M13 adds the bounded worker pool (D6/D7a).
 *        3) Return exit code 6 once several inputs can succeed and fail independently (D7c).
 * Dependencies: BuildGuards.h, CliOptions.h, Diag.h, OpcPackage.h, XmlPull.h, ZipReader.h, typedefs.h,
 *               memory management.h, windows.h, stdio.h
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
#include "XmlPull.h"
#include "OpcPackage.h"

//-- Probe messages

// The note one verified package produces, split so neither call runs past the column limit.
constexpr cchptr NOTE_PACKAGE  = "package verified: %u entries; main part %s %llu bytes; %u elements";
constexpr cchptr NOTE_RESOLVED = "resolved through relationships: %s";

// The parts a converter loads eagerly, resolved through the main part's own relationships rather than by
// name. M5 onwards reads them; M4 names them in a note, which is what proves they were resolved at all.
struct PROBE_KIND {
   OPC_REL_KIND kind;  ///< The relationship type to look for
   cchptr       label; ///< What to call it in the note
};

static constexpr PROBE_KIND PROBE_KINDS[] = {
    // One row per part the converter will want, in the order a reader would expect them
    {OPC_REL_STYLES, "styles"},       // word/styles.xml
    {OPC_REL_NUMBERING, "numbering"}, // word/numbering.xml
    {OPC_REL_SETTINGS, "settings"},   // word/settings.xml
    {OPC_REL_FOOTNOTES, "footnotes"}, // word/footnotes.xml
    {OPC_REL_ENDNOTES, "endnotes"},   // word/endnotes.xml
    {OPC_REL_COMMENTS, "comments"}    // word/comments.xml
};

constexpr cui32 PROBE_KIND_COUNT = ui32(sizeof(PROBE_KINDS) / sizeof(PROBE_KINDS[0]));

//-- Package probe

// Tokenizes the main document part from end to end and counts its elements. At M4 the walk is the point
// rather than the elements: it proves the part is well-formed XML in the namespaces the converter knows,
// and it is where a part that is not gets named instead of reaching a walker that cannot cope with it.
static cEXIT_CODE ProbeMainPart(OPC_PACKAGEptrc package, cwchptr path, ui32ptrc elements) {
   csi32 mainPart = OpcMainPart(package);

   *elements = 0;

   cOPC_RESULT loaded = OpcLoadXmlPart(package, mainPart);

   if(loaded != OPC_OK) {
      DiagErrorText(OpcResultText(package, loaded), path);
      return OpcExitCode(package, loaded);
   }

   XML_READER reader;
   ui32       counted = 0;

   // The tokenizer's own sentence is what a user needs here, so this reports rather than handing a
   // result back to a layer that would have to carry the reason for it.
   if(XmlOpen(&reader, OpcPartBytes(package, mainPart), OpcPartByteCount(package, mainPart)) == XML_OK) {
      for(;;) {
         cXML_TOKEN token = XmlNext(&reader);

         if(token == XML_TOKEN_START_ELEMENT) ++counted;
         if(token == XML_TOKEN_END_OF_INPUT || token == XML_TOKEN_ERROR) break;
      }
   }

   cXML_RESULT broke = reader.result;

   XmlClose(&reader);
   if(broke != XML_OK) {
      DiagErrorText(OpcMessageIn(package, XmlResultText(broke), mainPart), path);
      return EXIT_NOT_DOCX;
   }

   // The main part's own relationships are read here rather than in the note, so that a malformed
   // relationships part is a refusal whether or not anything is being printed -- -q must not decide
   // whether a defect is noticed. M5 onwards needs these resolved anyway.
   cOPC_RESULT related = OpcLoadRels(package, mainPart);

   if(related != OPC_OK) {
      DiagErrorText(OpcResultText(package, related), path);
      return OpcExitCode(package, related);
   }
   *elements = counted;
   return EXIT_ALL_CONVERTED;
}

// Names, in one line, whichever of the converter's eager parts the main document part relates to. The
// relationships have already been read and accepted by ProbeMainPart; nothing is loaded here, and no part
// is opened. What is being shown is that each was found by relationship rather than by name.
static void ProbeReportRelated(OPC_PACKAGEptrc package, cwchptr path) {
   csi32 mainPart = OpcMainPart(package);

   char message[512];
   ui64 used  = 0;
   ui32 named = 0;

   message[0] = 0;
   for(ui32 index = 0; index < PROBE_KIND_COUNT; ++index) {
      csi32 found = OpcFindRelByKind(package, mainPart, PROBE_KINDS[index].kind);

      if(found < 0) continue;

      // Found by kind, then looked up again by its own Id in the part that declared it. Relationship ids
      // are scoped per part (correctness rule 1), and this is the path that says so in code.
      cOPC_REL_VIEW byKind  = OpcRel(package, found);
      cOPC_REL_VIEW record  = OpcRel(package, OpcFindRelById(package, mainPart, byKind.id));
      cchptr        gap     = (named ? ", " : "");
      cchptr        label   = PROBE_KINDS[index].label;
      csi32         written = snprintf(message + used, sizeof(message) - used, "%s%s -> %s", gap, label, record.part);

      if(written <= 0 || ui64(written) >= sizeof(message) - used) break;
      used += ui64(written);
      ++named;
   }
   if(!named) return;

   char line[640];

   snprintf(line, sizeof(line), NOTE_RESOLVED, message);
   DiagNoteText(line, path);
}

// Opens one input as an OPC package and verifies as much of it as M4 can. Returns EXIT_ALL_CONVERTED when
// the package is sound -- which is not the same as the input being converted; wmain still reports that
// nothing was written.
static cEXIT_CODE ProbeInput(cwchptr path, cbool quiet) {
   ZIP_READER  reader;
   cZIP_RESULT opened = ZipOpen(&reader, path, &ZIP_DEFAULT_LIMITS);

   if(opened != ZIP_OK) {
      DiagErrorText(ZipResultText(&reader, opened), path);
      ZipClose(&reader);
      if(opened == ZIP_ERROR_OPEN) return EXIT_INPUT;
      if(opened == ZIP_ERROR_MEMORY || opened == ZIP_ERROR_RANGE) return EXIT_INTERNAL;
      return EXIT_NOT_DOCX;
   }

   OPC_PACKAGE package;
   EXIT_CODE   verdict = EXIT_ALL_CONVERTED;

   // The package borrows the reader, so both are released together on one path: a per-branch close would
   // turn the next branch anyone adds here into a use-after-free.
   cOPC_RESULT built = OpcOpen(&package, &reader);

   if(built != OPC_OK) {
      DiagErrorText(OpcResultText(&package, built), path);
      verdict = OpcExitCode(&package, built);
   } else {
      ui32 elements = 0;

      cEXIT_CODE walked = ProbeMainPart(&package, path, &elements);

      if(walked != EXIT_ALL_CONVERTED) {
         verdict = walked;
      } else if(!quiet) {
         csi32  mainPart = OpcMainPart(&package);
         cchptr name     = OpcPartName(&package, mainPart);
         cui64  bytes    = OpcPartByteCount(&package, mainPart);
         char   message[512];

         snprintf(message, sizeof(message), NOTE_PACKAGE, reader.entryCount, name, bytes, elements);
         DiagNoteText(message, path);
         ProbeReportRelated(&package, path);
      }
   }
   OpcClose(&package);
   ZipClose(&reader);
   return verdict;
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

   // M4 gets the package resolved and the main part tokenised; the style, walk and emit stages land at M5
   // through M11. Returning 0 here would claim exit code 0's contract -- every input converted -- for work
   // this build cannot do.
   DiagError("conversion is not implemented in this build; no output was written");
   CliFree(&options);
   return EXIT_INTERNAL;
}
