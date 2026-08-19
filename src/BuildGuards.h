/*
 * File: BuildGuards.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Compile-time build guards; fails the build when the AVX2 baseline flag is absent.
 * To Do: 1) Fold in any further baseline guards a later decision adds; keep them errors, never behaviour forks.
 *        2) Include this header first from every project translation unit as src/ grows past M2.
 * Dependencies: None
 * ISA: Scalar
 * Thread-safety: N/A
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

// GCS a2/a3 build guard (D4): the CPU baseline is AVX2+FMA3+BMI2, so every x64 build compiles with
// /arch:AVX2. This is an error guard, not a behaviour fork (a11) -- there is no sub-baseline path.
// Guard on __AVX2__ alone: MSVC defines __AVX2__ but never __FMA__ or __BMI2__.
#ifndef __AVX2__
#error DOCXtoMD: compile with /arch:AVX2 -- GCS a2 sets the CPU baseline to AVX2+FMA3+BMI2.
#endif
