/*
 * File: Diag.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-09-27
 * Description: Diagnostic sink: stdout and stderr writers that hold one spin lock per line, and the exit codes.
 * To Do: 1) Take over -q from the callers, so a note is suppressed in one place rather than at each site.
 *        2) Buffer the document written by DiagWriteOutBytes, once a document is large enough to want it.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: MT-safe
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Process exit codes

/// Process exit codes, and the value CliParse returns to say which kind of failure it hit.
/// @note Stable API: these values are contractual and scripts may depend on them. CLAUDE.md carries the
///       same table in prose; change neither without the other.
/// @note 2 to 5 are also the per-input verdicts a conversion returns. With one input the verdict is the
///       exit code; with several, Batch folds them (D7c).
enum EXIT_CODE : si32 {
   EXIT_ALL_CONVERTED = 0, ///< Every input was converted
   EXIT_USAGE         = 1, ///< The command line could not be understood
   EXIT_INPUT         = 2, ///< An input file was missing or could not be read
   EXIT_NOT_DOCX      = 3, ///< An input file was not a valid DOCX
   EXIT_OUTPUT        = 4, ///< An output file could not be written
   EXIT_INTERNAL      = 5, ///< The converter could not complete for a reason that is not the input's fault
   EXIT_PARTIAL       = 6  ///< At least one input converted and at least one failed
};

/// Constant and pointer forms of EXIT_CODE, spelled per GCS r2/t2: a leading c binds the pointee, a
/// trailing c binds the pointer.
typedef const EXIT_CODE        cEXIT_CODE;
typedef EXIT_CODE             *EXIT_CODEptr;
typedef const EXIT_CODE       *cEXIT_CODEptr;
typedef EXIT_CODE *const       EXIT_CODEptrc;
typedef const EXIT_CODE *const cEXIT_CODEptrc;

//== Locking contract

// Every writer below is MT-safe from M13, when Batch's workers all report through this one sink (D6).
// Each holds one process-wide spin lock -- SpinLockMin, because every write is a console or pipe
// syscall that can block for milliseconds on a redirected handle -- for exactly one call, so a line
// from one worker is never interleaved inside a line from another. The lock is not reentrant, and no
// writer calls another while it holds it. Which of two workers' lines comes first is not promised.

//== Writers

/// Writes UTF-8 text to stdout verbatim, with no prefix and no added newline.
/// @param text  NUL-terminated UTF-8; a null pointer writes nothing.
/// @note The console is put into CP_UTF8 by wmain, so the same bytes suit a console and a redirected file.
void DiagWriteOut(cchptr text);

/// Writes UTF-8 bytes to stdout verbatim, with no newline translation whatever.
/// @param bytes      The bytes; a null pointer writes nothing.
/// @param byteCount  How many.
/// @return true when every byte reached the handle. A caller handing over a converted document must
///         treat false as an output failure: stdout is the only copy, and a caller redirecting it into
///         a file has no other way to learn the document was lost.
/// @note This is the door --stdout hands a converted document through, and it deliberately bypasses the
///       C runtime's stream: stdout is a text stream on Windows, so fwrite would turn every LF in the
///       document into a CRLF and break the emitter's stated output contract. Buffered output already
///       queued on stdout is flushed first, so ordering is kept.
/// @note The lock is held for the whole document, which --stdout's single input makes harmless.
cbool DiagWriteOutBytes(cui8ptr bytes, cui64 byteCount);

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
/// @note The argument is transcoded before the lock is taken, so the lock is held only for the writes.
void DiagErrorText(cchptr message, cwchptr text);

/// Writes one progress line to stderr: "DOCXtoMD: note: <message>".
/// @param message  NUL-terminated UTF-8 sentence fragment, without a trailing newline.
/// @note -q suppresses notes, and until this module owns that flag the caller is what decides not to call.
void DiagNote(cchptr message);

/// Writes one progress line to stderr naming a wide argument: "DOCXtoMD: note: <message>: <text>".
/// @param message  NUL-terminated UTF-8 sentence fragment, without a trailing newline.
/// @param text     Wide text -- a path or an option -- transcoded to UTF-8 for the console.
/// @note Notes go to stderr, not stdout, so that --stdout can hand a document to a pipe uncontaminated.
/// @note -q suppresses notes, and until this module owns that flag the caller is what decides not to call.
void DiagNoteText(cchptr message, cwchptr text);

//== Names

/// A short phrase naming what an exit code means, for the list of failed inputs Batch writes (D7c).
/// @param code  Any value; one outside the table is named "an unknown verdict" rather than read past it.
/// @return A NUL-terminated ASCII phrase with no trailing punctuation. Never null.
cchptr DiagExitCodeText(cEXIT_CODE code);
