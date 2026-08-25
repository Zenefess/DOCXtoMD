/*
 * File: Convert.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: The per-file conversion pipeline and the output-path derivation D7b's operand grammar needs.
 * To Do: 1) Extract referenced media beside the document when M7 adds MediaExtractor.
 *        2) Load numbering, footnotes and endnotes here as M8 and M10 give them models to go into.
 *        3) Hand this whole function to a worker when M13 adds the bounded pool (D6/D7a).
 * Dependencies: CliOptions.h, Diag.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "CliOptions.h"
#include "Diag.h"

//== Limits

/// The longest output path this converter will build, terminator included. Windows' own MAX_PATH is 260
/// and its extended-length form reaches about 32,767; this sits between them, because a derived name is
/// only ever an input's own path with its extension replaced, or a named directory with a leaf added.
constexpr cui64 CONVERT_MAX_PATH = 4096u;

//== Entry points

/// Converts one input file and writes its Markdown.
/// @param options    The parsed command line. It is read and never written, so several workers may share
///                   one of these by const reference from M13 (D6).
/// @param inputPath  The input, as wmain received it. Paths stay UTF-16 until Win32 or Diag.
/// @return EXIT_ALL_CONVERTED, or the per-file verdict: 2 when the input cannot be opened, 3 when it is
///         not a usable DOCX, 4 when the output cannot be written, 5 for this program's own failures.
///         Every failure has already been reported.
/// @note This is the whole pipeline for one document: container, package, styles, walk, emit, write. At
///       M13 it is what one worker runs, which is why it takes no shared state and returns a verdict
///       rather than setting one.
cEXIT_CODE ConvertFile(cCLI_OPTIONSptr options, cwchptr inputPath);

/// Derives the output path for one input.
/// @param inputPath         The input path as given.
/// @param outputPath        The -o value, or null when there was none.
/// @param outputIsDirectory Whether -o named a directory, which D7b decides by the number of inputs.
/// @param dest              Receives the derived path, NUL-terminated.
/// @param destChars         Characters available at dest, terminator included.
/// @return true when a path was derived, false when it would not fit or there was nothing to derive from.
/// @note The rule is D7b's: with one input, -o is the .md filename and is used exactly as written; with
///       several, -o is a directory and each input contributes its own stem; with no -o at all the
///       output sits beside its input with the extension replaced.
/// @note Both separators are recognised, because Windows accepts both and a command line carries either.
///       A separator is appended to a directory only when it needs one, and never after a colon: "C:" is
///       drive-relative and "C:\" is not, so adding one there would change which directory is meant.
/// @note Pure: it touches no file and allocates nothing, which is what makes it the piece the unit tests
///       can hammer directly.
cbool ConvertOutputPath(cwchptr inputPath, cwchptr outputPath, cbool outputIsDirectory, wchptrc dest, cui64 destChars);
