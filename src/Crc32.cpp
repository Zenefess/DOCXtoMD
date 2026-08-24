/*
 * File: Crc32.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: Table-driven ZIP CRC-32; the table is built at compile time, so no initialiser runs.
 * To Do: 1) Benchmark slice-by-8 folding against this table before adopting it (bd1/bd2).
 *        2) Benchmark PCLMULQDQ folding behind an a8 CPUID check; it is not in a2's named baseline.
 * Dependencies: BuildGuards.h, Crc32.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "Crc32.h"

//== Table

// ZIP uses IEEE 802.3 CRC-32 in its reflected form. The SSE4.2 _mm_crc32_u* intrinsics implement
// CRC-32C (Castagnoli), a different polynomial, and would silently validate nothing -- do not reach
// for them here. AVX2 buys this file nothing either, which is why its ISA field reads Scalar.
constexpr cui32 CRC32_POLYNOMIAL = 0xEDB88320u;

// Wrapped in a struct so a constexpr function can return the whole table by value.
struct CRC32_LOOKUP {
   ui32 slot[256];
};

// Builds the byte-at-a-time table. Being constexpr, it runs in the compiler: there is no run-time
// initialiser for a worker to race, which is what lets this module stay lock-free under D6.
static constexpr CRC32_LOOKUP Crc32BuildTable(void) {
   CRC32_LOOKUP table = {};

   for(ui32 index = 0; index < 256u; ++index) {
      ui32 value = index;

      for(ui32 bit = 0; bit < 8u; ++bit) value = ((value & 1u) ? ((value >> 1u) ^ CRC32_POLYNOMIAL) : (value >> 1u));
      table.slot[index] = value;
   }
   return table;
}

static constexpr CRC32_LOOKUP CRC32_TABLE = Crc32BuildTable();

// Two published anchors of the reflected table, which is where a mistyped polynomial shows up first.
// Entry 1 is the polynomial's own reflection; entry 255 is the value every reference table ends on.
static_assert(CRC32_TABLE.slot[1] == 0x77073096u, "CRC-32 table: entry 1 does not match the reflected 0xEDB88320 polynomial.");
static_assert(CRC32_TABLE.slot[255] == 0x2D02EF8Du, "CRC-32 table: entry 255 does not match the reflected 0xEDB88320 polynomial.");

//== Entry points

cui32 Crc32Update(cui32 crc, cui8ptr data, cui64 byteCount) {
   if(!data || !byteCount) return crc;

   ui32 value = ~crc;

   for(ui64 index = 0; index < byteCount; ++index) value = CRC32_TABLE.slot[(value ^ ui32(data[index])) & 0xFFu] ^ (value >> 8u);
   return ~value;
}

cui32 Crc32(cui8ptr data, cui64 byteCount) { return Crc32Update(0u, data, byteCount); }
