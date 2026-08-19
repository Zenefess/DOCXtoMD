/*
 * File: Diag.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Diagnostic sink implementation; wide arguments cross to UTF-8 at the Win32 boundary.
 * To Do: 1) Guard every writer with include/spinlocks.h when M13 gives every worker this one sink (D6).
 *        2) Replace the local WideCharToMultiByte call with the Utf module once M4 lands it.
 * Dependencies: BuildGuards.h, Diag.h, typedefs.h, memory management.h, windows.h, stdio.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros, and memory management.h pulls those two in that order itself.
#include <windows.h>
#include <stdio.h>
#include "typedefs.h"
#include "memory management.h"
#include "Diag.h"

//-- Boundary transcoding

// Writes a wide argument to stderr as UTF-8. UTF-16 exists only at the Win32 boundary, so the conversion
// stays here rather than becoming an interface: M4's Utf module owns the document-side transcoding.
static void DiagWriteWideErr(cwchptr text) {
   csi32 byteCount = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);

   if(byteCount <= 0) {
      fputs("<unprintable>", stderr);
      return;
   }
   if(byteCount == 1) return; // Empty argument: the count covers the terminator alone

   chptrc buffer = (chptrc)amalloc(size_t(byteCount), 16u);

   if(!buffer) {
      fputs("<unprintable>", stderr);
      return;
   }
   // amalloc does not clear the block, so the second conversion's own count decides what is written:
   // trusting the first call's size would put uninitialised heap on stderr if this one ever failed.
   csi32 written = WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, byteCount, nullptr, nullptr);

   if(written > 1) fwrite(buffer, 1u, size_t(written - 1), stderr);
   else if(written <= 0) fputs("<unprintable>", stderr);

   mdealloc(buffer);
}

//== Writers

void DiagWriteOut(cchptr text) {
   if(text) fputs(text, stdout);
}

void DiagWriteErr(cchptr text) {
   if(!text) return;

   fflush(stdout); // Keep stderr from overtaking text already queued on stdout
   fputs(text, stderr);
}

void DiagError(cchptr message) {
   fflush(stdout);
   fputs("DOCXtoMD: error: ", stderr);
   if(message) fputs(message, stderr);
   fputc('\n', stderr);
}

void DiagErrorText(cchptr message, cwchptr text) {
   fflush(stdout);
   fputs("DOCXtoMD: error: ", stderr);
   if(message) fputs(message, stderr);
   fputs(": ", stderr);
   if(text) DiagWriteWideErr(text);
   fputc('\n', stderr);
}
