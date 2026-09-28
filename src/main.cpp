/*
 * File: main.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-09-27
 * Description: Entry point: console UTF-8 setup, option parsing, and the hand-over to Batch.
 * To Do: 1) Put the console's output code page back on exit: SetConsoleOutputCP outlives the process.
 * Dependencies: Batch.h, BuildGuards.h, CliOptions.h, Diag.h, typedefs.h, windows.h
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
#include "Batch.h"
#include "CliOptions.h"
#include "Diag.h"

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

   // Batch plans the run, converts each input on a pool of at most --threads workers (D6/D7a), lists what
   // failed and folds the verdicts (D7c). From here the options are read-only: every worker shares them.
   cEXIT_CODE code = BatchRun(&options);

   CliFree(&options);
   return code;
}
