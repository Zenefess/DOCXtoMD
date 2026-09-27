/*
 * File: Convert.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-27
 * Description: The per-file conversion pipeline and the output-path derivation D7b's operand grammar needs.
 * To Do: 1) Say so when -o named an existing directory and one input made it a file name, which today
 *           reports only that the file could not be created.
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

/// Whether two paths are one name, folding case the way a Windows file system does.
/// @param a  NUL-terminated path.
/// @param b  NUL-terminated path.
/// @return true when the two are equal under CompareStringOrdinal's ignore-case form, which reads the system's
///         own upper-case table: "Ete.md" and "ete.md" match, and so do the accented and Cyrillic pairs NTFS
///         folds, where folding A to Z alone would miss them.
/// @note A string comparison and not an identity test: ".\a.md" and "a.md" match only once both have been
///       normalised, and a short name or a link is not seen at all. Batch identifies a file that exists by
///       its volume and file ID and falls back to this only for paths that name nothing yet.
/// @note Pure: it touches no file and allocates nothing.
cbool ConvertSamePath(cwchptr a, cwchptr b);

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
/// @note Both separators are recognised, because Windows accepts both and a command line carries either,
///       and so is a drive letter's colon: "C:report.docx" is report.docx in the current directory of C:, so
///       its leaf is "report.docx" and a directory given by -o gains "report.md", not a name with a colon in
///       it -- which NTFS would read as an alternate data stream.
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
