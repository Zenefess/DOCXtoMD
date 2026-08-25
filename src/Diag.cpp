/*
 * File: Diag.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-25
 * Description: Diagnostic sink implementation; wide arguments cross to UTF-8 through the Utf module.
 * To Do: 1) Guard every writer with include/spinlocks.h when M13 gives every worker this one sink (D6).
 *        2) Take over -q from the callers, so a note is suppressed here rather than at each call site.
 * Dependencies: BuildGuards.h, Diag.h, Utf.h, typedefs.h, memory management.h, windows.h, stdio.h
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
#include "Utf.h"
#include "Diag.h"

//-- Boundary transcoding

// Writes a wide argument to stderr as UTF-8. UTF-16 exists only at the Win32 boundary, and Utf owns
// the transcoding for the whole project, so this is a measure, allocate and convert and nothing more.
static void DiagWriteWideErr(cwchptr text) {
   ui64 byteCount = 0;

   if(UtfFromWide(text, nullptr, 0, &byteCount) != UTF8_OK) {
      fputs("<unprintable>", stderr);
      return;
   }
   if(!byteCount) return; // An empty argument has nothing to print, not even a terminator

   ui8ptr buffer = (ui8ptr)amalloc(byteCount, 16u);

   if(!buffer) {
      fputs("<unprintable>", stderr);
      return;
   }

   ui64 written = 0;

   if(UtfFromWide(text, buffer, byteCount, &written) == UTF8_OK) fwrite(buffer, 1u, size_t(written), stderr);
   else fputs("<unprintable>", stderr);
   mdealloc(buffer);
}

//-- Limits

// Bytes per WriteFile call. A single call takes a DWORD count, and a smaller ceiling keeps a partial
// write on a pipe to something the loop above can absorb.
constexpr cui64 DIAG_WRITE_BYTES = 1u << 20;

//== Writers

void DiagWriteOut(cchptr text) {
   if(text) fputs(text, stdout);
}

void DiagWriteOutBytes(cui8ptr bytes, cui64 byteCount) {
   if(!bytes || !byteCount) return;
   fflush(stdout); // Whatever the runtime has queued goes first, so the document cannot overtake it

   cHANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);

   if(handle == INVALID_HANDLE_VALUE || !handle) return;

   ui64 done = 0;

   while(done < byteCount) {
      cui64 remaining = byteCount - done;
      cui64 want      = (remaining > DIAG_WRITE_BYTES ? DIAG_WRITE_BYTES : remaining);
      DWORD put       = 0;

      if(!WriteFile(handle, bytes + done, DWORD(want), &put, nullptr) || !put) return;
      done += put;
   }
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

void DiagNoteText(cchptr message, cwchptr text) {
   fflush(stdout);
   fputs("DOCXtoMD: note: ", stderr);
   if(message) fputs(message, stderr);
   fputs(": ", stderr);
   if(text) DiagWriteWideErr(text);
   fputc('\n', stderr);
}
