/*
 * File: Utf.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: UTF-8 validation over a compile-time lead-byte table, and UTF-16 transcoding.
 * To Do: 1) Benchmark an AVX2 ASCII fast path through UtfValidate before adopting one (bd1/bd2).
 *        2) Fold the surrogate walk into one shared routine if a third caller ever needs it.
 * Dependencies: BuildGuards.h, Utf.h, typedefs.h, memory management.h, windows.h
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
#include "Utf.h"

// UTF-16 is what wchar_t holds on the only platform this builds for, and UtfFromWide is where that is
// assumed. Nothing else in the project reads a wide character apart from comparing it, so this one
// assertion carries the whole dependence.
static_assert(sizeof(wchar) == 2u, "Utf: wchar_t is not 16 bits, so UtfFromWide's UTF-16 reading is wrong.");

//== Lead-byte table

// One row per lead byte, from Unicode 15.0 table 3-7. A sequence's length is fixed by its lead byte, and
// so is the range its *first* continuation byte may take: those narrowed ranges are what keep overlong
// forms, surrogates and code points above U+10FFFF out without a second pass over the decoded value.
struct UTF_LEAD {
   ui8 length;   ///< Bytes in the sequence, the lead included; 0 for a byte that cannot begin one
   ui8 first;    ///< Lowest byte the first continuation may take
   ui8 last;     ///< Highest byte the first continuation may take
   ui8 narrowed; ///< The result for a first continuation in 80..BF but outside first..last, and, when
                 ///< length is 0, the result for the lead byte itself
};

/// Constant form of UTF_LEAD, spelled per GCS r2.
typedef const UTF_LEAD cUTF_LEAD;

// Wrapped in a struct so a constexpr function can return the whole table by value.
struct UTF_LEAD_LOOKUP {
   UTF_LEAD slot[256];
};

// The row one lead byte selects. Written as a function so the table below is built in the compiler
// rather than by a run-time initialiser a worker could race.
static constexpr UTF_LEAD UtfLeadRow(cui32 lead) {
   if(lead < 0x80u) return {1u, 0u, 0u, ui8(UTF8_OK)};                     // 00..7F  ASCII
   if(lead < 0xC0u) return {0u, 0u, 0u, ui8(UTF8_ERROR_LEAD)};             // 80..BF  a continuation
   if(lead < 0xC2u) return {0u, 0u, 0u, ui8(UTF8_ERROR_OVERLONG)};         // C0..C1  always overlong
   if(lead < 0xE0u) return {2u, 0x80u, 0xBFu, ui8(UTF8_OK)};               // C2..DF  U+0080..U+07FF
   if(lead == 0xE0u) return {3u, 0xA0u, 0xBFu, ui8(UTF8_ERROR_OVERLONG)};  // E0      80..9F is overlong
   if(lead == 0xEDu) return {3u, 0x80u, 0x9Fu, ui8(UTF8_ERROR_SURROGATE)}; // ED      A0..BF is D800+
   if(lead < 0xF0u) return {3u, 0x80u, 0xBFu, ui8(UTF8_OK)};               // E1..EF  U+0800..U+FFFF
   if(lead == 0xF0u) return {4u, 0x90u, 0xBFu, ui8(UTF8_ERROR_OVERLONG)};  // F0      80..8F is overlong
   if(lead < 0xF4u) return {4u, 0x80u, 0xBFu, ui8(UTF8_OK)};               // F1..F3  U+40000..U+FFFFF
   if(lead == 0xF4u) return {4u, 0x80u, 0x8Fu, ui8(UTF8_ERROR_RANGE)};     // F4      90..BF is >10FFFF
   if(lead < 0xF8u) return {0u, 0u, 0u, ui8(UTF8_ERROR_RANGE)};            // F5..F7  above U+10FFFF
   return {0u, 0u, 0u, ui8(UTF8_ERROR_LEAD)};                              // F8..FF  never a lead byte
}

static constexpr UTF_LEAD_LOOKUP UtfBuildLeads(void) {
   UTF_LEAD_LOOKUP table = {};

   for(ui32 index = 0; index < 256u; ++index) table.slot[index] = UtfLeadRow(index);
   return table;
}

static constexpr UTF_LEAD_LOOKUP UTF_LEADS = UtfBuildLeads();

// The four rows that carry a narrowed range are the whole of the standard's ill-formed handling, so they
// are what a mistyped table loses first. C0 and F5 anchor the two lengths that no byte may begin.
static_assert(UTF_LEADS.slot[0xE0u].first == 0xA0u, "UTF-8 table: E0 must reject the overlong 80..9F continuations.");
static_assert(UTF_LEADS.slot[0xEDu].last == 0x9Fu, "UTF-8 table: ED must reject the surrogate A0..BF continuations.");
static_assert(UTF_LEADS.slot[0xF0u].first == 0x90u, "UTF-8 table: F0 must reject the overlong 80..8F continuations.");
static_assert(UTF_LEADS.slot[0xF4u].last == 0x8Fu, "UTF-8 table: F4 must reject the 90..BF continuations above U+10FFFF.");
static_assert(!UTF_LEADS.slot[0xC0u].length && !UTF_LEADS.slot[0xF5u].length, "UTF-8 table: C0 and F5 cannot begin a sequence.");

//== Sentences

// One sentence per UTF8_RESULT, in the order the enum declares them.
static constexpr cchptr UTF_RESULT_TEXT[UTF8_RESULT_COUNT] = {
    // Written for a user reading a console, so each names the part rather than the byte range
    "the text is well-formed UTF-8",                                                    // UTF8_OK
    "not a valid DOCX; a part holds a byte that cannot begin a UTF-8 sequence",         // LEAD
    "not a valid DOCX; a part ends in the middle of a UTF-8 sequence",                  // TRUNCATED
    "not a valid DOCX; a part holds a broken UTF-8 sequence",                           // CONTINUATION
    "not a valid DOCX; a part encodes a character in more UTF-8 bytes than it needs",   // OVERLONG
    "not a valid DOCX; a part encodes a UTF-16 surrogate, which UTF-8 has no form for", // SURROGATE
    "not a valid DOCX; a part encodes a character above U+10FFFF",                      // RANGE
    "not a valid DOCX; a UTF-16 part holds an unpaired surrogate",                      // UNPAIRED
    "not a valid DOCX; a UTF-16 part holds an odd number of bytes",                     // ODD_LENGTH
    "the converter asked to transcode text into a buffer too small to hold it",         // SPACE
    "not enough memory to transcode a part"                                             // MEMORY
};

//-- Surrogate folding

// Assembles one UTF-16 code unit out of two bytes, in whichever order the byte-order mark declared.
static cui32 UtfReadUnit(cui8ptr bytes, cui64 at, cbool bigEndian) {
   if(bigEndian) return (ui32(bytes[at]) << 8u) | ui32(bytes[at + 1u]);
   return ui32(bytes[at]) | (ui32(bytes[at + 1u]) << 8u);
}

// Folds one UTF-16 code unit, and the unit after it when the two open a surrogate pair, into one code
// point. UTF_REPLACEMENT stands for a lone surrogate, which the two callers then treat differently.
static cui32 UtfFoldPair(cui32 lead, cui32 trail, cbool hasTrail, boolptrc usedTrail) {
   *usedTrail = false;
   if(lead < 0xD800u || lead >= 0xE000u) return lead;
   if(lead >= 0xDC00u) return UTF_REPLACEMENT; // A trail surrogate with no lead in front of it
   if(!hasTrail || trail < 0xDC00u || trail >= 0xE000u) return UTF_REPLACEMENT;

   *usedTrail = true;
   return 0x10000u + ((lead - 0xD800u) << 10u) + (trail - 0xDC00u);
}

//== Entry points

cUTF8_RESULT UtfValidate(cui8ptr bytes, cui64 byteCount, ui64ptrc badOffset) {
   *badOffset = byteCount;
   if(!bytes) return UTF8_OK;

   ui64 at = 0;

   while(at < byteCount) {
      cUTF_LEAD row = UTF_LEADS.slot[bytes[at]];

      if(!row.length) {
         *badOffset = at;
         return UTF8_RESULT(row.narrowed);
      }
      if(row.length == 1u) {
         ++at;
         continue;
      }
      if(ui64(row.length) > byteCount - at) {
         *badOffset = at;
         return UTF8_ERROR_TRUNCATED;
      }

      cui32 following = bytes[at + 1u];

      // Out of 80..BF is a broken sequence; inside it but outside the row's narrowed range is one of the
      // three forms the standard excludes, and the row says which.
      if(following < 0x80u || following > 0xBFu) {
         *badOffset = at;
         return UTF8_ERROR_CONTINUATION;
      }
      if(following < row.first || following > row.last) {
         *badOffset = at;
         return UTF8_RESULT(row.narrowed);
      }
      for(ui32 index = 2u; index < ui32(row.length); ++index) {
         if(bytes[at + index] < 0x80u || bytes[at + index] > 0xBFu) {
            *badOffset = at;
            return UTF8_ERROR_CONTINUATION;
         }
      }
      at += ui64(row.length);
   }
   return UTF8_OK;
}

cui64 UtfBomBytes(cui8ptr bytes, cui64 byteCount) {
   if(!bytes) return 0;
   if(byteCount >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu) return 3u;
   if(byteCount >= 2u && bytes[0] == 0xFFu && bytes[1] == 0xFEu) return 2u;
   if(byteCount >= 2u && bytes[0] == 0xFEu && bytes[1] == 0xFFu) return 2u;
   return 0;
}

cUTF_ENCODING UtfDetectEncoding(cui8ptr bytes, cui64 byteCount) {
   if(!bytes || byteCount < 2u) return UTF_ENCODING_UTF8;
   if(bytes[0] == 0xFFu && bytes[1] == 0xFEu) return UTF_ENCODING_UTF16_LE;
   if(bytes[0] == 0xFEu && bytes[1] == 0xFFu) return UTF_ENCODING_UTF16_BE;
   return UTF_ENCODING_UTF8;
}

cui32 UtfDecode(cui8ptr bytes, cui64 byteCount, ui32ptrc codePoint) {
   *codePoint = UTF_REPLACEMENT;
   if(!bytes || !byteCount) return 0;

   cUTF_LEAD row = UTF_LEADS.slot[bytes[0]];

   if(!row.length || ui64(row.length) > byteCount) return 0;
   if(row.length == 1u) {
      *codePoint = ui32(bytes[0]);
      return 1u;
   }

   cui32 following = bytes[1];

   if(following < row.first || following > row.last) return 0;

   // The lead byte's payload is what its length leaves of the low seven bits: 1F, 0F or 07.
   ui32 point = ui32(bytes[0]) & (0x7Fu >> row.length);

   for(ui32 index = 1u; index < ui32(row.length); ++index) {
      if(bytes[index] < 0x80u || bytes[index] > 0xBFu) return 0;
      point = (point << 6u) | (ui32(bytes[index]) & 0x3Fu);
   }
   *codePoint = point;
   return ui32(row.length);
}

cui32 UtfEncode(cui32 codePoint, ui8ptrc dest) {
   if(codePoint < 0x80u) {
      dest[0] = ui8(codePoint);
      return 1u;
   }
   if(codePoint < 0x800u) {
      dest[0] = ui8(0xC0u | (codePoint >> 6u));
      dest[1] = ui8(0x80u | (codePoint & 0x3Fu));
      return 2u;
   }
   if(codePoint < 0x10000u) {
      if(codePoint >= 0xD800u && codePoint < 0xE000u) return 0; // A surrogate is not a scalar value
      dest[0] = ui8(0xE0u | (codePoint >> 12u));
      dest[1] = ui8(0x80u | ((codePoint >> 6u) & 0x3Fu));
      dest[2] = ui8(0x80u | (codePoint & 0x3Fu));
      return 3u;
   }
   if(codePoint > 0x10FFFFu) return 0;
   dest[0] = ui8(0xF0u | (codePoint >> 18u));
   dest[1] = ui8(0x80u | ((codePoint >> 12u) & 0x3Fu));
   dest[2] = ui8(0x80u | ((codePoint >> 6u) & 0x3Fu));
   dest[3] = ui8(0x80u | (codePoint & 0x3Fu));
   return 4u;
}

cUTF8_RESULT UtfFromWide(cwchptr text, ui8ptrc dest, cui64 destBytes, ui64ptrc producedBytes) {
   *producedBytes = 0;
   if(!text) return UTF8_OK;

   ui64 produced = 0;
   ui64 at       = 0;

   while(text[at]) {
      cui32 lead      = ui32(ui16(text[at]));
      cbool hasTrail  = (text[at + 1u] != 0);
      cui32 trail     = (hasTrail ? ui32(ui16(text[at + 1u])) : 0u);
      bool  usedTrail = false;
      cui32 point     = UtfFoldPair(lead, trail, hasTrail, &usedTrail);

      at += (usedTrail ? 2u : 1u);

      ui8   scratch[UTF_MAX_ENCODED];
      cui32 length = UtfEncode(point, scratch);

      if(dest) {
         if(ui64(length) > destBytes - produced) {
            *producedBytes = produced; // What was written before the room ran out, not a bare zero
            return UTF8_ERROR_SPACE;
         }
         for(ui32 index = 0; index < length; ++index) dest[produced + index] = scratch[index];
      }
      produced += ui64(length);
   }
   *producedBytes = produced;
   return UTF8_OK;
}

cUTF8_RESULT UtfTranscodeUtf16(cui8ptr bytes, cui64 byteCount, cbool bigEndian, ui8ptrptrc out, ui64ptrc outBytes) {
   *out      = nullptr;
   *outBytes = 0;
   if(!bytes) return UTF8_OK; // A null part is empty and owns nothing, so there is nothing to hand back
   if(byteCount & 1u) return UTF8_ERROR_ODD_LENGTH;

   // Only a UTF-16 mark may be skipped here. UtfBomBytes would also report the three bytes of a UTF-8
   // one, and skipping three would put every code unit one byte out of phase with the even-length
   // invariant the check above just established.
   cbool marked = (byteCount >= 2u && ((bytes[0] == 0xFFu && bytes[1] == 0xFEu) || (bytes[0] == 0xFEu && bytes[1] == 0xFFu)));
   cui64 skip   = (marked ? 2u : 0u);
   cui64 units  = (byteCount - skip) / 2u;

   // Two passes over the same units: the first says exactly how large the buffer must be, so the second
   // cannot overrun it and nothing has to grow. A code unit produces at most three UTF-8 bytes, and a
   // surrogate pair four from two units, so the total never exceeds one and a half times the input.
   ui64 needed = 0;

   for(ui32 pass = 0; pass < 2u; ++pass) {
      ui64 produced = 0;
      ui64 index    = 0;

      while(index < units) {
         cui64 at        = skip + index * 2u;
         cui32 lead      = UtfReadUnit(bytes, at, bigEndian);
         cbool hasTrail  = (index + 1u < units);
         cui32 trail     = (hasTrail ? UtfReadUnit(bytes, at + 2u, bigEndian) : 0u);
         bool  usedTrail = false;
         cui32 point     = UtfFoldPair(lead, trail, hasTrail, &usedTrail);

         // A part is content, not a console line: a surrogate that forms no pair is reported, not repaired.
         if(point == UTF_REPLACEMENT && lead >= 0xD800u && lead < 0xE000u) return UTF8_ERROR_UNPAIRED;
         index += (usedTrail ? 2u : 1u);

         ui8   scratch[UTF_MAX_ENCODED];
         cui32 length = UtfEncode(point, scratch);

         if(pass && *out)
            for(ui32 byte = 0; byte < length; ++byte) (*out)[produced + byte] = scratch[byte];
         produced += ui64(length);
      }
      if(pass) {
         *outBytes = produced;
         return UTF8_OK;
      }
      needed = produced;

      // One spare byte so a zero-length part still allocates something a caller can hold and free.
      *out = (ui8ptr)amalloc(needed + 1u, 16u);
      if(!*out) return UTF8_ERROR_MEMORY;
   }
   return UTF8_OK;
}

cchptr UtfResultText(cUTF8_RESULT result) {
   if(result < UTF8_OK || result >= UTF8_RESULT_COUNT) return "not a valid DOCX; a part is not readable text";
   return UTF_RESULT_TEXT[result];
}
