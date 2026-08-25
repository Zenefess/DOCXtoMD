/*
 * File: main.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-25
 * Description: Entry point: console UTF-8 setup, option parsing, the input loop and the exit-code fold.
 * To Do: 1) Replace the per-input loop with a Batch call when M13 adds the bounded worker pool (D6/D7a).
 *        2) Repeat the failures as one list before exiting, rather than only where each one happened.
 * Dependencies: BuildGuards.h, CliOptions.h, Convert.h, Diag.h, typedefs.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros.
#include <windows.h>
#include "typedefs.h"
#include "CliOptions.h"
#include "Convert.h"
#include "Diag.h"

//-- Messages

// The two ways one input's output path is already spoken for. Named constants because the ternary
// that chooses between them does not fit inside e2's 150 columns with both sentences written inline.
constexpr cchptr TAKEN_BY_INPUT   = "the output path is another input of this run";
constexpr cchptr TAKEN_BY_EARLIER = "an earlier input already writes that output file";

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

   ui32 converted = 0;
   ui32 failures  = 0;
   si32 worst     = si32(EXIT_ALL_CONVERTED);

   // RULE-DEV:a2 single-threaded by owner ruling (D5, narrowed by D6): the bounded worker pool that walks
   // this list belongs to Batch, which M13 adds. Until then the driver visits each input in argument order.
   for(ui32 i = 0; i < options.inputCount; ++i) {
      // Pre-flight, before anything is written: D7b derives every output name from an input's own
      // leaf, so two inputs named report.docx in two directories both target one report.md. Left
      // alone that converts both and keeps the second, which is silent data loss reported as success.
      cCONVERT_TARGET taken = ConvertTargetTaken(&options, i);

      if(taken != CONVERT_TARGET_FREE) {
         cchptr why = (taken == CONVERT_TARGET_IS_INPUT ? TAKEN_BY_INPUT : TAKEN_BY_EARLIER);

         DiagErrorText(why, options.inputs[i]);
         ++failures;
         if(si32(EXIT_OUTPUT) > worst) worst = si32(EXIT_OUTPUT);
         continue;
      }

      cEXIT_CODE code = ConvertFile(&options, options.inputs[i]);

      if(code == EXIT_ALL_CONVERTED) {
         ++converted;
         continue;
      }
      ++failures;
      if(si32(code) > worst) worst = si32(code);
   }
   CliFree(&options);
   if(!failures) return EXIT_ALL_CONVERTED;
   // D7c gives a run that both converted something and failed something its own exit code, so that a
   // caller can tell it apart from a run that did nothing. Each failure has already named itself where
   // it happened, which is what makes 6 a summary rather than the only diagnosis.
   if(converted) return EXIT_PARTIAL;
   // When everything failed, the highest per-file verdict is what the run returns.
   return worst;
}
