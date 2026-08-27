/*
 * File: MediaExtractor.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: The content-type extension table, the file naming and numbering, and the writer.
 * To Do: 1) Reuse one plan between documents once M13 gives a worker several.
 *        2) Report a picture that was dropped for want of its part, once there is a warning channel
 *           that is not an error.
 *        3) Recognise the remaining Office media types -- audio and video -- once anything emits one.
 * Dependencies: BuildGuards.h, Diag.h, Ir.h, MediaExtractor.h, OpcPackage.h, typedefs.h,
 *               memory management.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

// windows.h precedes typedefs.h in every project translation unit: typedefs.h keys its HANDLE and BYTE
// aliases off the Windows macros, and memory management.h pulls those two in that order itself.
#include <windows.h>
#include "typedefs.h"
#include "memory management.h"
#include "Diag.h"
#include "Ir.h"
#include "OpcPackage.h"
#include "MediaExtractor.h"

//-- Limits

// Bytes per WriteFile call. A single call takes a DWORD count, and a smaller ceiling keeps a short write
// from being a special case: the loop simply comes round again.
constexpr cui64 MEDIA_WRITE_BYTES = 0x00100000u;

// The longest media path this module will build, terminator included. It matches Convert's own ceiling,
// because a media path is an output path with one more segment on it.
constexpr cui64 MEDIA_MAX_PATH = 4096u;

//-- The content-type table

// One content type and the extension it names.
struct MEDIA_TYPE_ROW {
   cchptr type;      ///< The content type, lower case, with no parameters
   cchptr extension; ///< The extension it maps to, dot included
};

// CONVERSION_REFERENCE 1.2's mapping, plus the two raster formats it predates. The order is the order a
// reader would look them up in and nothing depends on it, because the match is exact rather than a
// prefix: "image/jpeg" and "image/jpg" are different strings and producers write both.
static constexpr MEDIA_TYPE_ROW MEDIA_TYPES[] = {
    {"image/png", ".png"},      {"image/jpeg", ".jpg"}, {"image/jpg", ".jpg"},     {"image/gif", ".gif"},    {"image/bmp", ".bmp"},
    {"image/x-ms-bmp", ".bmp"}, {"image/tiff", ".tif"}, {"image/svg+xml", ".svg"}, {"image/x-emf", ".emf"},  {"image/emf", ".emf"},
    {"image/x-wmf", ".wmf"},    {"image/wmf", ".wmf"},  {"image/webp", ".webp"},   {"image/x-icon", ".ico"}, {"image/vnd.microsoft.icon", ".ico"},
    {"image/x-pict", ".pict"},  {"image/pict", ".pict"}};

constexpr cui64 MEDIA_TYPE_COUNT = sizeof(MEDIA_TYPES) / sizeof(MEDIA_TYPES[0]);

// The row every extracted PNG depends on, pinned so a reordering cannot quietly lose it.
static_assert(MEDIA_TYPES[0].type[6] == 'p' && MEDIA_TYPES[0].extension[1] == 'p', // image/png, .png
              "MediaExtractor: the content-type table must begin with image/png.");

//-- Small helpers

// The length of a NUL-terminated string, bounded so a malformed one cannot run away.
static cui64 MediaLength(cchptr text, cui64 limit) {
   ui64 length = 0;

   while(length < limit && text[length]) ++length;
   return length;
}

// Whether two NUL-terminated strings match, comparing ASCII case-insensitively. A content type is
// case-insensitive in its type and subtype, and producers disagree about which case to write.
static cbool MediaSameType(cchptr a, cchptr b, cui64 byteCount) {
   for(ui64 index = 0; index < byteCount; ++index) {
      cchar left  = (a[index] >= 'A' && a[index] <= 'Z' ? char(a[index] - 'A' + 'a') : a[index]);
      cchar right = b[index];

      if(!right || left != right) return false;
   }
   return b[byteCount] == 0;
}

// Appends a range to a buffer that is being built, and reports whether it fitted.
static cbool MediaAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr bytes, cui64 byteCount) {
   if(*used + byteCount + 1u > destBytes) return false;
   for(ui64 index = 0; index < byteCount; ++index) dest[*used + index] = bytes[index];
   *used += byteCount;
   dest[*used] = 0;
   return true;
}

// Appends a decimal number to a buffer that is being built.
static cbool MediaAppendNumber(chptrc dest, cui64 destBytes, ui64ptrc used, ui32 value) {
   char digits[12];
   ui64 count = 0;

   do {
      digits[count++] = char('0' + (value % 10u));
      value /= 10u;
   } while(value);
   if(*used + count + 1u > destBytes) return false;
   while(count) dest[(*used)++] = digits[--count];
   dest[*used] = 0;
   return true;
}

// Turns one IR_SPAN_IMAGE into the plain text of its alt, which is what a picture with nowhere to come
// from degrades to. The alt text is already in the span, so nothing is copied: only the kind changes.
static void MediaToText(IR_DOCUMENTptrc document, cui32 spanIndex) {
   IR_SPANptr span = IrSpanMutable(document, spanIndex);

   if(!span) return;
   span->kind      = IR_SPAN_TEXT;
   span->fmt       = IR_FMT_NONE;
   span->flags     = IR_SPAN_FLAG_NONE;
   span->destBytes = 0;
}

//-- The plan

// Grows the plan to hold one more entry, and reports whether it could.
static cbool MediaReserve(MEDIA_SETptrc set) {
   if(set->count < set->capacity) return true;
   if(set->count >= MEDIA_MAX_FILES) return false;

   cui32   grown = (set->capacity ? set->capacity * 2u : 16u);
   si32ptr parts = (si32ptr)amalloc(ui64(grown) * sizeof(si32), 32u);
   chptr   names = (chptr)amalloc(ui64(grown) * MEDIA_MAX_NAME_BYTES, 32u);

   if(!parts || !names) {
      mdealloc(parts);
      mdealloc(names);
      return false;
   }
   for(ui32 index = 0; index < set->count; ++index) {
      parts[index] = set->parts[index];
      for(ui64 at = 0; at < MEDIA_MAX_NAME_BYTES; ++at) {
         names[index * MEDIA_MAX_NAME_BYTES + at] = set->names[index * MEDIA_MAX_NAME_BYTES + at];
      }
   }
   mdealloc(set->parts);
   mdealloc(set->names);
   set->parts    = parts;
   set->names    = names;
   set->capacity = grown;
   return true;
}

// The leaf name of one planned file.
static cchptr MediaNameOf(cMEDIA_SETptr set, cui32 index) { return set->names + ui64(index) * MEDIA_MAX_NAME_BYTES; }

// The plan entry for one part, adding it with a fresh file name when it is not there yet. A part drawn
// twice keeps one file, which is CONVERSION_REFERENCE 5.8's deduplication by relationship target.
static csi32 MediaFileFor(MEDIA_SETptrc set, OPC_PACKAGEptrc package, csi32 partIndex) {
   for(ui32 index = 0; index < set->count; ++index) {
      if(set->parts[index] == partIndex) return si32(index);
   }
   if(!MediaReserve(set)) return -1;

   char  extension[MEDIA_MAX_NAME_BYTES];
   chptr name = set->names + ui64(set->count) * MEDIA_MAX_NAME_BYTES;
   ui64  used = 0;

   MediaExtension(OpcContentTypeOf(package, partIndex), OpcPartName(package, partIndex), extension, sizeof(extension));
   name[0] = 0;
   if(!MediaAppend(name, MEDIA_MAX_NAME_BYTES, &used, "image", 5u)) return -1;
   if(!MediaAppendNumber(name, MEDIA_MAX_NAME_BYTES, &used, set->count + 1u)) return -1;
   if(!MediaAppend(name, MEDIA_MAX_NAME_BYTES, &used, extension, MediaLength(extension, sizeof(extension)))) return -1;
   set->parts[set->count] = partIndex;
   ++set->count;
   return si32(set->count - 1u);
}

//== Entry points

void MediaOpen(MEDIA_SETptrc set) { mzero(set, sizeof(MEDIA_SET)); }

void MediaClose(MEDIA_SETptrc set) {
   mdealloc(set->parts);
   mdealloc(set->names);
   MediaOpen(set);
}

cbool MediaPlan(MEDIA_SETptrc set, IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, cchptr linkPrefix, cbool emitImages) {
   cui64 prefixBytes = (linkPrefix ? MediaLength(linkPrefix, MEDIA_MAX_PATH) : 0);

   for(ui32 index = 0; index < IrSpanCount(document); ++index) {
      cIR_SPANptr span = IrSpanAt(document, index);

      if(!span || span->kind != IR_SPAN_IMAGE) continue;
      // Not a part of the package: either an external URL, which is already the destination a reader
      // needs, or a reference that resolved to nothing, which degrades to its alt text.
      if(!(span->flags & IR_SPAN_FLAG_PART)) {
         if(!emitImages || !span->destBytes) MediaToText(document, index);
         continue;
      }
      if(!emitImages) {
         MediaToText(document, index);
         continue;
      }

      char  partName[MEDIA_MAX_PATH];
      ui64  used   = 0;
      cbool copied = MediaAppend(partName, sizeof(partName), &used, IrDest(document, span->destAt), span->destBytes);
      csi32 part   = (copied ? OpcFindPart(package, partName) : -1);
      csi32 file   = (part < 0 ? -1 : MediaFileFor(set, package, part));

      if(file < 0) {
         // A picture whose part the archive does not hold is a defect in the document rather than in
         // the conversion, so it degrades to alt text the way --no-images does. A failed allocation
         // reaches the caller through the document's own sticky flag.
         MediaToText(document, index);
         if(IrFailed(document)) return false;
         continue;
      }

      char path[MEDIA_MAX_PATH];

      used = 0;
      if(prefixBytes && (!MediaAppend(path, sizeof(path), &used, linkPrefix, prefixBytes) || !MediaAppend(path, sizeof(path), &used, "/", 1u))) {
         MediaToText(document, index);
         continue;
      }
      if(!MediaAppend(path, sizeof(path), &used, MediaNameOf(set, ui32(file)), MediaLength(MediaNameOf(set, ui32(file)), MEDIA_MAX_NAME_BYTES))) {
         MediaToText(document, index);
         continue;
      }
      if(!IrSetDest(document, index, path, used)) return false;

      IR_SPANptr rewritten = IrSpanMutable(document, index);

      if(rewritten) rewritten->flags = IR_SPAN_FLAG_NONE;
   }
   return true;
}

cEXIT_CODE MediaWrite(cMEDIA_SETptr set, OPC_PACKAGEptrc package, cwchptr mediaDir) {
   if(!set->count) return EXIT_ALL_CONVERTED;
   // An existing directory is not a failure: a second conversion into the same place is ordinary, and
   // the two runs' files are named the same way, so the second simply replaces the first.
   if(!CreateDirectoryW(mediaDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
      DiagErrorText("cannot create the media directory", mediaDir);
      return EXIT_OUTPUT;
   }

   ui64 base = 0;

   while(base < MEDIA_MAX_PATH - 1u && mediaDir[base]) ++base;
   if(!base || base + MEDIA_MAX_NAME_BYTES + 2u >= MEDIA_MAX_PATH) {
      DiagErrorText("the media path would be longer than this converter builds", mediaDir);
      return EXIT_OUTPUT;
   }
   for(ui32 index = 0; index < set->count; ++index) {
      wchar  path[MEDIA_MAX_PATH];
      cchptr leaf      = MediaNameOf(set, index);
      cui64  leafBytes = MediaLength(leaf, MEDIA_MAX_NAME_BYTES);
      ui64   used      = 0;

      for(; used < base; ++used) path[used] = mediaDir[used];
      // A separator is appended unless the directory already ends in one. Both are recognised, because
      // Windows accepts either and a command line carries whichever the user typed.
      if(path[used - 1u] != L'\\' && path[used - 1u] != L'/') path[used++] = L'\\';
      // The leaf is generated here and is ASCII by construction, so widening it is one byte per
      // character. No archive entry name ever reaches disk, which is correctness rule 10's other half.
      for(ui64 at = 0; at < leafBytes; ++at) path[used++] = wchar(ui8(leaf[at]));
      path[used] = 0;

      cOPC_RESULT loaded = OpcLoadPart(package, set->parts[index]);

      if(loaded != OPC_OK) {
         DiagErrorText(OpcResultText(package, loaded), mediaDir);
         return OpcExitCode(package, loaded);
      }

      cui8ptr bytes     = OpcPartBytes(package, set->parts[index]);
      cui64   byteCount = OpcPartByteCount(package, set->parts[index]);
      cHANDLE file      = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

      if(file == INVALID_HANDLE_VALUE) {
         DiagErrorText("cannot create media file", path);
         return EXIT_OUTPUT;
      }

      ui64 done = 0;

      while(done < byteCount) {
         cui64 remaining = byteCount - done;
         cui64 want      = (remaining > MEDIA_WRITE_BYTES ? MEDIA_WRITE_BYTES : remaining);
         DWORD put       = 0;

         if(!WriteFile(file, bytes + done, DWORD(want), &put, nullptr) || !put) {
            CloseHandle(file);
            DeleteFileW(path);
            DiagErrorText("cannot write media file", path);
            return EXIT_OUTPUT;
         }
         done += put;
      }
      if(!CloseHandle(file)) {
         DeleteFileW(path);
         DiagErrorText("cannot write media file", path);
         return EXIT_OUTPUT;
      }
   }
   return EXIT_ALL_CONVERTED;
}

cui64 MediaExtension(cchptr contentType, cchptr fallback, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   if(!destBytes) return 0;
   dest[0] = 0;
   if(contentType) {
      // A content type may carry parameters -- "image/png; charset=binary" -- which are not part of the
      // type. The match is against everything before the first semicolon, with trailing space removed.
      cui64 whole  = MediaLength(contentType, 128u);
      ui64  length = 0;

      while(length < whole && contentType[length] != ';') ++length;
      while(length && (contentType[length - 1u] == ' ' || contentType[length - 1u] == '\t')) --length;
      for(ui64 index = 0; index < MEDIA_TYPE_COUNT; ++index) {
         if(!MediaSameType(contentType, MEDIA_TYPES[index].type, length)) continue;
         MediaAppend(dest, destBytes, &used, MEDIA_TYPES[index].extension, MediaLength(MEDIA_TYPES[index].extension, destBytes));
         return used;
      }
   }
   // Nothing this build knows. The entry name's own extension is the next best answer, and it is taken
   // only when it is short and alphanumeric -- anything else is a name a producer meant as data.
   if(fallback) {
      cui64 length = MediaLength(fallback, MEDIA_MAX_PATH);
      ui64  dot    = length;

      while(dot && fallback[dot - 1u] != '.' && fallback[dot - 1u] != '/' && fallback[dot - 1u] != '\\') --dot;
      if(dot && fallback[dot - 1u] == '.' && length - dot >= 1u && length - dot <= 8u) {
         bool plain = true;

         for(ui64 index = dot; index < length; ++index) {
            cchar byte = fallback[index];

            if(!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9'))) plain = false;
         }
         if(plain) {
            MediaAppend(dest, destBytes, &used, ".", 1u);
            MediaAppend(dest, destBytes, &used, fallback + dot, length - dot);
            return used;
         }
      }
   }
   MediaAppend(dest, destBytes, &used, ".bin", 4u);
   return used;
}
