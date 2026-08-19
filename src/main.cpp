/*
 * File: main.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Entry point: console UTF-8 setup, option parsing, input checks and exit-code mapping.
 * To Do: 1) Hand each readable input to the conversion pipeline as M3 through M11 build it.
 *        2) Replace the per-input loop with a Batch call when M13 adds the bounded worker pool (D6/D7a).
 *        3) Return exit code 6 once several inputs can succeed and fail independently (D7c).
 * Dependencies: BuildGuards.h, CliOptions.h, Diag.h, typedefs.h, windows.h
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
#include "Diag.h"

//-- Local helpers

// Reports whether a path names a file that can be opened for reading. A directory fails here too:
// CreateFileW refuses one under OPEN_EXISTING without FILE_FLAG_BACKUP_SEMANTICS, which is wanted.
static cbool InputIsReadable(cwchptr path) {
   cHANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

   if(file == INVALID_HANDLE_VALUE) return false;

   CloseHandle(file);
   return true;
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

   ui32 unreadable = 0;

   // RULE-DEV:a2 single-threaded by owner ruling (D5, narrowed by D6): the bounded worker pool that walks
   // this list belongs to Batch, which M13 adds. Until then the driver visits each input in argument order.
   for(ui32 i = 0; i < options.inputCount; ++i) {
      if(!InputIsReadable(options.inputs[i])) {
         DiagErrorText("cannot open input file", options.inputs[i]);
         ++unreadable;
      }
   }
   if(unreadable) {
      CliFree(&options);
      return EXIT_INPUT;
   }

   // M2 is the CLI skeleton; the container, XML and conversion stages land at M3 through M11. Returning 0
   // here would claim exit code 0's contract -- every input converted -- for work this build cannot do.
   DiagError("conversion is not implemented in this build; no output was written");
   CliFree(&options);
   return EXIT_INTERNAL;
}
