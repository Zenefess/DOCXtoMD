/*
 * File: Batch.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-27
 * Last Modified: 2026-09-27
 * Description: Worker pool, run plan, failure list and exit-code fold; no lock: one interlocked cursor, verdicts read after the join.
 * To Do: 1) Measure the pool against the sequential loop in bench/ before anything here claims a speed-up (bd1/bd2).
 *        2) Bound the pool by memory as well as by --threads, once a process-wide budget is ruled on: each worker holds
 *           a whole document, and the ZIP caps are per document.
 * Dependencies: BuildGuards.h, Batch.h, CliOptions.h, Convert.h, Diag.h, typedefs.h, memory management.h,
 *               windows.h, process.h, intrin.h, stdio.h
 * ISA: Scalar
 * Thread-safety: MT-safe
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros, and memory management.h pulls those two in that order itself.
#include <windows.h>
#include <process.h>
#include <intrin.h>
#include <stdio.h>
#include "typedefs.h"
#include "memory management.h"
#include "CliOptions.h"
#include "Convert.h"
#include "Diag.h"
#include "Batch.h"

//-- Types

// The interlocked intrinsics take a volatile LONG, which is 32 bits on every Windows target. typedefs.h has
// no alias for it, so this translation unit names one rather than spelling the qualifier at each use (r2).
typedef volatile LONG vLONG;

// The one array of thread handles, spelled per r2.
typedef HANDLE *HANDLEptr;

// The cursor every worker takes its next item from. It has a cache line to itself because it is the one
// location every worker writes, and a line shared with the read-only fields beside it would false-share.
struct al64 BATCH_CURSOR {
   vLONG next; ///< How many items have been taken, or tried for, so far
};

// Pointer aliases for the pre-flight's records, spelled per r2.
typedef BATCH_ID *const    BATCH_IDptrc;
typedef BATCH_INPUT *const BATCH_INPUTptrc;

// Everything a worker reads. It lives on the stack of the thread that called BatchPool, which outlives
// every worker because it waits for all of them before it returns.
struct BATCH_POOL {
   BATCH_CURSOR cursor;    ///< Written by every worker, and only ever through _InterlockedIncrement
   BATCH_JOB    job;       ///< Read-only while the pool runs
   cptr         context;   ///< Read-only while the pool runs
   EXIT_CODEptr verdicts;  ///< One slot per item, each written by exactly one worker
   ui32         itemCount; ///< Read-only while the pool runs
};

typedef BATCH_POOL       *BATCH_POOLptr;
typedef BATCH_POOL *const BATCH_POOLptrc;

// What BatchRun's job reads: the command line and which of its inputs the plan let through.
struct BATCH_WORK {
   cCLI_OPTIONSptr options; ///< The command line as the user wrote it: every file and message uses its spellings
   cui32ptr        queue;   ///< Indexes into options->inputs of the inputs a worker converts, in argument order
};

typedef const BATCH_WORK *cBATCH_WORKptr;

//-- Messages

// What the pre-flight says about an input it does not hand a worker, kept together so that every refusal
// sentence can be read in one place. The repeat's note claims no outcome, because the earliest spelling's
// is not known when it is written -- that spelling may be refused, or fail.
constexpr cchptr TAKEN_BY_SELF    = "the output path is the input file";
constexpr cchptr TAKEN_BY_INPUT   = "the output path is another input of this run";
constexpr cchptr TAKEN_BY_EARLIER = "an earlier input already writes that output file";
constexpr cchptr TAKEN_MEDIA_DIR  = "an earlier input of this run already uses that --media-dir";
constexpr cchptr NAMED_TWICE      = "the same file as an earlier input, so it takes that input's result";

//-- Workers

// Runs items until none is left. Every thread of the pool runs this, the calling thread included.
static void BatchDrain(BATCH_POOLptrc pool) {
   for(;;) {
      // A full barrier (LOCK XADD), and the only location two workers ever write. The cursor starts at 0
      // and the intrinsic returns the incremented value, so the item this worker has taken is one less.
      cui32 item = ui32(_InterlockedIncrement(&pool->cursor.next)) - 1u;

      if(item >= pool->itemCount) return;
      pool->verdicts[item] = pool->job(pool->context, item);
   }
}

// The entry point _beginthreadex starts. Its shape is the C runtime's; what it returns is never read.
static ui32 __stdcall BatchWorker(ptr argument) {
   BatchDrain((BATCH_POOLptr)argument);
   return 0;
}

// BatchRun's job: one input, converted end to end by whichever worker took it.
static cEXIT_CODE BatchConvertOne(cptr context, cui32 item) {
   cBATCH_WORKptr work = (cBATCH_WORKptr)context;

   return ConvertFile(work->options, work->options->inputs[work->queue[item]]);
}

//-- The pre-flight's paths

// Characters, terminator included, that one path needs normalised -- or its literal spelling needs, when
// that is more, since the literal spelling is what is kept if normalising fails.
static cui64 BatchFullChars(cwchptr path) {
   cui64 needed = ui64(GetFullPathNameW(path, 0, nullptr, nullptr));
   ui64  length = 0;

   while(path[length]) ++length;
   return (needed > length + 1u ? needed : length + 1u);
}

// Normalises one path into dest, which holds chars characters, and returns the character after its
// terminator: GetFullPathNameW's absolute form with every separator a backslash and every "." and ".."
// resolved, or the literal spelling when Win32 cannot normalise it.
static wchptr BatchFullPath(cwchptr path, wchptrc dest, cui64 chars) {
   ui64 written = ui64(GetFullPathNameW(path, DWORD(chars), dest, nullptr));

   if(!written || written >= chars) {
      written = 0;
      while(path[written]) {
         dest[written] = path[written];
         ++written;
      }
      dest[written] = 0;
   }
   return dest + written + 1u;
}

// Which file a path reaches, when it reaches one. FILE_READ_ATTRIBUTES is all the handle needs and every
// share mode is granted, so this cannot get in the way of anything; backup semantics lets a directory be
// opened too. FileIdInfo is the full 128-bit ID ReFS needs; a file system that cannot report it falls back
// to the 64-bit index, which is unique on NTFS and FAT for as long as the file exists.
static void BatchIdentify(cwchptr path, BATCH_IDptrc id) {
   id->volume = 0;
   id->low    = 0;
   id->high   = 0;
   id->known  = false;
   if(!path) return;

   cDWORD  shared = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
   cHANDLE file   = CreateFileW(path, FILE_READ_ATTRIBUTES, shared, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);

   if(file == INVALID_HANDLE_VALUE) return;

   FILE_ID_INFO               wide;
   BY_HANDLE_FILE_INFORMATION narrow;

   if(GetFileInformationByHandleEx(file, FileIdInfo, &wide, sizeof(wide))) {
      id->volume = ui64(wide.VolumeSerialNumber);
      for(ui32 at = 0; at < 8u; ++at) {
         id->low |= ui64(wide.FileId.Identifier[at]) << (8u * at);
         id->high |= ui64(wide.FileId.Identifier[at + 8u]) << (8u * at);
      }
      id->known = true;
   } else if(GetFileInformationByHandle(file, &narrow)) {
      id->volume = ui64(narrow.dwVolumeSerialNumber);
      id->low    = (ui64(narrow.nFileIndexHigh) << 32) | ui64(narrow.nFileIndexLow);
      id->known  = true;
   }
   CloseHandle(file);
}

// Gathers what the pre-flight compares, into entries and one arena of normalised strings the caller
// releases; null when the arena could not be allocated. Each output is derived from the input as typed,
// because that is the spelling ConvertFile derives from, and only then normalised: normalising the input
// first can change its stem -- Win32 drops a trailing dot, so "a.docx." opens a.docx but derives
// a.docx.md -- and the plan would judge a path no worker writes. GetFullPathNameW reads the current
// directory, which is process-wide state, so this runs before any worker starts; nothing in this program
// changes the directory, so the answer is the one every conversion then sees.
static wchptr BatchGather(cCLI_OPTIONSptr options, BATCH_INPUTptrc entries) {
   wchar derived[CONVERT_MAX_PATH];
   cbool several = options->inputCount > 1u;
   ui64  chars   = 0;

   // Measured first and filled second, so that the arena is one allocation of the size it needs.
   for(ui32 index = 0; index < options->inputCount; ++index) {
      chars += BatchFullChars(options->inputs[index]);
      if(options->toStdout) continue;
      if(ConvertOutputPath(options->inputs[index], options->outputPath, several, derived, CONVERT_MAX_PATH)) chars += BatchFullChars(derived);
   }

   wchptr arena = (wchptr)amalloc(sizeof(wchar) * size_t(chars ? chars : 1u), 16u);

   if(!arena) return nullptr;

   wchptr at = arena;

   for(ui32 index = 0; index < options->inputCount; ++index) {
      BATCH_INPUTptrc entry = entries + index;

      entry->input  = at;
      at            = BatchFullPath(options->inputs[index], at, BatchFullChars(options->inputs[index]));
      entry->output = nullptr;
      BatchIdentify(options->inputs[index], &entry->self);
      BatchIdentify(nullptr, &entry->target);
      // --stdout writes no file, and an output that cannot be derived fails in ConvertFile on its own.
      if(options->toStdout) continue;
      if(!ConvertOutputPath(options->inputs[index], options->outputPath, several, derived, CONVERT_MAX_PATH)) continue;
      entry->output = at;
      at            = BatchFullPath(derived, at, BatchFullChars(derived));
      BatchIdentify(derived, &entry->target);
   }
   return arena;
}

// Whether two paths reach one file, by the rule BatchPlan's header states.
static cbool BatchSameFile(cwchptr a, cBATCH_IDptr aId, cwchptr b, cBATCH_IDptr bId) {
   if(aId->known && bId->known) return aId->volume == bId->volume && aId->low == bId->low && aId->high == bId->high;
   if(aId->known || bId->known) return false; // One file exists and the other does not, so they are two
   return a && b && ConvertSamePath(a, b);
}

//-- What a run of several inputs says at its end

// How many inputs converted and how many workers the pool held -- the one line of output that shows the
// pool was as wide as --threads asked, since every byte a worker writes is the same at any width.
static void BatchNoteWorkers(cui32 converted, cui32 count, cui32 workers) {
   char line[128];

   snprintf(line, sizeof(line), "%u of %u inputs converted by %u worker%s", converted, count, workers, (workers == 1u ? "" : "s"));
   DiagNote(line);
}

// D7c: every input that failed, once more, in argument order and with its own verdict, after the last
// worker has finished -- so that a run whose per-input messages arrived interleaved still ends with one
// list a person can read, and exit code 6 is a summary rather than the only diagnosis.
static void BatchListFailures(cCLI_OPTIONSptr options, cEXIT_CODEptr verdicts, cui32 failed) {
   char line[128];

   snprintf(line, sizeof(line), "%u of %u inputs failed:", failed, options->inputCount);
   DiagError(line);
   for(ui32 index = 0; index < options->inputCount; ++index) {
      if(verdicts[index] == EXIT_ALL_CONVERTED) continue;
      snprintf(line, sizeof(line), "failed (exit %d, %s)", si32(verdicts[index]), DiagExitCodeText(verdicts[index]));
      DiagErrorText(line, options->inputs[index]);
   }
}

//== Entry points

cui32 BatchPool(BATCH_JOB job, cptr context, cui32 itemCount, cui32 threadCount, EXIT_CODEptrc verdicts) {
   if(!itemCount) return 0;

   BATCH_POOL pool;

   pool.cursor.next = 0;
   pool.job         = job;
   pool.context     = context;
   pool.verdicts    = verdicts;
   pool.itemCount   = itemCount;

   cui32 asked   = (threadCount ? threadCount : 1u);
   cui32 wanted  = (asked < itemCount ? asked : itemCount);
   ui32  started = 0;
   // The calling thread is the first worker, so only the others are started, and a pool of one starts
   // nothing: the single-file case stays single-threaded (D6). A handle array that cannot be allocated
   // is a pool of one, which is slower and otherwise the same.
   HANDLEptr threads = (wanted > 1u ? (HANDLEptr)amalloc(sizeof(HANDLE) * size_t(wanted - 1u), 16u) : nullptr);

   for(ui32 index = 1; threads && index < wanted; ++index) {
      // _beginthreadex and not CreateThread: every job calls the C runtime -- _aligned_malloc through
      // amalloc, and the file writes -- and a thread CreateThread starts leaks the runtime's per-thread
      // block. A stack size of 0 is the executable's own default, the reserve the calling thread has.
      cHANDLE thread = (HANDLE)_beginthreadex(nullptr, 0u, BatchWorker, &pool, 0u, nullptr);

      if(!thread) break; // A smaller pool, never a failed run: the threads already running do the rest
      threads[started++] = thread;
   }
   BatchDrain(&pool);
   // One wait per thread rather than WaitForMultipleObjects, which stops at 64 handles and --threads does
   // not. Each wait is a full barrier, so every verdict that thread wrote is visible from here on.
   for(ui32 index = 0; index < started; ++index) {
      WaitForSingleObject(threads[index], INFINITE);
      CloseHandle(threads[index]);
   }
   if(threads) mdealloc(threads);
   return started + 1u;
}

void BatchPlan(cBATCH_INPUTptr inputs, cui32 count, cbool sharedMedia, BATCH_PLANptrc plans, ui32ptrc sources) {
   bool claimed = false;
   ui32 owner   = 0;

   for(ui32 index = 0; index < count; ++index) {
      cBATCH_INPUTptr mine  = inputs + index;
      BATCH_PLAN      plan  = BATCH_PLAN_CONVERT;
      ui32            other = index;

      // A repeat is settled first, because it is the same document rather than another one: whatever the
      // earliest spelling's output collides with, this one's collides with too, and the earliest spelling
      // is what reports it. It is the earliest, so a repeat's source is never itself a repeat.
      for(ui32 at = 0; plan == BATCH_PLAN_CONVERT && at < index; ++at) {
         if(!BatchSameFile(mine->input, &mine->self, inputs[at].input, &inputs[at].self)) continue;
         plan  = BATCH_PLAN_REPEAT;
         other = at;
      }
      // An output that is the input itself would be written over the document being read.
      if(plan == BATCH_PLAN_CONVERT && mine->output && BatchSameFile(mine->output, &mine->target, mine->input, &mine->self)) {
         plan = BATCH_PLAN_IS_SELF;
      }
      // Then another input it would destroy, anywhere on the line, or an earlier input's output it would
      // overwrite -- D7b derives every name from an input's own leaf, so two report.docx in two directories
      // both target one report.md. The first input to name a path keeps it, whatever becomes of that input.
      for(ui32 at = 0; plan == BATCH_PLAN_CONVERT && mine->output && at < count; ++at) {
         cBATCH_INPUTptr theirs = inputs + at;

         if(at != index && BatchSameFile(mine->output, &mine->target, theirs->input, &theirs->self)) plan = BATCH_PLAN_IS_INPUT;
         else if(at < index && theirs->output && BatchSameFile(mine->output, &mine->target, theirs->output, &theirs->target)) {
            plan = BATCH_PLAN_CLAIMED;
         }
         if(plan != BATCH_PLAN_CONVERT) other = at;
      }
      if(plan == BATCH_PLAN_CONVERT && sharedMedia) {
         // Decision D15's strict half: the first input planned for conversion owns the directory.
         if(claimed) {
            plan  = BATCH_PLAN_MEDIA;
            other = owner;
         } else {
            claimed = true;
            owner   = index;
         }
      }
      plans[index]   = plan;
      sources[index] = other;
   }
}

cEXIT_CODE BatchFold(cEXIT_CODEptr verdicts, cui32 count) {
   ui32 converted = 0;
   ui32 failed    = 0;
   si32 worst     = si32(EXIT_ALL_CONVERTED);

   for(ui32 index = 0; index < count; ++index) {
      if(verdicts[index] == EXIT_ALL_CONVERTED) {
         ++converted;
         continue;
      }
      ++failed;
      if(si32(verdicts[index]) > worst) worst = si32(verdicts[index]);
   }
   if(!failed) return EXIT_ALL_CONVERTED;
   // D7c gives a run that both converted something and failed something its own exit code, so that a
   // caller can tell it apart from a run that did nothing.
   if(converted) return EXIT_PARTIAL;
   // When everything failed, the highest per-input verdict is what the run returns.
   return EXIT_CODE(worst);
}

cEXIT_CODE BatchRun(cCLI_OPTIONSptr options) {
   cui32 count = options->inputCount;

   if(!count) return EXIT_ALL_CONVERTED; // CliParse refuses a command line with no input; nothing failed

   csize_t slots = size_t(count);
   // One slot per input in each of six arrays: the verdicts; the plan and each input's source; the queue of
   // inputs a worker converts and the verdicts the pool hands back for them; and what the pre-flight
   // compares. Every one of them, and the arena of strings, is released on the one path out.
   EXIT_CODEptr   verdicts = (EXIT_CODEptr)amalloc(sizeof(EXIT_CODE) * slots, 16u);
   EXIT_CODEptr   results  = (EXIT_CODEptr)amalloc(sizeof(EXIT_CODE) * slots, 16u);
   BATCH_PLANptr  plans    = (BATCH_PLANptr)amalloc(sizeof(BATCH_PLAN) * slots, 16u);
   ui32ptr        sources  = (ui32ptr)amalloc(sizeof(ui32) * slots, 16u);
   ui32ptr        queue    = (ui32ptr)amalloc(sizeof(ui32) * slots, 16u);
   BATCH_INPUTptr entries  = (BATCH_INPUTptr)amalloc(sizeof(BATCH_INPUT) * slots, 16u);
   wchptr         arena    = nullptr;
   bool           ready    = verdicts && results && plans && sources && queue && entries;
   EXIT_CODE      folded   = EXIT_INTERNAL;

   if(ready) {
      arena = BatchGather(options, entries);
      ready = (arena != nullptr);
   }
   if(!ready) DiagError("not enough memory to plan the run");
   if(ready) {
      // The pre-flight, in argument order and before any worker starts, so that what it refuses and what
      // it reports are the same at every --threads count.
      cbool shared = options->mediaDir && options->emitImages && !options->toStdout && count > 1u;
      ui32  queued = 0;

      BatchPlan(entries, count, shared, plans, sources);
      for(ui32 index = 0; index < count; ++index) {
         verdicts[index] = EXIT_ALL_CONVERTED;
         if(plans[index] == BATCH_PLAN_CONVERT) {
            queue[queued++] = index;
            continue;
         }
         if(plans[index] == BATCH_PLAN_REPEAT) {
            if(!options->quiet) DiagNoteText(NAMED_TWICE, options->inputs[index]);
            continue;
         }

         cchptr why = TAKEN_MEDIA_DIR;

         if(plans[index] == BATCH_PLAN_IS_SELF) why = TAKEN_BY_SELF;
         else if(plans[index] == BATCH_PLAN_IS_INPUT) why = TAKEN_BY_INPUT;
         else if(plans[index] == BATCH_PLAN_CLAIMED) why = TAKEN_BY_EARLIER;
         DiagErrorText(why, options->inputs[index]);
         verdicts[index] = EXIT_OUTPUT;
      }

      // The pool hands back each verdict at the input's place in the queue, and the queue says where that
      // input stands on the command line.
      BATCH_WORK work    = {options, queue};
      cui32      workers = BatchPool(BatchConvertOne, &work, queued, options->threadCount, results);

      for(ui32 item = 0; item < queued; ++item) verdicts[queue[item]] = results[item];
      // A repeat takes the verdict of its earliest spelling, which BatchPlan guarantees is not a repeat.
      for(ui32 index = 0; index < count; ++index) {
         if(plans[index] == BATCH_PLAN_REPEAT) verdicts[index] = verdicts[sources[index]];
      }
      folded = BatchFold(verdicts, count);
      // A single input's own message is the whole of what its outcome says; a note or a list of one would
      // only repeat it. With several, the note is the one line that says the pool was as wide as asked.
      if(count > 1u) {
         ui32 failed = 0;

         for(ui32 index = 0; index < count; ++index) failed += (verdicts[index] != EXIT_ALL_CONVERTED ? 1u : 0u);
         if(!options->quiet) BatchNoteWorkers(count - failed, count, workers);
         if(failed) BatchListFailures(options, verdicts, failed);
      }
   }
   if(arena) mdealloc(arena);
   if(entries) mdealloc(entries);
   if(queue) mdealloc(queue);
   if(sources) mdealloc(sources);
   if(plans) mdealloc(plans);
   if(results) mdealloc(results);
   if(verdicts) mdealloc(verdicts);
   return folded;
}
