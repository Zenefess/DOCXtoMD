/*
 * File: DOCXtoMD.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Placeholder entry point; carries the AVX2 build guard until src/main.cpp supersedes it at M2.
 * To Do: 1) Replace with src/main.cpp and move the build guard into src/BuildGuards.h (M2).
 *        2) Add wmain, SetConsoleOutputCP(CP_UTF8), and the CliOptions and Diag wiring (M2).
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
// GCS a2/a3 build guard (D4): the CPU baseline is AVX2+FMA3+BMI2, so every x64 build compiles with
// /arch:AVX2. This is an error guard, not a behaviour fork (a11) -- there is no sub-baseline path.
#ifndef __AVX2__
#error DOCXtoMD.cpp: compile with /arch:AVX2 -- GCS a2 sets the CPU baseline to AVX2+FMA3+BMI2.
#endif

#include "typedefs.h"

// r11 does not reach this name: the entry point is spelled by the language, not chosen here.
si32 main() { return 0; }
