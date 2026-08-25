/*
 * File: CliOptions.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-25
 * Description: Parsed command line, the hard-break policy, and the usage and version writers.
 * To Do: 1) Consume the options the converter does not read yet: --media-dir and --no-images.
 *        2) Hand the input list and --threads count to Batch when M13 adds the bounded worker pool (D7a).
 *        3) Add the remaining policy switches CONVERSION_REFERENCE.md 6.3 lists, once their stages exist.
 * Dependencies: Diag.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Diag.h"

// Reported by --version. A macro, not a constant, so it concatenates into a single string literal.
#define DOCXTOMD_VERSION "0.1.0"

//== Option values

/// How a w:br of type textWrapping is rendered, chosen by --hard-break.
/// @note M5's emitter is the first thing to read it; M2 recorded it and nothing acted on it.
enum HARD_BREAK : ui8 {
   HARD_BREAK_BACKSLASH = 0, ///< Trailing backslash; the default
   HARD_BREAK_SPACES    = 1  ///< Two trailing spaces
};

/// Constant form of HARD_BREAK, spelled per GCS r2: the qualifier lives in the typedef.
typedef const HARD_BREAK cHARD_BREAK;

//== Parsed command line

/// One parsed command line. Filled by CliParse, read-only afterwards.
/// @note D7b: every operand is an input and there is no positional output, so the inputs are a list from
///       the first commit. -o names a file when there is exactly one input, a directory when several.
/// @note From M13 the workers share one of these by const reference, so nothing may mutate it once
///       CliParse returns (D6).
struct CLI_OPTIONS {
   cwchptrptr inputs;      ///< Input paths in command-line order; allocated by CliParse, freed by CliFree
   cwchptr    outputPath;  ///< -o/--output; null when absent
   cwchptr    mediaDir;    ///< --media-dir; null selects the per-input <stem>_media default
   ui32       inputCount;  ///< Number of entries in inputs
   ui32       threadCount; ///< -j/--threads; defaults to the system virtual core count (D7a)
   HARD_BREAK hardBreak;   ///< --hard-break
   bool       emitImages;  ///< Cleared by --no-images, which keeps alt text only
   bool       quiet;       ///< -q/--quiet: errors only
   bool       toStdout;    ///< --stdout; legal only with exactly one input (D7d)
   bool       showHelp;    ///< -h/--help
   bool       showVersion; ///< --version
};

/// Pointer aliases for CLI_OPTIONS, spelled per GCS r2/t2: a leading c binds the pointee, a trailing c
/// binds the pointer.
typedef CLI_OPTIONS             *CLI_OPTIONSptr;
typedef const CLI_OPTIONS       *cCLI_OPTIONSptr;
typedef CLI_OPTIONS *const       CLI_OPTIONSptrc;
typedef const CLI_OPTIONS *const cCLI_OPTIONSptrc;

//== Entry points

/// Parses a wmain command line into an option set.
/// @param argc     Argument count exactly as wmain received it.
/// @param argv     Argument vector exactly as wmain received it; argv[0] is skipped.
/// @param options  Receives the parsed options; every field is written, so it need not be initialised.
/// @return EXIT_ALL_CONVERTED when the command line parsed, EXIT_USAGE when it did not, or EXIT_INTERNAL
///         when the input list could not be allocated. Every failure has already been reported.
/// @note On EXIT_ALL_CONVERTED the caller owns options->inputs and must release it with CliFree; on any
///       other result nothing is owned and CliFree must not be called.
/// @note An allocation failure is deliberately not EXIT_USAGE: printing the usage text for it would blame
///       the user for the tool's problem.
/// @note -h, --help and --version are answered the moment they are seen, so they beat anything later on
///       the line, including a missing or unreadable input. A bad option earlier on the line still wins.
cEXIT_CODE CliParse(csi32 argc, cwchptrcptr argv, CLI_OPTIONSptrc options);

/// Releases the input list held by an option set and clears the list fields. Safe to call twice.
/// @param options  Options previously filled by a successful CliParse.
void CliFree(CLI_OPTIONSptrc options);

/// Writes the usage text, terminated by a newline.
/// @param toStandardError  true routes it to stderr, as a usage error should; false to stdout, as --help does.
void CliWriteUsage(cbool toStandardError);

/// Writes the version line to stdout, terminated by a newline.
void CliWriteVersion(void);
