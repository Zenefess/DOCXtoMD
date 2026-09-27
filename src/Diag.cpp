/*
 * File: Diag.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-09-27
 * Description: Diagnostic sink implementation: one spin lock per line; wide arguments cross to UTF-8 through Utf.
 * To Do: 1) Take over -q from the callers, so a note is suppressed here rather than at each call site.
 *        2) Use spinlocks.h's padded lock type in place of DIAG_LOCK_LINE once the header gains one.
 * Dependencies: BuildGuards.h, Diag.h, Utf.h, typedefs.h, memory management.h, spinlocks.h, windows.h, stdio.h
 * ISA: Scalar
 * Thread-safety: MT-safe
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
#include "spinlocks.h"
#include "Utf.h"
#include "Diag.h"

//-- The lock

// A lock flag with a cache line to itself. spinlocks.h has no padded lock type yet (its To Do 3), and a
// flag sharing a line with anything another thread writes would false-share; al64 makes the structure
// both start a line and fill one.
struct al64 DIAG_LOCK_LINE {
   vui32 flag; ///< 0 unlocked, 1 locked; spinlocks.h's contract
};

static_assert(sizeof(DIAG_LOCK_LINE) == 64u, "Diag: the lock flag must own a whole cache line.");

// The one lock every writer holds for exactly one line (D6). SpinLockMin rather than SpinLock, because
// the section is a console or pipe write: a syscall that can block for milliseconds on a redirected
// handle, which SpinLock would spend spinning where SpinLockMin yields. Memory order is spinlocks.h's:
// acquiring is a LOCK CMPXCHG and releasing an XCHG, both full barriers, so nothing written inside one
// line can be seen outside it early.
static DIAG_LOCK_LINE DIAG_LOCK = {};

//-- Boundary transcoding

// Crosses a wide argument to UTF-8 on the heap. UTF-16 exists only at the Win32 boundary and Utf owns
// the transcoding for the whole project, so this is a measure, allocate and convert and nothing more.
// It runs before the lock is taken, so the critical section holds writes and nothing else. The result
// is null for an argument with nothing to print; *printable goes false when the argument cannot be
// transcoded or its buffer allocated, and the writer then prints a placeholder in its place.
static ui8ptr DiagFromWide(cwchptr text, ui64ptrc byteCount, boolptrc printable) {
   ui64 needed = 0;

   *byteCount = 0;
   *printable = true;
   if(!text) return nullptr;
   if(UtfFromWide(text, nullptr, 0, &needed) != UTF8_OK) {
      *printable = false;
      return nullptr;
   }
   if(!needed) return nullptr; // An empty argument has nothing to print, not even a terminator

   ui8ptr buffer = (ui8ptr)amalloc(needed, 16u);

   if(!buffer) {
      *printable = false;
      return nullptr;
   }
   if(UtfFromWide(text, buffer, needed, byteCount) != UTF8_OK) {
      mdealloc(buffer);
      *byteCount = 0;
      *printable = false;
      return nullptr;
   }
   return buffer;
}

// Writes one "DOCXtoMD: <kind>: <message>: <text>" line under the lock.
static void DiagLineWide(cchptr prefix, cchptr message, cwchptr text) {
   ui64   byteCount = 0;
   bool   printable = true;
   ui8ptr utf8      = DiagFromWide(text, &byteCount, &printable);

   SpinLockMin(&DIAG_LOCK.flag);
   fflush(stdout); // Keep stderr from overtaking text already queued on stdout
   fputs(prefix, stderr);
   if(message) fputs(message, stderr);
   fputs(": ", stderr);
   if(utf8) fwrite(utf8, 1u, size_t(byteCount), stderr);
   else if(!printable) fputs("<unprintable>", stderr);
   fputc('\n', stderr);
   SpinUnlock(&DIAG_LOCK.flag);
   if(utf8) mdealloc(utf8);
}

//-- Limits

// Bytes per WriteFile call. A single call takes a DWORD count, and a smaller ceiling keeps a partial
// write on a pipe to something the loop above can absorb.
constexpr cui64 DIAG_WRITE_BYTES = 1u << 20;

//-- Names

// One phrase per EXIT_CODE, in the enum's order: the failure list reads "failed (exit 3, not a valid DOCX)".
static constexpr cchptr DIAG_EXIT_TEXT[] = {
    "converted",                       // EXIT_ALL_CONVERTED
    "not a usable command line",       // EXIT_USAGE
    "the input could not be read",     // EXIT_INPUT
    "not a valid DOCX",                // EXIT_NOT_DOCX
    "the output could not be written", // EXIT_OUTPUT
    "an internal error",               // EXIT_INTERNAL
    "partly converted"                 // EXIT_PARTIAL
};

static_assert(sizeof(DIAG_EXIT_TEXT) / sizeof(DIAG_EXIT_TEXT[0]) == size_t(EXIT_PARTIAL) + 1u, "Diag: one phrase per exit code.");

//== Writers

void DiagWriteOut(cchptr text) {
   if(!text) return;

   SpinLockMin(&DIAG_LOCK.flag);
   fputs(text, stdout);
   SpinUnlock(&DIAG_LOCK.flag);
}

cbool DiagWriteOutBytes(cui8ptr bytes, cui64 byteCount) {
   if(!bytes || !byteCount) return true; // An empty document is a legitimate zero-byte output

   SpinLockMin(&DIAG_LOCK.flag);
   fflush(stdout); // Whatever the runtime has queued goes first, so the document cannot overtake it

   cHANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
   ui64    done   = 0;
   bool    ok     = (handle != INVALID_HANDLE_VALUE && handle);

   while(ok && done < byteCount) {
      cui64 remaining = byteCount - done;
      cui64 want      = (remaining > DIAG_WRITE_BYTES ? DIAG_WRITE_BYTES : remaining);
      DWORD put       = 0;

      if(!WriteFile(handle, bytes + done, DWORD(want), &put, nullptr) || !put) {
         ok = false;
         break;
      }
      done += put;
   }
   SpinUnlock(&DIAG_LOCK.flag);
   return ok;
}

void DiagWriteErr(cchptr text) {
   if(!text) return;

   SpinLockMin(&DIAG_LOCK.flag);
   fflush(stdout); // Keep stderr from overtaking text already queued on stdout
   fputs(text, stderr);
   SpinUnlock(&DIAG_LOCK.flag);
}

void DiagError(cchptr message) {
   SpinLockMin(&DIAG_LOCK.flag);
   fflush(stdout);
   fputs("DOCXtoMD: error: ", stderr);
   if(message) fputs(message, stderr);
   fputc('\n', stderr);
   SpinUnlock(&DIAG_LOCK.flag);
}

void DiagErrorText(cchptr message, cwchptr text) { DiagLineWide("DOCXtoMD: error: ", message, text); }

void DiagNoteText(cchptr message, cwchptr text) { DiagLineWide("DOCXtoMD: note: ", message, text); }

//== Names

cchptr DiagExitCodeText(cEXIT_CODE code) {
   if(si32(code) < 0 || si32(code) > si32(EXIT_PARTIAL)) return "an unknown verdict";
   return DIAG_EXIT_TEXT[code];
}
