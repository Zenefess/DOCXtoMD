/*
 * File: Check.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: The unit-test harness's counters and reporting.
 * To Do: 1) Print the group name beside each failure once groups nest.
 * Dependencies: Check.h, typedefs.h, stdio.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include <stdio.h>
#include "typedefs.h"
#include "Check.h"

//-- Counters

// The suite is a single-threaded console program, so plain counters are enough; nothing here is shared.
static ui32   CHECK_PASSED = 0;
static ui32   CHECK_FAILED = 0;
static cchptr CHECK_GROUP  = "";

//== Entry points

void CheckGroup(cchptr name) {
   CHECK_GROUP = name;
   printf("\n%s\n", name);
}

cbool CheckReport(cbool passed, cchptr expression, cui32 line) {
   if(passed) {
      ++CHECK_PASSED;
      return true;
   }
   ++CHECK_FAILED;
   printf("FAIL  %s:%u  %s\n", CHECK_GROUP, line, expression);
   return false;
}

csi32 CheckSummary(void) {
   if(CHECK_FAILED) {
      printf("\n%u of %u checks failed\n", CHECK_FAILED, CHECK_PASSED + CHECK_FAILED);
      return 1;
   }
   printf("\nall %u checks passed\n", CHECK_PASSED);
   return 0;
}
