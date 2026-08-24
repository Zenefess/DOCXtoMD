/*
 * File: TestUtf.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: Unit tests for Utf: the ill-formed classes of table 3-7, and the UTF-16 boundary.
 * To Do: 1) Add a sweep over every lead byte once a second implementation exists to compare against.
 * Dependencies: BuildGuards.h, Check.h, Utf.h, typedefs.h, memory management.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include <windows.h>
#include "typedefs.h"
#include "memory management.h"
#include "Utf.h"
#include "Check.h"

//-- Helpers

// Runs one byte string through UtfValidate and reports both the class and where it stopped.
static cbool UtfCase(cchptr bytes, cui64 byteCount, cUTF8_RESULT want, cui64 wantOffset) {
   ui64 offset = 0;

   cUTF8_RESULT got = UtfValidate((cui8ptr)bytes, byteCount, &offset);

   return got == want && offset == wantOffset;
}

// Encodes one code point and compares the bytes against a literal.
static cbool UtfEncodeCase(cui32 point, cchptr want, cui32 wantLength) {
   ui8   buffer[UTF_MAX_ENCODED] = {};
   cui32 length                  = UtfEncode(point, buffer);

   if(length != wantLength) return false;
   for(ui32 index = 0; index < length; ++index) {
      if(buffer[index] != ui8(want[index])) return false;
   }
   return true;
}

// Transcodes a wide string and compares the bytes against a literal.
static cbool UtfWideCase(cwchptr text, cchptr want, cui64 wantBytes) {
   ui8  buffer[64] = {};
   ui64 produced   = 0;

   if(UtfFromWide(text, nullptr, 0, &produced) != UTF8_OK || produced != wantBytes) return false;
   if(UtfFromWide(text, buffer, sizeof(buffer), &produced) != UTF8_OK || produced != wantBytes) return false;
   for(ui64 index = 0; index < wantBytes; ++index) {
      if(buffer[index] != ui8(want[index])) return false;
   }
   return true;
}

// Whether a NUL-terminated string ends with another.
static cbool TextEndsWith(cchptr text, cchptr tail) {
   ui64 textLength = 0;
   ui64 tailLength = 0;

   while(text[textLength]) ++textLength;
   while(tail[tailLength]) ++tailLength;
   if(tailLength > textLength) return false;
   for(ui64 index = 0; index < tailLength; ++index) {
      if(text[textLength - tailLength + index] != tail[index]) return false;
   }
   return true;
}

//== Entry point

void TestUtf(void) {
   CheckGroup("Utf: well-formed sequences");
   CHECK(UtfCase("", 0, UTF8_OK, 0));
   CHECK(UtfCase("plain ASCII", 11u, UTF8_OK, 11u));
   CHECK(UtfCase("\xC2\xA0", 2u, UTF8_OK, 2u));         // U+00A0, the lowest two-byte form
   CHECK(UtfCase("\xDF\xBF", 2u, UTF8_OK, 2u));         // U+07FF, the highest
   CHECK(UtfCase("\xE0\xA0\x80", 3u, UTF8_OK, 3u));     // U+0800, the lowest three-byte form
   CHECK(UtfCase("\xED\x9F\xBF", 3u, UTF8_OK, 3u));     // U+D7FF, just below the surrogates
   CHECK(UtfCase("\xEE\x80\x80", 3u, UTF8_OK, 3u));     // U+E000, just above them
   CHECK(UtfCase("\xEF\xBF\xBD", 3u, UTF8_OK, 3u));     // U+FFFD
   CHECK(UtfCase("\xF0\x90\x80\x80", 4u, UTF8_OK, 4u)); // U+10000, the lowest four-byte form
   CHECK(UtfCase("\xF4\x8F\xBF\xBF", 4u, UTF8_OK, 4u)); // U+10FFFF, the highest code point
   CHECK(UtfCase("\xEF\xBB\xBFtext", 7u, UTF8_OK, 7u)); // A byte-order mark is a valid U+FEFF

   CheckGroup("Utf: ill-formed sequences, one class each");
   CHECK(UtfCase("\x80", 1u, UTF8_ERROR_LEAD, 0));    // A continuation byte in the lead position
   CHECK(UtfCase("ab\xBF", 3u, UTF8_ERROR_LEAD, 2u)); // and the offset names it, not the start
   CHECK(UtfCase("\xF8", 1u, UTF8_ERROR_LEAD, 0));    // F8..FF are not lead bytes at all
   CHECK(UtfCase("\xFF", 1u, UTF8_ERROR_LEAD, 0));
   CHECK(UtfCase("\xC0\x80", 2u, UTF8_ERROR_OVERLONG, 0));     // U+0000 written in two bytes
   CHECK(UtfCase("\xC1\xBF", 2u, UTF8_ERROR_OVERLONG, 0));     // U+007F written in two bytes
   CHECK(UtfCase("\xE0\x80\x80", 3u, UTF8_ERROR_OVERLONG, 0)); // U+0000 written in three
   CHECK(UtfCase("\xE0\x9F\xBF", 3u, UTF8_ERROR_OVERLONG, 0)); // U+07FF written in three
   CHECK(UtfCase("\xF0\x8F\xBF\xBF", 4u, UTF8_ERROR_OVERLONG, 0));
   CHECK(UtfCase("\xED\xA0\x80", 3u, UTF8_ERROR_SURROGATE, 0)); // U+D800
   CHECK(UtfCase("\xED\xBF\xBF", 3u, UTF8_ERROR_SURROGATE, 0)); // U+DFFF
   CHECK(UtfCase("\xF4\x90\x80\x80", 4u, UTF8_ERROR_RANGE, 0)); // U+110000
   CHECK(UtfCase("\xF5\x80\x80\x80", 4u, UTF8_ERROR_RANGE, 0)); // F5 can only start something too large
   CHECK(UtfCase("\xC2", 1u, UTF8_ERROR_TRUNCATED, 0));
   CHECK(UtfCase("\xE0\xA0", 2u, UTF8_ERROR_TRUNCATED, 0));
   CHECK(UtfCase("\xF0\x90\x80", 3u, UTF8_ERROR_TRUNCATED, 0));
   CHECK(UtfCase("\xC2\x41", 2u, UTF8_ERROR_CONTINUATION, 0));
   CHECK(UtfCase("\xE1\x41\x80", 3u, UTF8_ERROR_CONTINUATION, 0));
   CHECK(UtfCase("\xE1\x80\x41", 3u, UTF8_ERROR_CONTINUATION, 0));
   CHECK(UtfCase("\xF1\x80\x80\x41", 4u, UTF8_ERROR_CONTINUATION, 0));

   CheckGroup("Utf: byte-order marks");
   CHECK(UtfBomBytes((cui8ptr) "\xEF\xBB\xBF", 3u) == 3u);
   CHECK(UtfBomBytes((cui8ptr) "\xFF\xFE", 2u) == 2u);
   CHECK(UtfBomBytes((cui8ptr) "\xFE\xFF", 2u) == 2u);
   CHECK(UtfBomBytes((cui8ptr) "<?xml", 5u) == 0);
   CHECK(UtfBomBytes((cui8ptr) "\xEF\xBB", 2u) == 0);
   CHECK(UtfDetectEncoding((cui8ptr) "<?xml", 5u) == UTF_ENCODING_UTF8);
   CHECK(UtfDetectEncoding((cui8ptr) "\xEF\xBB\xBF<", 4u) == UTF_ENCODING_UTF8);
   CHECK(UtfDetectEncoding((cui8ptr) "\xFF\xFE<", 3u) == UTF_ENCODING_UTF16_LE);
   CHECK(UtfDetectEncoding((cui8ptr) "\xFE\xFF\x00", 3u) == UTF_ENCODING_UTF16_BE);

   CheckGroup("Utf: decoding and encoding one code point");

   ui32 point = 0;

   CHECK(UtfDecode((cui8ptr) "A", 1u, &point) == 1u && point == 0x41u);
   CHECK(UtfDecode((cui8ptr) "\xC2\xA0", 2u, &point) == 2u && point == 0xA0u);
   CHECK(UtfDecode((cui8ptr) "\xE2\x82\xAC", 3u, &point) == 3u && point == 0x20ACu);
   CHECK(UtfDecode((cui8ptr) "\xF0\x9F\x98\x80", 4u, &point) == 4u && point == 0x1F600u);
   CHECK(UtfDecode((cui8ptr) "\xED\xA0\x80", 3u, &point) == 0 && point == UTF_REPLACEMENT);
   CHECK(UtfDecode((cui8ptr) "\xC2", 1u, &point) == 0);
   CHECK(UtfDecode(nullptr, 0, &point) == 0);
   CHECK(UtfEncodeCase(0x41u, "A", 1u));
   CHECK(UtfEncodeCase(0x7Fu, "\x7F", 1u));
   CHECK(UtfEncodeCase(0x80u, "\xC2\x80", 2u));
   CHECK(UtfEncodeCase(0x7FFu, "\xDF\xBF", 2u));
   CHECK(UtfEncodeCase(0x800u, "\xE0\xA0\x80", 3u));
   CHECK(UtfEncodeCase(0xFFFFu, "\xEF\xBF\xBF", 3u));
   CHECK(UtfEncodeCase(0x10000u, "\xF0\x90\x80\x80", 4u));
   CHECK(UtfEncodeCase(0x10FFFFu, "\xF4\x8F\xBF\xBF", 4u));
   CHECK(UtfEncodeCase(0xD800u, "", 0));   // A surrogate is not a scalar value
   CHECK(UtfEncodeCase(0x110000u, "", 0)); // and neither is anything past U+10FFFF

   CheckGroup("Utf: the wide boundary");

   cwchar ascii[]  = {wchar('h'), wchar('i'), 0};
   cwchar euro[]   = {wchar(0x20ACu), 0};
   cwchar pair[]   = {wchar(0xD83Du), wchar(0xDE00u), 0}; // U+1F600
   cwchar lone[]   = {wchar(0xD83Du), wchar('x'), 0};     // A lead surrogate with no trail behind it
   cwchar orphan[] = {wchar(0xDE00u), 0};                 // A trail surrogate with no lead in front

   CHECK(UtfWideCase(ascii, "hi", 2u));
   CHECK(UtfWideCase(euro, "\xE2\x82\xAC", 3u));
   CHECK(UtfWideCase(pair, "\xF0\x9F\x98\x80", 4u));
   CHECK(UtfWideCase(lone,
                     "\xEF\xBF\xBD"
                     "x",
                     4u)); // Replaced, because a console line still prints
   CHECK(UtfWideCase(orphan, "\xEF\xBF\xBD", 3u));

   ui64 produced   = 0;
   ui8  cramped[2] = {};

   CHECK(UtfFromWide(nullptr, nullptr, 0, &produced) == UTF8_OK && produced == 0);
   CHECK(UtfFromWide(euro, cramped, sizeof(cramped), &produced) == UTF8_ERROR_SPACE);
   CHECK(!produced); // U+20AC needs three bytes and two were offered, so nothing was written

   ui8 narrow[3] = {};

   // A buffer that fits the first character and not the second reports the first, not a bare zero.
   cwchar mixed[] = {wchar('A'), wchar(0x20ACu), 0};

   CHECK(UtfFromWide(mixed, narrow, sizeof(narrow), &produced) == UTF8_ERROR_SPACE);
   CHECK(produced == 1u && narrow[0] == 'A');

   CheckGroup("Utf: transcoding a UTF-16 part");

   ui8ptr out      = nullptr;
   ui64   outBytes = 0;

   CHECK(UtfTranscodeUtf16((cui8ptr) "\xFF\xFE"
                                     "h\0i\0",
                           6u, false, &out, &outBytes) == UTF8_OK);
   CHECK(outBytes == 2u && out && out[0] == 'h' && out[1] == 'i');
   mdealloc(out);
   CHECK(UtfTranscodeUtf16((cui8ptr) "\xFE\xFF\x00h\x00i", 6u, true, &out, &outBytes) == UTF8_OK);
   CHECK(outBytes == 2u && out && out[0] == 'h' && out[1] == 'i');
   mdealloc(out);
   CHECK(UtfTranscodeUtf16((cui8ptr) "\x3D\xD8\x00\xDE", 4u, false, &out, &outBytes) == UTF8_OK);
   CHECK(outBytes == 4u && out && out[0] == 0xF0u && out[1] == 0x9Fu && out[2] == 0x98u && out[3] == 0x80u);
   mdealloc(out);
   CHECK(UtfTranscodeUtf16((cui8ptr) "h\0i", 3u, false, &out, &outBytes) == UTF8_ERROR_ODD_LENGTH);
   CHECK(UtfTranscodeUtf16((cui8ptr) "\x3D\xD8", 2u, false, &out, &outBytes) == UTF8_ERROR_UNPAIRED);
   CHECK(!out); // Nothing is handed back when the transcode fails

   // A UTF-8 mark is not a whole number of UTF-16 code units, so it is left where it is rather than
   // skipped: skipping three bytes would read every unit after it one byte out of phase.
   CHECK(UtfTranscodeUtf16((cui8ptr) "\xEF\xBB\xBF\x41\x00\x42", 6u, false, &out, &outBytes) == UTF8_OK);
   // Three code units of nonsense at three UTF-8 bytes each, not one character and a dropped byte.
   CHECK(outBytes == 9u && out);
   mdealloc(out);

   // An empty part still hands back a buffer, because UTF8_OK promises one.
   CHECK(UtfTranscodeUtf16((cui8ptr) "", 0, false, &out, &outBytes) == UTF8_OK);
   CHECK(out && !outBytes);
   mdealloc(out);
   CHECK(UtfTranscodeUtf16(nullptr, 0, false, &out, &outBytes) == UTF8_OK && !out);

   CheckGroup("Utf: sentences");
   CHECK(UtfResultText(UTF8_OK) && UtfResultText(UTF8_ERROR_MEMORY));
   CHECK(UtfResultText(UTF8_RESULT(-1)) && UtfResultText(UTF8_RESULT_COUNT));
   // Pinned against the enum: a sentence table and the values it is indexed by drift apart
   // silently, so each row below is the tail of the sentence its own value must map to.
   CHECK(TextEndsWith(UtfResultText(UTF8_ERROR_OVERLONG), "more UTF-8 bytes than it needs"));
   CHECK(TextEndsWith(UtfResultText(UTF8_ERROR_SURROGATE), "which UTF-8 has no form for"));
   CHECK(TextEndsWith(UtfResultText(UTF8_ERROR_ODD_LENGTH), "an odd number of bytes"));
   CHECK(TextEndsWith(UtfResultText(UTF8_ERROR_SPACE), "too small to hold it"));
   CHECK(TextEndsWith(UtfResultText(UTF8_ERROR_MEMORY), "transcode a part"));
}
