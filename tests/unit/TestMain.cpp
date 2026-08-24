/*
 * File: TestMain.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: Entry point of the unit-test binary: runs every group and returns the summary's verdict.
 * To Do: 1) Take a group name on the command line so one area can be run alone.
 *        2) Register groups from a table once there are enough of them for a list to be worth having.
 * Dependencies: BuildGuards.h, Check.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Check.h"

//== Groups

// Declared here rather than in a header: there are three of them, each is called exactly once, and a
// header holding nothing but these three lines would be one more file for the .vcxproj to track.
void TestUtf(void);
void TestXmlPull(void);
void TestOpcPackage(void);

//== Entry point

// r11 does not reach this name: the entry point is spelled by the language, not chosen here. The suite
// takes no arguments, so it is main rather than wmain -- there is no path to keep in UTF-16.
si32 main(void) {
   TestUtf();
   TestXmlPull();
   TestOpcPackage();
   return CheckSummary();
}
