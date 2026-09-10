/*
 * File: Convert.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-27
 * Description: The per-file conversion pipeline and the output-path derivation D7b's operand grammar needs.
 * To Do: 1) Load numbering, footnotes and endnotes here as M8 and M10 give them models to go into.
 *        2) Hand this whole function to a worker when M13 adds the bounded pool (D6/D7a).
 *        3) Say so when -o named an existing directory and one input made it a file name, which today
 *           reports only that the file could not be created.
 *        4) Pre-flight a --media-dir shared by several inputs, the way ConvertTargetTaken pre-flights
 *           a shared output name; the architecture note gives both to Batch at M13.
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

//== Types

/// What another input in the same run has already claimed of one input's derived output path.
enum CONVERT_TARGET : si32 {
   CONVERT_TARGET_FREE,     ///< Nothing else in the run needs that path
   CONVERT_TARGET_IS_INPUT, ///< The derived output is another input of this run, and would destroy it
   CONVERT_TARGET_CLAIMED,  ///< An earlier input derives the same output, so this one would overwrite it
   CONVERT_TARGET_COUNT
};

/// Constant form of CONVERT_TARGET, spelled per GCS r2: the qualifier lives in the typedef.
typedef const CONVERT_TARGET cCONVERT_TARGET;

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

/// Whether converting one input would destroy something the rest of the run still needs.
/// @param options  The parsed command line.
/// @param index    Which input to test, as an index into options->inputs.
/// @return CONVERT_TARGET_FREE when the input may be converted; otherwise why it may not be.
/// @note D7b derives every output name from an input's own leaf, so two inputs with the same leaf
///       name in different directories both target one .md, and without this check the second
///       silently overwrites the first -- two documents converted, one destroyed, exit 0. Argument
///       order decides: the first input to name a path keeps it and the later ones are refused, so
///       a run converts what it can and names what it could not. The architecture note that
///       recommends this pre-flight gives it to Batch at M13; the loop in main.cpp is what Batch
///       replaces, so it is done there until then.
/// @note Pure: it touches no file and allocates nothing. It is O(index) in derivations, which is
///       O(n^2) over a whole run -- a command line cannot hold enough operands for that to matter.
cCONVERT_TARGET ConvertTargetTaken(cCLI_OPTIONSptr options, cui32 index);

/// Derives the output path for one input.
/// @param inputPath         The input path as given.
/// @param outputPath        The -o value, or null when there was none.
/// @param outputIsDirectory Whether -o named a directory, which D7d decides by the number of inputs.
/// @param dest              Receives the derived path, NUL-terminated.
/// @param destChars         Characters available at dest, terminator included.
/// @return true when a path was derived, false when it would not fit or there was nothing to derive from.
/// @note The rule is D7d's: with one input, -o is the .md filename and is used exactly as written; with
///       several, -o is a directory and each input contributes its own stem; with no -o at all the
///       output sits beside its input with the extension replaced.
/// @note Both separators are recognised, because Windows accepts both and a command line carries either.
///       A separator is appended to a directory only when it needs one, and never after a colon: "C:" is
///       drive-relative and "C:\" is not, so adding one there would change which directory is meant.
/// @note Pure: it touches no file and allocates nothing, which is what makes it the piece the unit tests
///       can hammer directly.
cbool ConvertOutputPath(cwchptr inputPath, cwchptr outputPath, cbool outputIsDirectory, wchptrc dest, cui64 destChars);

/// Derives the directory a document's pictures go in, and the path that reaches them from the document.
/// @param documentPath  The document's own path: the .md, or the input when --stdout means there is no .md.
/// @param named         The --media-dir value, or null when there was none.
/// @param dir           Receives the directory, NUL-terminated, as a path Win32 will accept.
/// @param dirChars      Characters available at dir, terminator included.
/// @param prefix        Receives the UTF-8 path that stands in front of a file name in the document,
///                      with the Windows separator folded to the one a URL uses, NUL-terminated.
/// @param prefixChars   Bytes available at prefix, terminator included.
/// @return true when both were derived, false when neither will fit or there is nothing to derive from.
/// @note With no --media-dir the directory is the document's own stem with "_media" on it, sitting
///       beside the document, so the path a reader follows is a single leaf and keeps working wherever
///       the pair is moved to. With --media-dir the directory is exactly what was asked for and the
///       path is the same string, which is relative to the working directory rather than to the
///       document: the user chose it, and second-guessing a path they typed would be worse.
/// @note Pure: it touches no file and allocates nothing.
cbool ConvertMediaDir(cwchptr documentPath, cwchptr named, wchptrc dir, cui64 dirChars, chptrc prefix, cui64 prefixChars);
