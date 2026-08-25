/*
 * File: Utf.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: UTF-8 validation and decoding, and the UTF-16 transcoding the Win32 boundary needs.
 * To Do: 1) Benchmark an AVX2 ASCII fast path through UtfValidate before adopting one (bd1/bd2).
 *        2) Add UTF-8 to UTF-16 when an output path first has to be built out of document text.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Results

/// Why a byte range is not well-formed UTF-8. The classes are Unicode 15.0 table 3-7's, split so that a
/// message can say what is actually wrong rather than only that something is.
/// @note A part is refused outright when one of these is found. Decision D8 in CLAUDE.md ruled that on
///       2026-08-24, settling a disagreement between M4's definition of done and an earlier reading of
///       docs/CONVERSION_REFERENCE.md 5.12, which has since been corrected to match. U+FFFD is still
///       substituted in one place, UtfFromWide, because a console path reports a name rather than
///       converting document content.
enum UTF8_RESULT : si32 {
   UTF8_OK = 0,             ///< Every byte in the range is part of a well-formed sequence
   UTF8_ERROR_LEAD,         ///< A sequence began with a continuation byte, or with F8..FF
   UTF8_ERROR_TRUNCATED,    ///< A sequence needs continuation bytes that the range does not contain
   UTF8_ERROR_CONTINUATION, ///< A byte outside 80..BF stood where a continuation byte was required
   UTF8_ERROR_OVERLONG,     ///< A code point was encoded in more bytes than its shortest form needs
   UTF8_ERROR_SURROGATE,    ///< A UTF-16 surrogate, U+D800 to U+DFFF, was encoded as three bytes
   UTF8_ERROR_RANGE,        ///< A code point above U+10FFFF, which Unicode does not define
   UTF8_ERROR_UNPAIRED,     ///< UTF-16 input held a lone surrogate, so no code point could be formed
   UTF8_ERROR_ODD_LENGTH,   ///< UTF-16 input held an odd number of bytes, so a code unit is cut in half
   UTF8_ERROR_SPACE,        ///< The destination buffer is too small; a caller mistake, not bad input
   UTF8_ERROR_MEMORY,       ///< An allocation failed
   UTF8_RESULT_COUNT        ///< Number of values above; not a result
};

/// Constant form of UTF8_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const UTF8_RESULT cUTF8_RESULT;

/// What a part's leading bytes say it is encoded in. XML parts are UTF-8 in every producer seen so far,
/// but ISO/IEC 29500 permits UTF-16 and Word will read it, so the byte-order marks are recognised.
enum UTF_ENCODING : si32 {
   UTF_ENCODING_UTF8 = 0, ///< UTF-8, with or without a leading EF BB BF
   UTF_ENCODING_UTF16_LE, ///< UTF-16 little-endian, marked by FF FE
   UTF_ENCODING_UTF16_BE  ///< UTF-16 big-endian, marked by FE FF
};

/// Constant form of UTF_ENCODING, spelled per GCS r2.
typedef const UTF_ENCODING cUTF_ENCODING;

//== Constants

/// The replacement character, which the console path substitutes for text it cannot represent.
constexpr cui32 UTF_REPLACEMENT = 0xFFFDu;

/// The most bytes one code point occupies in UTF-8.
constexpr cui32 UTF_MAX_ENCODED = 4u;

//== Entry points

/// Checks a byte range for well-formed UTF-8.
/// @param bytes      First byte; a null pointer is an empty, valid range whatever byteCount says.
/// @param byteCount  Number of bytes to check.
/// @param badOffset  Receives the offset of the first byte of the offending sequence, or byteCount when
///                   the range is well formed. May not be null.
/// @return UTF8_OK, or which rule the range breaks.
/// @note Every XML part is put through this before it is tokenised, so XmlPull may assume its input is
///       well formed and walk it a byte at a time without re-checking each lead.
cUTF8_RESULT UtfValidate(cui8ptr bytes, cui64 byteCount, ui64ptrc badOffset);

/// Reports how many bytes of byte-order mark stand at the start of a range.
/// @param bytes      First byte.
/// @param byteCount  Number of bytes available.
/// @return 3 for a UTF-8 mark, 2 for either UTF-16 mark, otherwise 0.
cui64 UtfBomBytes(cui8ptr bytes, cui64 byteCount);

/// Reports what a part's leading bytes say it is encoded in.
/// @param bytes      First byte.
/// @param byteCount  Number of bytes available.
/// @return The encoding a byte-order mark declares, or UTF_ENCODING_UTF8 when there is no mark.
/// @note Only the mark is consulted. An XML declaration naming some other encoding is not honoured,
///       because no producer emits one; a UTF-32 mark is read as UTF-16, and its parts then fail to
///       tokenise, which is a better outcome than pretending to support an encoding nothing writes.
cUTF_ENCODING UtfDetectEncoding(cui8ptr bytes, cui64 byteCount);

/// Decodes the one code point a range starts with.
/// @param bytes      First byte of the sequence.
/// @param byteCount  Bytes available from there; decoding never reads past this.
/// @param codePoint  Receives the code point, or UTF_REPLACEMENT when the sequence is ill formed.
/// @return The number of bytes the sequence occupies, or 0 when it is ill formed.
cui32 UtfDecode(cui8ptr bytes, cui64 byteCount, ui32ptrc codePoint);

/// Encodes one code point as UTF-8.
/// @param codePoint  A Unicode scalar value: at most U+10FFFF, and not a surrogate.
/// @param dest       Receives the bytes; must have room for UTF_MAX_ENCODED of them.
/// @return The number of bytes written, or 0 when the code point is not a scalar value.
cui32 UtfEncode(cui32 codePoint, ui8ptrc dest);

/// Transcodes a NUL-terminated wide string to UTF-8, terminator excluded.
/// @param text           NUL-terminated UTF-16; a null pointer produces nothing.
/// @param dest           Receives the UTF-8 bytes, unterminated. Null measures instead of writing.
/// @param destBytes      Bytes available at dest; ignored when dest is null.
/// @param producedBytes  Receives the number of bytes written, or needed when dest is null.
///                       On UTF8_ERROR_SPACE it is what was written before the room ran out.
/// @return UTF8_OK, or UTF8_ERROR_SPACE when a non-null dest is too small to hold the result.
/// @note A lone surrogate becomes U+FFFD rather than failing. This is the console and path boundary,
///       where a name that cannot be represented should still be reported rather than swallowed; part
///       bytes take the strict route, through UtfValidate and UtfTranscodeUtf16.
/// @note wchar_t is UTF-16 on the only platform this builds for, and a static assertion in the
///       implementation pins that. It is the whole of the project's dependence on the width.
cUTF8_RESULT UtfFromWide(cwchptr text, ui8ptrc dest, cui64 destBytes, ui64ptrc producedBytes);

/// Transcodes a UTF-16 part into a freshly allocated UTF-8 buffer.
/// @param bytes      First byte of the part. A leading UTF-16 mark is consumed here; a UTF-8 one is
///                   not, because this entry point reads UTF-16 and three bytes is not a whole
///                   number of code units. A null pointer is an empty part and yields a null out.
/// @param byteCount  Bytes in the part.
/// @param bigEndian  true for UTF-16BE, false for UTF-16LE, as UtfDetectEncoding reported.
/// @param out        Receives the buffer, which the caller releases with mdealloc. Null on failure.
/// @param outBytes   Receives the number of bytes in that buffer.
/// @return UTF8_OK, or why the part could not be transcoded.
/// @note Unlike the console path this refuses a lone surrogate: a part is document content, and content
///       that cannot be decoded is the case M4's definition of done says to report rather than repair.
cUTF8_RESULT UtfTranscodeUtf16(cui8ptr bytes, cui64 byteCount, cbool bigEndian, ui8ptrptrc out, ui64ptrc outBytes);

/// The user-facing sentence for a result, ready to hand to DiagErrorText.
/// @param result  The result to describe.
/// @return A static, NUL-terminated ASCII sentence with no trailing punctuation.
cchptr UtfResultText(cUTF8_RESULT result);
