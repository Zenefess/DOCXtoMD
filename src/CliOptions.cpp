/*
 * File: CliOptions.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Command-line parser and validator, plus the usage and version text.
 * To Do: 1) Accept the policy switches CONVERSION_REFERENCE.md 6.3 lists as their conversion stages land.
 *        2) Move output-path derivation here once M5 writes files, including the -o file-or-directory split.
 *        3) Add the duplicate-output pre-flight check that stops two workers targeting one .md file (M13).
 * Dependencies: BuildGuards.h, CliOptions.h, Diag.h, typedefs.h, memory management.h, windows.h, stdio.h
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

//== Console text

// The usage text is the published CLI contract: keep it in step with the Target CLI block in CLAUDE.md.
constexpr cchptr USAGE_TEXT = "Usage: DOCXtoMD [options] <input.docx> [input2.docx [input3.docx [...]]]\n"
                              "  -o, --output <path>    output path: the .md filename for a single input,\n"
                              "                         the destination directory when several are given\n"
                              "  -j, --threads <n>      worker threads (default: system virtual core count)\n"
                              "  --media-dir <dir>      image dir (default <stem>_media\\)   --no-images   alt text only\n"
                              "  --hard-break=<backslash|spaces>  (default backslash)      -q, --quiet   errors only\n"
                              "  --stdout               markdown to stdout - single input only\n"
                              "  --version              print version, exit 0              -h, --help    usage, exit 0\n";

//-- Local helpers

// Exact comparison of two NUL-terminated wide strings.
static cbool StringEqual(cwchptr a, cwchptr b) {
   ui32 i = 0;

   while(a[i] && a[i] == b[i]) ++i;
   return a[i] == b[i];
}

// Matches arg against a long option that takes a value. Every valued long option accepts both spellings:
// "--name value" leaves value null, and "--name=value" points it at the tail. An option that merely shares
// a prefix, such as --media-dir against --media, does not match.
static cbool ArgMatchValue(cwchptr arg, cwchptr name, cwchptrptr value) {
   ui32 i = 0;

   while(name[i] && arg[i] == name[i]) ++i;
   if(name[i] || (arg[i] && arg[i] != L'=')) return false;

   *value = (arg[i] ? arg + i + 1 : nullptr);
   return true;
}

// Resolves the value of an option: the inline tail when the argument carried one, otherwise the next
// argument, which it consumes by advancing index. An empty or missing value is reported and refused.
static cbool TakeValue(csi32 argc, cwchptrcptr argv, si32ptrc index, cwchptr inlineVal, cwchptr option, cwchptrptr value) {
   if(inlineVal) {
      if(!inlineVal[0]) {
         DiagErrorText("option value is empty", option);
         return false;
      }
      *value = inlineVal;
      return true;
   }
   if(*index + 1 >= argc) {
      DiagErrorText("option requires a value", option);
      return false;
   }
   *index += 1;
   if(!argv[*index][0]) {
      DiagErrorText("option value is empty", option);
      return false;
   }
   *value = argv[*index];
   return true;
}

// Parses a NUL-terminated run of decimal digits. Anything else -- a sign, a space, an empty string, a
// value past 32 bits -- fails, so a mistyped count is reported rather than silently coerced.
static cbool ParseUi32(cwchptr text, ui32ptrc value) {
   ui64 result = 0;

   if(!text[0]) return false;
   for(ui32 i = 0; text[i]; ++i) {
      if(text[i] < L'0' || text[i] > L'9') return false;
      result = result * 10u + ui64(text[i] - L'0');
      if(result > 0xFFFFFFFFull) return false;
   }
   *value = ui32(result);
   return true;
}

// D7a's default worker count. GetSystemInfo's dwNumberOfProcessors and std::thread::hardware_concurrency
// both report only the calling thread's processor group, capping at 64 on large machines; this does not.
static cui32 DefaultThreadCount(void) {
   cui32 count = ui32(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));

   return (count ? count : 1u);
}

// Releases a partly built option set after a usage error, so the caller owns nothing when parsing fails.
static cEXIT_CODE CliParseFailed(CLI_OPTIONSptrc options) {
   CliFree(options);
   return EXIT_USAGE;
}

//== Entry points

cEXIT_CODE CliParse(csi32 argc, cwchptrcptr argv, CLI_OPTIONSptrc options) {
   options->inputs      = nullptr;
   options->outputPath  = nullptr;
   options->mediaDir    = nullptr;
   options->inputCount  = 0;
   options->threadCount = DefaultThreadCount();
   options->hardBreak   = HARD_BREAK_BACKSLASH;
   options->emitImages  = true;
   options->quiet       = false;
   options->toStdout    = false;
   options->showHelp    = false;
   options->showVersion = false;

   if(argc < 2) {
      DiagError("no input file given");
      return EXIT_USAGE;
   }

   // Worst case every argument after argv[0] is an input, so that many slots always suffice.
   options->inputs = (cwchptrptr)amalloc(sizeof(cwchptr) * size_t(argc - 1), 16u);
   if(!options->inputs) {
      // Not EXIT_USAGE: the command line was fine, so printing the usage text would blame the user.
      DiagError("out of memory while parsing the command line");
      return EXIT_INTERNAL;
   }

   for(si32 i = 1; i < argc; ++i) {
      cwchptr arg   = argv[i];
      cwchptr value = nullptr;

      // --help and --version answer the whole command line, so they are done the moment they are seen:
      // nothing after them is parsed, and neither an unreadable input nor a later typo can suppress them.
      if(StringEqual(arg, L"-h") || StringEqual(arg, L"--help")) {
         options->showHelp = true;
         return EXIT_ALL_CONVERTED;
      }
      if(StringEqual(arg, L"--version")) {
         options->showVersion = true;
         return EXIT_ALL_CONVERTED;
      }
      if(StringEqual(arg, L"-q") || StringEqual(arg, L"--quiet")) {
         options->quiet = true;
         continue;
      }
      if(StringEqual(arg, L"--stdout")) {
         options->toStdout = true;
         continue;
      }
      if(StringEqual(arg, L"--no-images")) {
         options->emitImages = false;
         continue;
      }
      if(StringEqual(arg, L"-o") || ArgMatchValue(arg, L"--output", &value)) {
         if(!TakeValue(argc, argv, &i, value, arg, &value)) return CliParseFailed(options);
         options->outputPath = value;
         continue;
      }
      if(ArgMatchValue(arg, L"--media-dir", &value)) {
         if(!TakeValue(argc, argv, &i, value, arg, &value)) return CliParseFailed(options);
         options->mediaDir = value;
         continue;
      }
      if(StringEqual(arg, L"-j") || ArgMatchValue(arg, L"--threads", &value)) {
         if(!TakeValue(argc, argv, &i, value, arg, &value)) return CliParseFailed(options);

         ui32 count = 0;

         if(!ParseUi32(value, &count) || !count) {
            DiagErrorText("--threads takes a whole number of at least 1, not", value);
            return CliParseFailed(options);
         }

         cui32 cores = DefaultThreadCount();

         if(count > cores) {
            char message[128];

            snprintf(message, sizeof(message), "--threads must be 1 to %u, the virtual core count this system reports", cores);
            DiagError(message);
            return CliParseFailed(options);
         }
         options->threadCount = count;
         continue;
      }
      if(ArgMatchValue(arg, L"--hard-break", &value)) {
         if(!TakeValue(argc, argv, &i, value, arg, &value)) return CliParseFailed(options);

         if(StringEqual(value, L"backslash")) options->hardBreak = HARD_BREAK_BACKSLASH;
         else if(StringEqual(value, L"spaces")) options->hardBreak = HARD_BREAK_SPACES;
         else {
            DiagErrorText("--hard-break takes backslash or spaces, not", value);
            return CliParseFailed(options);
         }
         continue;
      }
      if(!arg[0]) {
         DiagError("empty argument");
         return CliParseFailed(options);
      }
      if(arg[0] == L'-') {
         DiagErrorText("unrecognised option", arg);
         return CliParseFailed(options);
      }
      // D7b: every remaining operand is an input; there is no positional output operand.
      options->inputs[options->inputCount++] = arg;
   }

   if(!options->inputCount) {
      DiagError("no input file given");
      return CliParseFailed(options);
   }
   if(options->toStdout && options->inputCount > 1) {
      DiagError("--stdout takes a single input file");
      return CliParseFailed(options);
   }
   if(options->toStdout && options->outputPath) {
      DiagError("--stdout and --output cannot both be given");
      return CliParseFailed(options);
   }
   return EXIT_ALL_CONVERTED;
}

void CliFree(CLI_OPTIONSptrc options) {
   if(options->inputs) mdealloc(options->inputs);
   options->inputs     = nullptr;
   options->inputCount = 0;
}

void CliWriteUsage(cbool toStandardError) {
   if(toStandardError) DiagWriteErr(USAGE_TEXT);
   else DiagWriteOut(USAGE_TEXT);
}

void CliWriteVersion(void) { DiagWriteOut("DOCXtoMD " DOCXTOMD_VERSION "\n"); }
