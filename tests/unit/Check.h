/*
 * File: Check.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: The unit-test harness: one CHECK macro, a group heading, and a pass/fail summary.
 * To Do: 1) Add a filter argument so one group can be run alone, once the suite is large enough to want it.
 *        2) Report the first failing case's input verbatim, rather than only the expression that failed.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Entry points

/// Starts a named group of checks, so a failure says which area it came from.
/// @param name  NUL-terminated ASCII heading.
void CheckGroup(cchptr name);

/// Records one check and reports it when it fails.
/// @param passed      What the check evaluated to.
/// @param expression  The source text of the condition, supplied by the CHECK macro.
/// @param line        The line it is written on, supplied by the CHECK macro.
/// @return What passed was, so a caller may stop a sequence of dependent checks after the first failure.
cbool CheckReport(cbool passed, cchptr expression, cui32 line);

/// Writes the totals.
/// @return 0 when every check passed, 1 otherwise, ready to be a process exit code.
csi32 CheckSummary(void);

//== Macros

/// Evaluates one condition and records it.
#define CHECK(condition) CheckReport((condition), #condition, ui32(__LINE__))
