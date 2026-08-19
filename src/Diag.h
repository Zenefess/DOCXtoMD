/*
 * File: Diag.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Diagnostic sink: UTF-8 stdout and stderr writers, and the stable process exit codes.
 * To Do: 1) Take include/spinlocks.h and become MT-safe when M13 gives every worker this one sink (D6).
 *        2) Add the note level that -q suppresses, once M5 gives the converter something to report.
 *        3) Collect the per-file failure list that exit code 6 summarises at M13 (D7c).
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Process exit codes

/// Process exit codes, and the value CliParse returns to say which kind of failure it hit.
/// @note Stable API: these values are contractual and scripts may depend on them. CLAUDE.md carries the
///       same table in prose; change neither without the other.
/// @note M2 can reach 0, 1, 2 and 5. The rest name verdicts the milestones that produce them will start
///       returning, and are declared now so the contract lives in one place.
enum EXIT_CODE : si32 {
   EXIT_ALL_CONVERTED = 0, ///< Every input was converted
   EXIT_USAGE         = 1, ///< The command line could not be understood
   EXIT_INPUT         = 2, ///< An input file was missing or could not be read
   EXIT_NOT_DOCX      = 3, ///< An input file was not a valid DOCX
   EXIT_OUTPUT        = 4, ///< An output file could not be written
   EXIT_INTERNAL      = 5, ///< The converter could not complete for a reason that is not the input's fault
   EXIT_PARTIAL       = 6  ///< At least one input converted and at least one failed
};

/// Constant form of EXIT_CODE, spelled per GCS r2: the qualifier lives in the typedef, not the identifier.
typedef const EXIT_CODE cEXIT_CODE;

//== Writers

/// Writes UTF-8 text to stdout verbatim, with no prefix and no added newline.
/// @param text  NUL-terminated UTF-8; a null pointer writes nothing.
/// @note The console is put into CP_UTF8 by wmain, so the same bytes suit a console and a redirected file.
void DiagWriteOut(cchptr text);

/// Writes UTF-8 text to stderr verbatim, with no prefix and no added newline.
/// @param text  NUL-terminated UTF-8; a null pointer writes nothing.
void DiagWriteErr(cchptr text);

/// Writes one error line to stderr: "DOCXtoMD: error: <message>".
/// @param message  NUL-terminated UTF-8 sentence fragment, without a trailing newline.
/// @note stdout is flushed first, so an error never overtakes text already written to stdout.
void DiagError(cchptr message);

/// Writes one error line to stderr naming a wide argument: "DOCXtoMD: error: <message>: <text>".
/// @param message  NUL-terminated UTF-8 sentence fragment, without a trailing newline.
/// @param text     Wide text -- a path or an option -- transcoded to UTF-8 for the console.
/// @note An untranscodable argument is replaced by a placeholder rather than suppressing the whole line.
void DiagErrorText(cchptr message, cwchptr text);
