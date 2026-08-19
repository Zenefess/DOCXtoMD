/*
 * File: Inflate.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: First-party RFC 1951 DEFLATE decoder over a whole in-memory stream (decision D1).
 * To Do: 1) Benchmark a wider match copy for non-overlapping distances before adopting one (bd1/bd2).
 *        2) Add a streaming entry point if a part ever arrives that will not fit in one buffer.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Results

/// Why an inflate stopped. Every value but INFLATE_OK means the output must be discarded.
/// @note The wording a user sees lives in ZipReader, which is the layer that knows the stream came out
///       of a .docx; this module stays domain-free and reports only which RFC 1951 rule was broken.
enum INFLATE_RESULT : si32 {
   INFLATE_OK = 0,              ///< A final block ended the stream and every byte decoded
   INFLATE_ERROR_TRUNCATED,     ///< The stream needed bits that the input does not contain
   INFLATE_ERROR_OVERFLOW,      ///< The stream tried to produce more than the caller's output cap allows
   INFLATE_ERROR_BLOCK_TYPE,    ///< A block declared the reserved type 3
   INFLATE_ERROR_STORED_LENGTH, ///< A stored block's LEN and one's-complement NLEN disagree
   INFLATE_ERROR_CODE_LENGTHS,  ///< A dynamic block's code-length sequence is malformed
   INFLATE_ERROR_HUFFMAN,       ///< A Huffman table is over-subscribed, or a code matched no symbol
   INFLATE_ERROR_SYMBOL,        ///< A length symbol outside 257..285 was used
   INFLATE_ERROR_DISTANCE,      ///< A distance symbol is undefined, or the match reaches before the output
   INFLATE_RESULT_COUNT         ///< Number of values above; not a result
};

/// Constant form of INFLATE_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const INFLATE_RESULT cINFLATE_RESULT;

//== Entry points

/// Inflates one raw DEFLATE stream -- no zlib or gzip wrapper -- into a caller-owned buffer.
/// @param source         First byte of the compressed stream.
/// @param sourceBytes    Bytes available at source; reading stops there rather than running on.
/// @param dest           Output buffer; may be null only when destBytes is 0.
/// @param destBytes      Hard output cap. This is where a decompression bomb is stopped: the stream is
///                       refused the moment it tries to write past it, not afterwards from a header field.
/// @param producedBytes  Receives the number of bytes written, on success and on failure alike.
/// @return INFLATE_OK, or the rule that was broken. On failure the buffer holds partial, unusable output.
/// @note The 32 KiB match window RFC 1951 specifies is satisfied by keeping the whole output: a match may
///       reach back no further than the bytes already produced, which is checked on every match.
cINFLATE_RESULT InflateRaw(cui8ptr source, cui64 sourceBytes, ui8ptrc dest, cui64 destBytes, ui64ptrc producedBytes);
