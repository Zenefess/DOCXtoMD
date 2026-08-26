/*
 * File: TestMain.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-25
 * Description: Entry point of the unit-test binary: runs every group and returns the summary's verdict.
 * To Do: 1) Take a group name on the command line so one area can be run alone.
 *        2) Register groups from a table now that there are eight of them.
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

// Declared here rather than in a header: each is called exactly once, and a header holding nothing but
// these lines would be one more file for the .vcxproj to track. They run in pipeline order, so a failure
// in a layer everything else rests on is reported before the layers above it fail for the same reason.
void TestUtf(void);
void TestXmlPull(void);
void TestOpcPackage(void);
void TestStyleModel(void);
void TestDocWalker(void);
void TestMdEscape(void);
void TestMdEmitter(void);
void TestConvert(void);

//== Entry point

// r11 does not reach this name: the entry point is spelled by the language, not chosen here. The suite
// takes no arguments, so it is main rather than wmain -- there is no path to keep in UTF-16.
si32 main(void) {
   TestUtf();
   TestXmlPull();
   TestOpcPackage();
   TestStyleModel();
   TestDocWalker();
   TestMdEscape();
   TestMdEmitter();
   TestConvert();
   return CheckSummary();
}
