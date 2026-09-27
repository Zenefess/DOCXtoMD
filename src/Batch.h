/*
 * File: Batch.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-27
 * Last Modified: 2026-09-27
 * Description: The input list on a bounded pool of worker threads, one file per worker at a time, and the exit-code fold.
 * To Do: 1) Say which input a claimed output or --media-dir belongs to, which the plan knows and the message omits.
 *        2) Give a shared --media-dir its per-input subdirectories if decision D15 is ruled that way.
 * Dependencies: CliOptions.h, Diag.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: MT-safe
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "CliOptions.h"
#include "Diag.h"

//== Types

/// One unit of pool work.
/// @param context  Whatever the caller handed BatchPool; every worker reads the same one, so it must not
///                 be written while the pool runs.
/// @param item     Which item, 0 to itemCount - 1. Each is run exactly once, by exactly one thread.
/// @return The item's verdict, which BatchPool stores at the item's own index.
typedef cEXIT_CODE (*BATCH_JOB)(cptr context, cui32 item);

/// What the pre-flight decided for one input, before any worker starts.
enum BATCH_PLAN : ui8 {
   BATCH_PLAN_CONVERT = 0, ///< A worker converts it
   BATCH_PLAN_REPEAT,      ///< Named earlier on the line: not converted again, and given that input's verdict
   BATCH_PLAN_IS_INPUT,    ///< Refused: its output path is another input of the run, which it would destroy
   BATCH_PLAN_CLAIMED,     ///< Refused: an earlier input writes the same output file
   BATCH_PLAN_MEDIA,       ///< Refused: an earlier input already extracts its pictures into the one --media-dir
   BATCH_PLAN_IS_SELF,     ///< Refused: its output path is the input itself, spelled another way
   BATCH_PLAN_COUNT
};

/// Pointer aliases for BATCH_PLAN, spelled per GCS r2/t2.
typedef BATCH_PLAN       *BATCH_PLANptr;
typedef const BATCH_PLAN *cBATCH_PLANptr;
typedef BATCH_PLAN *const BATCH_PLANptrc;
typedef const BATCH_PLAN  cBATCH_PLAN;

//== Entry points

/// Converts every input the command line names and folds their verdicts into the process exit code.
/// @param options  The parsed command line. Read-only from here on, because every worker shares it (D6).
/// @return EXIT_ALL_CONVERTED when every input converted, EXIT_PARTIAL when at least one converted and at
///         least one did not, and otherwise the highest per-input verdict (D7c). EXIT_INTERNAL, having said
///         so, when the run could not be planned for want of memory.
/// @note The pre-flight runs first, on the calling thread and in argument order, over a copy of the paths
///       normalised by GetFullPathNameW, so ".\a.docx" and "a.docx" are one input and "out\" and "out/"
///       one directory. Its refusals are reported before any worker starts. It is deterministic by
///       construction, which is what lets the same command line produce the same bytes and the same exit
///       code at every --threads count. A single input goes through it too, because "-o .\a.docx a.docx"
///       would otherwise write the Markdown over the document it came from.
/// @note A worker is a thread converting one input at a time until none is left; there are at most
///       --threads of them, the calling thread included, so --threads 1 and a single input start no
///       thread at all and convert in argument order exactly as the loop this replaced did.
/// @note With more than one input and at least one failure, every failed input is listed once more after
///       the last worker has finished, in argument order with its own verdict, so exit code 6 is always a
///       summary and never the only diagnosis (D7c). The per-input messages themselves are written as each
///       worker reaches them, one whole line at a time, in no promised order.
cEXIT_CODE BatchRun(cCLI_OPTIONSptr options);

/// Decides, for every input, whether a worker converts it -- the pure half of BatchRun's pre-flight.
/// @param options  The command line, with its paths already normalised when normalising matters.
/// @param plans    Receives one plan per input.
/// @param sources  Receives one index per input: the input a repeat repeats, the input a refusal is about,
///                 and the input's own index for BATCH_PLAN_CONVERT.
/// @note A shared --media-dir -- one named with several inputs and pictures not turned off -- belongs to the
///       first input planned for conversion, and every later one is refused, because each document names
///       its pictures image1, image2 and so on and two of them in one directory overwrite each other's.
///       That is the strict half of decision D15, which is open; the lenient half would give each input a
///       directory of its own under the named one.
/// @note Pure: it touches no file and allocates nothing.
void BatchPlan(cCLI_OPTIONSptr options, BATCH_PLANptrc plans, ui32ptrc sources);

/// Runs a job once for each item on a bounded pool of threads, the calling thread included.
/// @param job          The work.
/// @param context      Handed to every call of job.
/// @param itemCount    How many items; below 2^31, which a command line cannot approach.
/// @param threadCount  The most threads that may run jobs at once, the calling thread included; 0 reads as 1.
/// @param verdicts     Receives job's verdict for each item, at the item's own index.
/// @return How many threads ran jobs, the calling thread included: the smaller of threadCount and itemCount,
///         fewer when a thread could not be started, and 0 when there were no items.
/// @note Items are handed out in item order through one interlocked cursor, so a thread that finishes early
///       takes the next item rather than waiting for a share decided up front. A thread that cannot be
///       started leaves a smaller pool rather than a failed run: the calling thread alone completes the work.
/// @note Memory order: starting a thread and waiting for one to finish are both full barriers on Windows,
///       and taking an item is an _InterlockedIncrement, which is one too. So whatever the caller wrote
///       before the call is seen by every job, and every verdict a job returns is seen by the caller once
///       this returns. A verdict slot is written by exactly one thread and read only after the wait.
cui32 BatchPool(BATCH_JOB job, cptr context, cui32 itemCount, cui32 threadCount, EXIT_CODEptrc verdicts);

/// Folds per-input verdicts into the process exit code (D7c).
/// @param verdicts  One verdict per input.
/// @param count     How many.
/// @return EXIT_ALL_CONVERTED when every verdict is, including when there are none; EXIT_PARTIAL when some
///         are and some are not; otherwise the highest verdict among them.
cEXIT_CODE BatchFold(cEXIT_CODEptr verdicts, cui32 count);
