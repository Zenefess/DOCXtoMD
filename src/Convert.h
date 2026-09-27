/*
 * File: Convert.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-27
 * Description: The per-file conversion pipeline and the output-path derivation D7b's operand grammar needs.
 * To Do: 1) Say so when -o named an existing directory and one input made it a file name, which today
 *           reports only that the file could not be created.
 *        2) Compare an output path with the inputs by file identity once it exists, which would catch a
 *           link or a short name that a normalised string cannot.
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
   CONVERT_TARGET_REPEATED, ///< The same input was named earlier: one document, which is converted once
   CONVERT_TARGET_IS_SELF,  ///< The derived output is the input itself, spelled another way
   CONVERT_TARGET_COUNT
};

/// Constant form of CONVERT_TARGET, spelled per GCS r2: the qualifier lives in the typedef.
typedef const CONVERT_TARGET cCONVERT_TARGET;

//== Entry points

/// Converts one input file and writes its Markdown.
/// @param options    The parsed command line. It is read and never written, so Batch's workers share one
///                   of these by const reference (D6).
/// @param inputPath  The input, as wmain received it. Paths stay UTF-16 until Win32 or Diag.
/// @return EXIT_ALL_CONVERTED, or the per-file verdict: 2 when the input cannot be opened, 3 when it is
///         not a usable DOCX, 4 when the output cannot be written, 5 for this program's own failures.
///         Every failure has already been reported.
/// @note This is the whole pipeline for one document: container, package, styles, numbering, walk,
///       notes, resolve, emit, write. It is what one of Batch's workers runs (D6), which is why it takes
///       no shared state and returns a verdict rather than setting one: two calls on two threads share
///       nothing but options, which neither writes, and Diag, which locks.
/// @note The footnotes are walked after the body and the endnotes after the footnotes, and only the
///       notes something already walked references are read: an endnote cited from a footnote is found,
///       and a malformed notes part nothing cites costs the document nothing.
/// @note The order of the passes between the walk and the emitter is the one order that works, and
///       M8 and M10 each added to it. References resolve against the part they were read in -- a
///       note's against its own; note references take their labels in the order they are read; anchors
///       resolve once every reference is a destination; the coalescer runs a second time, because
///       muting a link or a reference makes two spans adjacent that were not; the media plan turns a
///       part name into a path and can turn a picture back into its alt text; the counter pass turns a
///       list reference into a marker and clears the ones that named nothing; and dropping the emptied
///       blocks comes last, because it is what restores the invariant the emitter rests on.
cEXIT_CODE ConvertFile(cCLI_OPTIONSptr options, cwchptr inputPath);

/// Whether converting one input would destroy something the rest of the run still needs.
/// @param options  The parsed command line. Batch hands in a copy whose paths it has normalised, so that
///                 two spellings of one file compare equal; the comparison here is literal either way.
/// @param index    Which input to test, as an index into options->inputs.
/// @param other    Receives the other input the answer is about -- the one this input repeats, the one
///                 it would destroy, or the earlier one that claims its output -- or index itself when
///                 the answer is CONVERT_TARGET_FREE or CONVERT_TARGET_IS_SELF. May be null.
/// @return CONVERT_TARGET_FREE when the input may be converted; otherwise why it may not be converted by
///         a worker of its own.
/// @note D7b derives every output name from an input's own leaf, so two inputs with the same leaf
///       name in different directories both target one .md, and without this check the second
///       silently overwrites the first -- two documents converted, one destroyed, exit 0. Argument
///       order decides: the first input to name a path keeps it and the later ones are refused, so
///       a run converts what it can and names what it could not.
/// @note One input named twice is not a collision but it is not two conversions either: two workers
///       writing one output file at once race for it, so a repeat is reported first, whatever else is
///       true of it, and Batch gives it the verdict of the earliest spelling rather than a worker.
/// @note Pure: it touches no file and allocates nothing. It is O(index) in derivations, which is
///       O(n^2) over a whole run -- a command line cannot hold enough operands for that to matter.
cCONVERT_TARGET ConvertTargetTaken(cCLI_OPTIONSptr options, cui32 index, ui32ptr other);

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
