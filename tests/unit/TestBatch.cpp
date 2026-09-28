/*
 * File: TestBatch.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-27
 * Last Modified: 2026-09-27
 * Description: Unit tests for the worker pool, the run plan, the exit-code fold and the exit-code phrases.
 * To Do: 1) Drive BatchRun itself once the suite may open files; tests/run_golden.py is what drives it today.
 *        2) Measure the widest a pool ever runs against the cores the machine has, once bench/ exists.
 * Dependencies: BuildGuards.h, Batch.h, Check.h, CliOptions.h, Convert.h, Diag.h, typedefs.h, windows.h, intrin.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros.
#include <windows.h>
#include <intrin.h>
#include "typedefs.h"
#include "Check.h"
#include "CliOptions.h"
#include "Convert.h"
#include "Diag.h"
#include "Batch.h"

//-- What a pool run saw

// The interlocked intrinsics take a volatile LONG; named once here rather than spelled at each use (r2).
typedef volatile LONG vLONG;
typedef const LONG    cLONG;
typedef vLONG        *vLONGptr;

// The most items any case hands the pool.
constexpr cui32 POOL_MAX_ITEMS = 1000u;

// Everything a job records. CHECK is called from the main thread only, after the pool has joined, so the
// jobs write here and nowhere else -- every field is written through an interlocked intrinsic, because two
// workers may write the same one, or by exactly one worker at an index only it owns.
struct POOL_TRACE {
   vLONG runs[POOL_MAX_ITEMS];   ///< How many times each item ran
   vLONG order[POOL_MAX_ITEMS];  ///< The items in the order they started
   DWORD thread[POOL_MAX_ITEMS]; ///< Which thread ran each item
   vLONG started;                ///< How many items have started, which is the next slot of order
   vLONG inside;                 ///< How many jobs are running at this moment
   vLONG widest;                 ///< The most jobs that were ever running at one moment
   ui32  meet;                   ///< When above 1, each job waits until this many are running at once
};

typedef POOL_TRACE *POOL_TRACEptr;

// What the job is handed. The context is const to every worker; the trace it points at is not.
struct POOL_CASE {
   POOL_TRACEptr trace;
};

typedef const POOL_CASE *cPOOL_CASEptr;

// The trace lives in static storage rather than on the stack, because it is several kilobytes and every
// case clears it before running.
static POOL_TRACE TRACE;

// Every item's verdict is a function of the item alone, so a verdict that lands at the wrong index shows.
// Six values, 0 to 5, so EXIT_PARTIAL can never be one and marks a slot nothing wrote.
static cEXIT_CODE PoolVerdictOf(cui32 item) { return EXIT_CODE(item % 6u); }

// Reads a field another worker may be writing at the same moment. An interlocked OR with nothing is a
// read that is a full barrier, which a plain read of a volatile is not under every memory model.
static cLONG PoolRead(vLONGptr field) { return _InterlockedOr(field, 0); }

// Raises widest to at least value, which another worker may be raising at the same moment.
static void PoolRaise(cui32 value) {
   for(;;) {
      cLONG seen = PoolRead(&TRACE.widest);

      if(seen >= LONG(value)) return;
      if(_InterlockedCompareExchange(&TRACE.widest, LONG(value), seen) == seen) return;
   }
}

// Whether two NUL-terminated strings are the same bytes.
static cbool PoolSame(cchptr produced, cchptr wanted) {
   ui32 index = 0;

   if(!produced) return false;
   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// The job: records that it ran, where and in what order, and -- when the case asks for it -- waits until
// meet jobs are running at once, which only a pool that really runs them at once can reach. A pool that
// ran them one after another would wait out the whole bound on every item and then fail the check.
static cEXIT_CODE PoolJob(cptr context, cui32 item) {
   POOL_TRACEptr trace = ((cPOOL_CASEptr)context)->trace;
   cui32         slot  = ui32(_InterlockedIncrement(&trace->started)) - 1u;

   _InterlockedIncrement(&trace->runs[item]);
   if(slot < POOL_MAX_ITEMS) trace->order[slot] = LONG(item);
   trace->thread[item] = GetCurrentThreadId();
   PoolRaise(ui32(_InterlockedIncrement(&trace->inside)));
   // Five seconds by the clock rather than a count of sleeps, because Sleep(1) lasts a whole 15.6 ms timer
   // tick on Windows: a pool that meets never comes near the bound, and one that cannot fails in seconds.
   cui64 deadline = GetTickCount64() + 5000u;

   while(trace->meet > 1u && ui32(PoolRead(&trace->widest)) < trace->meet && GetTickCount64() < deadline) Sleep(1);
   _InterlockedDecrement(&trace->inside);
   return PoolVerdictOf(item);
}

// Clears the trace, runs one pool and reports how many threads it said ran.
static cui32 PoolRun(cui32 items, cui32 threads, cui32 meet, EXIT_CODEptrc verdicts) {
   POOL_CASE poolCase = {&TRACE};

   for(ui32 index = 0; index < POOL_MAX_ITEMS; ++index) {
      TRACE.runs[index]   = 0;
      TRACE.order[index]  = -1;
      TRACE.thread[index] = 0;
      verdicts[index]     = EXIT_PARTIAL;
   }
   TRACE.started = 0;
   TRACE.inside  = 0;
   TRACE.widest  = 0;
   TRACE.meet    = meet;
   return BatchPool(PoolJob, &poolCase, items, threads, verdicts);
}

// Whether every item ran exactly once and every verdict landed at its own index -- and nothing past the
// last item ran or was written.
static cbool PoolComplete(cui32 items, cEXIT_CODEptr verdicts) {
   for(ui32 index = 0; index < POOL_MAX_ITEMS; ++index) {
      cbool inside = index < items;

      if(TRACE.runs[index] != (inside ? 1 : 0)) return false;
      if(verdicts[index] != (inside ? PoolVerdictOf(index) : EXIT_PARTIAL)) return false;
   }
   return TRACE.started == LONG(items) && TRACE.inside == 0;
}

//-- What a plan decided

// The most inputs a plan case names, and the longest output one derives.
constexpr cui32 PLAN_MAX_INPUTS = 8u;
constexpr cui64 PLAN_MAX_PATH   = 256u;

// Plans a command line and compares every input's plan and source with the expected ones. The spellings
// are compared as given -- BatchRun normalises them first -- and what the files on disk are is supplied by
// the case: selves and targets give, for each input, the file its input and its output reach, 0 for a path
// that reaches nothing and any other number standing for one file. Null for either means nothing exists.
static cbool PlansTo(cwchptrptr inputs, cui32 count, cwchptr output, cbool shared, cui32ptr selves, cui32ptr targets, // The case
                     cBATCH_PLANptr plans, cui32ptr sources) {                                                        // Its answer
   BATCH_INPUT entries[PLAN_MAX_INPUTS] = {};
   wchar       derived[PLAN_MAX_INPUTS][PLAN_MAX_PATH];
   BATCH_PLAN  planned[PLAN_MAX_INPUTS];
   ui32        from[PLAN_MAX_INPUTS];

   for(ui32 index = 0; index < count; ++index) {
      cui32 self   = (selves ? selves[index] : 0u);
      cui32 target = (targets ? targets[index] : 0u);

      entries[index].input        = inputs[index];
      entries[index].output       = (ConvertOutputPath(inputs[index], output, count > 1u, derived[index], PLAN_MAX_PATH) ? derived[index] : nullptr);
      entries[index].self.volume  = 1u;
      entries[index].self.low     = self;
      entries[index].self.high    = 0;
      entries[index].self.known   = self != 0u;
      entries[index].target       = entries[index].self;
      entries[index].target.low   = target;
      entries[index].target.known = target != 0u;
   }
   BatchPlan(entries, count, shared, planned, from);
   for(ui32 index = 0; index < count; ++index) {
      if(planned[index] != plans[index] || from[index] != sources[index]) return false;
   }
   return true;
}

//== The suite

void TestBatch(void);

void TestBatch(void) {
   EXIT_CODE verdicts[POOL_MAX_ITEMS];

   CheckGroup("Batch: the pool runs every item exactly once, and each verdict lands at its item");
   cui32 THREADS[] = {0u, 1u, 2u, 3u, 8u, 64u};
   cui32 ITEMS[]   = {1u, 2u, 7u, 1000u};

   for(ui32 t = 0; t < sizeof(THREADS) / sizeof(THREADS[0]); ++t) {
      for(ui32 i = 0; i < sizeof(ITEMS) / sizeof(ITEMS[0]); ++i) {
         cui32 asked = (THREADS[t] ? THREADS[t] : 1u);
         cui32 ran   = PoolRun(ITEMS[i], THREADS[t], 0u, verdicts);

         CHECK(PoolComplete(ITEMS[i], verdicts));
         // A pool is never wider than it has items for, and never wider than it was asked to be.
         CHECK(ran == (asked < ITEMS[i] ? asked : ITEMS[i]));
      }
   }

   CheckGroup("Batch: a pool with no items runs nothing");
   CHECK(PoolRun(0u, 8u, 0u, verdicts) == 0u);
   CHECK(PoolComplete(0u, verdicts));

   CheckGroup("Batch: a pool of one starts no thread and runs its items in order");
   // The single-file case stays single-threaded (D6), and --threads 1 converts in argument order.
   cDWORD self = GetCurrentThreadId();

   for(ui32 pass = 0; pass < 2u; ++pass) {
      bool here    = true;
      bool ordered = true;

      CHECK(PoolRun(50u, pass, 0u, verdicts) == 1u);
      CHECK(PoolComplete(50u, verdicts));
      for(ui32 index = 0; index < 50u; ++index) {
         here    = here && TRACE.thread[index] == self;
         ordered = ordered && TRACE.order[index] == LONG(index);
      }
      CHECK(here);
      CHECK(ordered);
      CHECK(TRACE.widest == 1);
   }

   CheckGroup("Batch: a pool of several really runs its items at once");
   // Each job waits until as many jobs are running as the pool is wide, so these pass only when the pool
   // is as wide as it was asked to be -- and every item ran on some thread, never twice.
   CHECK(PoolRun(2u, 2u, 2u, verdicts) == 2u);
   CHECK(TRACE.widest == 2);
   CHECK(PoolComplete(2u, verdicts));
   CHECK(PoolRun(4u, 4u, 4u, verdicts) == 4u);
   CHECK(TRACE.widest == 4);
   CHECK(PoolComplete(4u, verdicts));
   // Wider than the machine is fine: the pool is sized by --threads, which CliParse bounds, not by itself.
   CHECK(PoolRun(12u, 12u, 12u, verdicts) == 12u);
   CHECK(TRACE.widest == 12);
   CHECK(PoolComplete(12u, verdicts));
   // Some item ran on a thread that is not this one, which is what a started thread means.
   bool elsewhere = false;

   for(ui32 index = 0; index < 12u; ++index) elsewhere = elsewhere || TRACE.thread[index] != self;
   CHECK(elsewhere);

   CheckGroup("Batch: the verdicts fold into the exit code D7c describes");
   cEXIT_CODE ALL_GOOD[]  = {EXIT_ALL_CONVERTED, EXIT_ALL_CONVERTED};
   cEXIT_CODE ONE_BAD[]   = {EXIT_ALL_CONVERTED, EXIT_NOT_DOCX};
   cEXIT_CODE ALL_BAD[]   = {EXIT_NOT_DOCX, EXIT_INPUT};
   cEXIT_CODE WORST_BAD[] = {EXIT_OUTPUT, EXIT_INTERNAL, EXIT_INPUT};
   cEXIT_CODE LAST_GOOD[] = {EXIT_INTERNAL, EXIT_OUTPUT, EXIT_ALL_CONVERTED};

   CHECK(BatchFold(ALL_GOOD, 2u) == EXIT_ALL_CONVERTED);
   CHECK(BatchFold(ALL_GOOD, 0u) == EXIT_ALL_CONVERTED);
   CHECK(BatchFold(ONE_BAD, 2u) == EXIT_PARTIAL);
   CHECK(BatchFold(ONE_BAD, 1u) == EXIT_ALL_CONVERTED);
   CHECK(BatchFold(ALL_BAD, 2u) == EXIT_NOT_DOCX);
   CHECK(BatchFold(ALL_BAD + 1, 1u) == EXIT_INPUT);
   CHECK(BatchFold(WORST_BAD, 3u) == EXIT_INTERNAL);
   CHECK(BatchFold(LAST_GOOD, 3u) == EXIT_PARTIAL);
   CHECK(BatchFold(LAST_GOOD, 2u) == EXIT_INTERNAL);

   CheckGroup("Batch: every exit code has its own phrase for the failure list");
   // Pinned row by row, because a phrase table and the enum it is indexed by drift apart silently.
   CHECK(PoolSame(DiagExitCodeText(EXIT_ALL_CONVERTED), "converted"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_USAGE), "not a usable command line"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_INPUT), "the input could not be read"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_NOT_DOCX), "not a valid DOCX"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_OUTPUT), "the output could not be written"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_INTERNAL), "an internal error"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_PARTIAL), "partly converted"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_CODE(7)), "an unknown verdict"));
   CHECK(PoolSame(DiagExitCodeText(EXIT_CODE(-1)), "an unknown verdict"));

   CheckGroup("Batch: the plan converts distinct inputs and keeps a repeat to one conversion");
   // Where no file exists the normalised spellings decide, folding case as NTFS does.
   cwchptr     DISTINCT[]      = {L"a.docx", L"b.docx", L"c.docx"};
   cBATCH_PLAN ALL_CONVERT[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_CONVERT, BATCH_PLAN_CONVERT};
   cui32       OWN[]           = {0u, 1u, 2u};
   cwchptr     REPEATED[]      = {L"a.docx", L"b.docx", L"A.DOCX"};
   cBATCH_PLAN REPEAT_PLAN[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_CONVERT, BATCH_PLAN_REPEAT};
   cui32       REPEAT_SOURCE[] = {0u, 1u, 0u};

   CHECK(PlansTo(DISTINCT, 3u, nullptr, false, nullptr, nullptr, ALL_CONVERT, OWN));
   CHECK(PlansTo(REPEATED, 3u, nullptr, false, nullptr, nullptr, REPEAT_PLAN, REPEAT_SOURCE));

   CheckGroup("Batch: the plan refuses an input that would destroy another or overwrite its output");
   cwchptr     SAME_LEAF[]    = {L"p\\report.docx", L"q\\report.docx"};
   cBATCH_PLAN CLAIM_PLAN[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_CLAIMED};
   cui32       CLAIM_SOURCE[] = {0u, 0u};
   cwchptr     THREE_SAME[]   = {L"a\\r.docx", L"b\\r.docx", L"c\\r.docx"};
   cBATCH_PLAN THREE_PLAN[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_CLAIMED, BATCH_PLAN_CLAIMED};
   cui32       THREE_SOURCE[] = {0u, 0u, 0u};
   // c.docx would write c.md, which is the second input -- and c.md, whose stem is c, would write itself.
   cwchptr     OUTPUT_IN[]    = {L"c.docx", L"c.md"};
   cBATCH_PLAN INPUT_PLAN[]   = {BATCH_PLAN_IS_INPUT, BATCH_PLAN_IS_SELF};
   cui32       INPUT_SOURCE[] = {1u, 1u};
   // A repeat of a refused input is that input, refused with it rather than converted by a second worker.
   cwchptr     REFUSED_TWICE[] = {L"c.docx", L"c.md", L"c.docx"};
   cBATCH_PLAN TWICE_PLAN[]    = {BATCH_PLAN_IS_INPUT, BATCH_PLAN_IS_SELF, BATCH_PLAN_REPEAT};
   cui32       TWICE_SOURCE[]  = {1u, 1u, 0u};

   CHECK(PlansTo(SAME_LEAF, 2u, L"dst\\", false, nullptr, nullptr, CLAIM_PLAN, CLAIM_SOURCE));
   // Only the first of three keeps the path; the second and third are both refused, and both name it.
   CHECK(PlansTo(THREE_SAME, 3u, L"dst\\", false, nullptr, nullptr, THREE_PLAN, THREE_SOURCE));
   CHECK(PlansTo(OUTPUT_IN, 2u, nullptr, false, nullptr, nullptr, INPUT_PLAN, INPUT_SOURCE));
   CHECK(PlansTo(REFUSED_TWICE, 3u, nullptr, false, nullptr, nullptr, TWICE_PLAN, TWICE_SOURCE));
   // -o naming the one input would write the Markdown over the document it is read from -- and so would
   // the identical spelling, which the pre-flight catches before ConvertFile's own check sees it.
   cwchptr     SELF[]        = {L"a.docx"};
   cBATCH_PLAN SELF_PLAN[]   = {BATCH_PLAN_IS_SELF};
   cui32       SELF_SOURCE[] = {0u};

   CHECK(PlansTo(SELF, 1u, L"a.docx", false, nullptr, nullptr, SELF_PLAN, SELF_SOURCE));
   CHECK(PlansTo(SELF, 1u, L"A.docx", false, nullptr, nullptr, SELF_PLAN, SELF_SOURCE));
   CHECK(PlansTo(SELF, 1u, L"a.md", false, nullptr, nullptr, ALL_CONVERT, OWN));

   CheckGroup("Batch: a file that exists is judged by its identity, not by its spelling");
   // Two spellings of one file a string cannot unify -- a short name, a link, a \\?\ prefix -- are one input.
   cwchptr     ALIASES[]      = {L"C:\\w\\a.docx", L"\\\\?\\C:\\w\\a.docx"};
   cui32       ONE_FILE[]     = {7u, 7u};
   cBATCH_PLAN ALIAS_PLAN[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_REPEAT};
   cui32       ALIAS_SOURCE[] = {0u, 0u};
   // Two files a case-sensitive directory holds under names differing only in case are two inputs -- and
   // since neither output exists yet, the outputs they would write fold together and the second is refused,
   // loudly, rather than silently dropped as a repeat of the first.
   cwchptr     CASED[]       = {L"Report.docx", L"report.docx"};
   cui32       TWO_FILES[]   = {7u, 8u};
   cui32       TWO_OUTPUTS[] = {9u, 10u};
   cBATCH_PLAN CASED_PLAN[]  = {BATCH_PLAN_CONVERT, BATCH_PLAN_CLAIMED};
   // A path to a file that exists and one to nothing are two files, whatever their spellings.
   cui32 HALF_THERE[] = {7u, 0u};

   CHECK(PlansTo(ALIASES, 2u, nullptr, false, ONE_FILE, nullptr, ALIAS_PLAN, ALIAS_SOURCE));
   CHECK(PlansTo(CASED, 2u, nullptr, false, TWO_FILES, nullptr, CASED_PLAN, CLAIM_SOURCE));
   // Two outputs that both exist, as two files, are two outputs.
   CHECK(PlansTo(CASED, 2u, nullptr, false, TWO_FILES, TWO_OUTPUTS, ALL_CONVERT, OWN));
   CHECK(PlansTo(CASED, 2u, nullptr, false, HALF_THERE, nullptr, CASED_PLAN, CLAIM_SOURCE));
   // -o spelling the input another way is the input, because the output already exists and is that file.
   cwchptr ALONE[]   = {L"a.docx"};
   cui32   IT[]      = {7u};
   cui32   OTHER[]   = {8u};
   cui32   NOTHING[] = {0u};

   CHECK(PlansTo(ALONE, 1u, L"\\\\?\\C:\\w\\a.docx", false, IT, IT, SELF_PLAN, SELF_SOURCE));
   // -o naming a different file that folds to the input's name is not the input: a case-sensitive directory.
   CHECK(PlansTo(ALONE, 1u, L"A.docx", false, IT, OTHER, ALL_CONVERT, OWN));
   // -o naming nothing that exists cannot be an input that does.
   CHECK(PlansTo(ALONE, 1u, L"A.docx", false, IT, NOTHING, ALL_CONVERT, OWN));
   // An output that is another input under another name destroys it.
   cwchptr LINKED[]       = {L"c.docx", L"x.md"};
   cui32   LINK_SELVES[]  = {7u, 8u};
   cui32   LINK_TARGETS[] = {8u, 8u};

   CHECK(PlansTo(LINKED, 2u, nullptr, false, LINK_SELVES, LINK_TARGETS, INPUT_PLAN, INPUT_SOURCE));

   CheckGroup("Batch: a --media-dir shared by several inputs belongs to the first one converted (D15)");
   cBATCH_PLAN MEDIA_PLAN[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_MEDIA, BATCH_PLAN_MEDIA};
   cui32       MEDIA_SOURCE[] = {0u, 0u, 0u};
   // The first two inputs are refused, so the third is the first one converted and owns the directory.
   cwchptr     FIRST_REFUSED[] = {L"c.docx", L"c.md", L"d.docx", L"e.docx"};
   cBATCH_PLAN OWNED_PLAN[]    = {BATCH_PLAN_IS_INPUT, BATCH_PLAN_IS_SELF, BATCH_PLAN_CONVERT, BATCH_PLAN_MEDIA};
   cui32       OWNED_SOURCE[]  = {1u, 1u, 2u, 2u};
   // A repeat shares its earliest spelling's directory rather than claiming one.
   cwchptr     REPEAT_FIRST[]  = {L"a.docx", L"a.docx", L"b.docx"};
   cBATCH_PLAN SHARED_PLAN[]   = {BATCH_PLAN_CONVERT, BATCH_PLAN_REPEAT, BATCH_PLAN_MEDIA};
   cui32       SHARED_SOURCE[] = {0u, 0u, 0u};

   CHECK(PlansTo(DISTINCT, 3u, nullptr, true, nullptr, nullptr, MEDIA_PLAN, MEDIA_SOURCE));
   CHECK(PlansTo(FIRST_REFUSED, 4u, nullptr, true, nullptr, nullptr, OWNED_PLAN, OWNED_SOURCE));
   CHECK(PlansTo(REPEAT_FIRST, 3u, nullptr, true, nullptr, nullptr, SHARED_PLAN, SHARED_SOURCE));
   // Nothing is shared when BatchRun says nothing is: --no-images, or one input.
   CHECK(PlansTo(DISTINCT, 3u, nullptr, false, nullptr, nullptr, ALL_CONVERT, OWN));
}
