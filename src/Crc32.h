/*
 * File: Crc32.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: ZIP CRC-32: IEEE 802.3, reflected polynomial 0xEDB88320, over one or many byte ranges.
 * To Do: 1) Benchmark slice-by-8 folding against this table before adopting it (bd1/bd2).
 *        2) Benchmark PCLMULQDQ folding behind an a8 CPUID check; it is not in a2's named baseline.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Entry points

/// Folds one byte range into a running CRC-32, so an entry may be verified in pieces.
/// @param crc        Running value; 0 starts a new checksum, otherwise a value this function returned.
/// @param data       First byte to fold in; a null pointer folds nothing in.
/// @param byteCount  Number of bytes to fold in.
/// @return The CRC-32 of everything folded in so far, ready to compare against a ZIP directory entry.
/// @note The pre- and post-inversion the standard specifies happens inside each call, so the value passed
///       in and the value returned are both finished checksums rather than an internal running state.
cui32 Crc32Update(cui32 crc, cui8ptr data, cui64 byteCount);

/// Computes the CRC-32 of one whole byte range.
/// @param data       First byte; a null pointer yields the checksum of an empty range.
/// @param byteCount  Number of bytes.
/// @return The CRC-32, in the form a ZIP local or central directory header stores.
cui32 Crc32(cui8ptr data, cui64 byteCount);
