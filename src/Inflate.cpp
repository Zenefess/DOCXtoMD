/*
 * File: Inflate.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: RFC 1951 decoder: stored, fixed and dynamic blocks over canonical Huffman decode tables.
 * To Do: 1) Benchmark a wider match copy for non-overlapping distances before adopting one (bd1/bd2).
 *        2) Benchmark a wider primary lookup table; INFLATE_FAST_BITS is a guess, not a measurement.
 *        3) Add a streaming entry point if a part ever arrives that will not fit in one buffer.
 * Dependencies: BuildGuards.h, Inflate.h, typedefs.h, memory management.h, windows.h
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
#include "Inflate.h"

//== RFC 1951 constants

constexpr cui32 INFLATE_MAX_BITS        = 15u;  // Longest Huffman code the format allows
constexpr cui32 INFLATE_MAX_LIT_CODES   = 288u; // Literal/length alphabet, including the two never-valid codes
constexpr cui32 INFLATE_MAX_DIST_CODES  = 32u;  // Distance alphabet, including the two never-valid codes
constexpr cui32 INFLATE_MAX_CODES       = INFLATE_MAX_LIT_CODES;
constexpr cui32 INFLATE_CODE_LEN_CODES  = 19u;  // Code-length alphabet a dynamic block's header uses
constexpr cui32 INFLATE_LAST_LIT_SYMBOL = 285u; // Length symbols run 257..285; 286 and 287 never appear
constexpr cui32 INFLATE_LAST_DIST_CODE  = 29u;  // Distance symbols run 0..29; 30 and 31 never appear
constexpr cui32 INFLATE_MAX_HLIT        = 286u; // Largest literal/length alphabet a dynamic header may declare
constexpr cui32 INFLATE_MAX_HDIST       = 30u;  // Largest distance alphabet a dynamic header may declare
constexpr cui32 INFLATE_END_OF_BLOCK    = 256u; // The literal alphabet's one mandatory symbol

// Width of the primary decode table. Nine bits covers every fixed-Huffman literal and the large majority
// of dynamic ones; anything longer falls through to the canonical bit-at-a-time walk below.
constexpr cui32 INFLATE_FAST_BITS = 9u;
constexpr cui32 INFLATE_FAST_SIZE = 1u << INFLATE_FAST_BITS;

// A table slot holds (code length << 12) | symbol. Symbols stop at 287 and the table only holds codes of
// INFLATE_FAST_BITS or fewer, so the all-ones pattern cannot be a real entry and reads as "no match".
constexpr cui16 INFLATE_FAST_NONE = 0xFFFFu;

// The order a dynamic block stores its code-length code lengths in (RFC 1951 3.2.7).
constexpr cui8 INFLATE_LENGTH_ORDER[INFLATE_CODE_LEN_CODES] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

// Length symbol 257..285 -> base length and extra-bit count (RFC 1951 3.2.5). The row comments both
// document the mapping and stop the formatter reflowing a table whose shape is the point.
constexpr cui16 INFLATE_LENGTH_BASE[29]  = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27, // 257..271
                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};   // 272..285
constexpr cui8  INFLATE_LENGTH_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,                     // 257..271
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};                       // 272..285

// Distance symbol 0..29 -> base distance and extra-bit count (RFC 1951 3.2.5).
constexpr cui16 INFLATE_DIST_BASE[30]  = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,     // 0..9
                                          33,   49,   65,   97,   129,  193,  257,  385,   513,   769,    // 10..19
                                          1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577}; // 20..29
constexpr cui8  INFLATE_DIST_EXTRA[30] = {0, 0, 0,  0,  1,  1,  2,  2,  3,  3,                            // 0..9
                                          4, 4, 5,  5,  6,  6,  7,  7,  8,  8,                            // 10..19
                                          9, 9, 10, 10, 11, 11, 12, 12, 13, 13};                          // 20..29

//== Decoder state

// One inflate in progress. It lives on the caller's stack for the duration of InflateRaw and is never
// shared, so this module needs no lock under D6.
struct INFLATE_STATE {
   cui8ptr source;      // Compressed bytes
   ui64    sourceBytes; // Bytes available at source
   ui64    sourceIndex; // Next byte to pull into the bit buffer
   ui64    bitBuffer;   // Bits not yet consumed, least significant first
   ui32    bitCount;    // Bits held in bitBuffer, real and phantom together; above them it is always zero
   ui32    realBits;    // How many of those bits came from the input rather than from past its end
   bool    exhausted;   // A consume reached into bits the input does not contain
   ui8ptr  dest;        // Output buffer
   ui64    destBytes;   // Hard output cap
   ui64    destIndex;   // Bytes produced so far
};

typedef INFLATE_STATE *const INFLATE_STATEptrc;

// One canonical Huffman decoder: the counts and symbol order RFC 1951 3.2.2 defines, plus the primary
// lookup table derived from them.
struct INFLATE_HUFFMAN {
   ui16 count[INFLATE_MAX_BITS + 1]; // Codes of each length; count[0] is the unused symbols
   ui16 symbol[INFLATE_MAX_CODES];   // Symbols ordered by code, shortest code first
   ui16 fast[INFLATE_FAST_SIZE];     // (length << 12) | symbol, or INFLATE_FAST_NONE
};

typedef INFLATE_HUFFMAN *const       INFLATE_HUFFMANptrc;
typedef const INFLATE_HUFFMAN *const cINFLATE_HUFFMANptrc;

//-- Bit reader

// Loads at least `need` bits. Past the end of the input it appends phantom zero bits instead of reading
// on, and leaves them out of realBits: peeking past the end of a stream is normal -- the primary decode
// table is indexed by a fixed-width peek -- while consuming past it is what truncation actually means.
static void InflateFill(INFLATE_STATEptrc state, cui32 need) {
   while(state->bitCount < need) {
      if(state->sourceIndex >= state->sourceBytes) {
         state->bitCount += 8u; // bitBuffer already holds zero in those positions
         continue;
      }
      state->bitBuffer |= ui64(state->source[state->sourceIndex++]) << state->bitCount;
      state->bitCount += 8u;
      state->realBits += 8u;
   }
}

// Discards bits the caller has finished with, and raises `exhausted` if any of them were phantom.
static void InflateConsume(INFLATE_STATEptrc state, cui32 count) {
   if(count > state->realBits) {
      state->exhausted = true;
      state->realBits  = 0;
   } else state->realBits -= count;

   state->bitBuffer >>= count;
   state->bitCount -= count;
}

// Reads and consumes `need` bits, least significant first. `need` never exceeds 16 here, so a fill tops
// out at 23 bits and the 64-bit buffer cannot overflow.
static cui32 InflateBits(INFLATE_STATEptrc state, cui32 need) {
   InflateFill(state, need);

   cui32 value = ui32(state->bitBuffer & ((1ull << need) - 1ull));

   InflateConsume(state, need);
   return value;
}

// Reads `need` bits without consuming them, so the decoder can index its primary table before it knows
// how many bits the matching code actually uses.
static cui32 InflatePeek(INFLATE_STATEptrc state, cui32 need) {
   InflateFill(state, need);
   return ui32(state->bitBuffer & ((1ull << need) - 1ull));
}

//-- Table construction

// Reverses the low `bits` bits of a value. Canonical codes are numbered most significant bit first while
// the stream delivers them least significant bit first, so the primary table is indexed by the reversal.
static cui32 InflateReverseBits(cui32 value, cui32 bits) {
   ui32 result = 0;

   for(ui32 i = 0; i < bits; ++i) result |= ((value >> i) & 1u) << (bits - 1u - i);
   return result;
}

// Builds a canonical decoder from a code-length array (RFC 1951 3.2.2). Returns 0 for a complete code,
// the number of unused code patterns for an incomplete one, or -1 when the lengths over-subscribe the
// alphabet. Whether an incomplete code is legal is the caller's call, not this function's.
static csi32 InflateBuildHuffman(INFLATE_HUFFMANptrc table, cui8ptr lengths, cui32 codeCount) {
   ui16 offset[INFLATE_MAX_BITS + 1];
   si32 left = 1;

   for(ui32 length = 0; length <= INFLATE_MAX_BITS; ++length) table->count[length] = 0;
   for(ui32 i = 0; i < codeCount; ++i) ++table->count[lengths[i]];
   for(ui32 i = 0; i < INFLATE_FAST_SIZE; ++i) table->fast[i] = INFLATE_FAST_NONE;

   // Every symbol unused. Reported as complete, exactly as an empty distance alphabet must be: the table
   // is left matching nothing, so any attempt to decode from it fails in InflateDecode instead.
   if(ui32(table->count[0]) == codeCount) return 0;

   // Kraft's inequality walked one length at a time: `left` is the code space still unassigned.
   for(ui32 length = 1; length <= INFLATE_MAX_BITS; ++length) {
      left <<= 1;
      left -= si32(table->count[length]);
      if(left < 0) return -1;
   }

   offset[1] = 0;
   for(ui32 length = 1; length < INFLATE_MAX_BITS; ++length) offset[length + 1] = ui16(offset[length] + table->count[length]);
   for(ui32 i = 0; i < codeCount; ++i)
      if(lengths[i]) table->symbol[offset[lengths[i]]++] = ui16(i);

   // Fill the primary table by enumerating the canonical codes shortest first. Each short code owns every
   // index whose low bits match it, which is what lets one lookup return both the symbol and its length.
   ui32 code = 0, index = 0;

   for(ui32 length = 1; length <= INFLATE_FAST_BITS; ++length) {
      cui32 count = table->count[length];

      for(ui32 i = 0; i < count; ++i) {
         cui32 reversed = InflateReverseBits(code + i, length);
         cui16 entry    = ui16((length << 12) | table->symbol[index + i]);

         for(ui32 slot = reversed; slot < INFLATE_FAST_SIZE; slot += (1u << length)) table->fast[slot] = entry;
      }
      index += count;
      code = (code + count) << 1;
   }
   return left;
}

// Fills in the fixed literal/length and distance decoders RFC 1951 3.2.6 defines. Building these is not
// free -- two full table constructions, each clearing 512 primary slots and re-enumerating the canonical
// code space -- and an empty fixed block is ten bits, so a stream of nothing but empty fixed blocks would
// pay that cost about every byte. InflateBlocks therefore builds them once per stream; the flag it uses
// is on its own stack, so nothing is shared between workers (D6).
static void InflateBuildFixed(INFLATE_HUFFMANptrc lengthCode, INFLATE_HUFFMANptrc distCode) {
   ui8 lengths[INFLATE_MAX_LIT_CODES];

   for(ui32 i = 0; i < 144u; ++i) lengths[i] = 8u;
   for(ui32 i = 144u; i < 256u; ++i) lengths[i] = 9u;
   for(ui32 i = 256u; i < 280u; ++i) lengths[i] = 7u;
   for(ui32 i = 280u; i < INFLATE_MAX_LIT_CODES; ++i) lengths[i] = 8u;
   InflateBuildHuffman(lengthCode, lengths, INFLATE_MAX_LIT_CODES);

   // All 32 distance codes are five bits wide, which makes the code complete. Symbols 30 and 31 are still
   // undefined by the format, and InflateCodes refuses them rather than the table doing it.
   for(ui32 i = 0; i < INFLATE_MAX_DIST_CODES; ++i) lengths[i] = 5u;
   InflateBuildHuffman(distCode, lengths, INFLATE_MAX_DIST_CODES);
}

//-- Symbol decoding

// Decodes one symbol. Returns -1 when no code matches, which means either a corrupt stream or a table the
// caller was allowed to leave incomplete.
static csi32 InflateDecode(INFLATE_STATEptrc state, cINFLATE_HUFFMANptrc table) {
   cui16 entry = table->fast[InflatePeek(state, INFLATE_FAST_BITS)];

   if(entry != INFLATE_FAST_NONE) {
      InflateConsume(state, ui32(entry >> 12));
      return si32(entry & 0x0FFFu);
   }

   // No code of INFLATE_FAST_BITS or fewer matches, so walk the canonical code space one bit at a time.
   // `first` is the smallest code of the current length and `index` where that length's symbols start; a
   // code that is still unmatched at a given length always exceeds them both, so neither test can wrap.
   ui32 code = 0, first = 0, index = 0;

   for(ui32 length = 1; length <= INFLATE_MAX_BITS; ++length) {
      cui32 count = table->count[length];

      code |= InflateBits(state, 1u);
      if(code - first < count) return si32(table->symbol[index + (code - first)]);
      index += count;
      first = (first + count) << 1;
      code <<= 1;
   }
   return -1;
}

//-- Block decoding

// Copies a stored block through verbatim (RFC 1951 3.2.4).
static cINFLATE_RESULT InflateStoredBlock(INFLATE_STATEptrc state) {
   InflateConsume(state, state->bitCount & 7u); // A stored block's LEN starts on a byte boundary

   cui32 length  = InflateBits(state, 16u);
   cui32 nlength = InflateBits(state, 16u);

   if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
   if(length != (~nlength & 0xFFFFu)) return INFLATE_ERROR_STORED_LENGTH;
   if(state->destIndex + ui64(length) > state->destBytes) return INFLATE_ERROR_OVERFLOW;

   ui32 remaining = length;

   // Every consume since the alignment took a whole byte, so bitCount is a multiple of eight: draining it
   // eight bits at a time empties it exactly, and sourceIndex is then the true stream position.
   while(remaining && state->bitCount >= 8u) {
      state->dest[state->destIndex++] = ui8(InflateBits(state, 8u));
      --remaining;
   }
   if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
   if(state->sourceIndex + ui64(remaining) > state->sourceBytes) {
      state->exhausted = true;
      return INFLATE_ERROR_TRUNCATED;
   }
   Copy(state->source + state->sourceIndex, state->dest + state->destIndex, ui64(remaining));
   state->sourceIndex += ui64(remaining);
   state->destIndex += ui64(remaining);
   return INFLATE_OK;
}

// Reads a dynamic block's header and builds its two decoders (RFC 1951 3.2.7).
static cINFLATE_RESULT InflateBuildDynamic(INFLATE_STATEptrc state, INFLATE_HUFFMANptrc lengthCode, INFLATE_HUFFMANptrc distCode) {
   ui8 order[INFLATE_CODE_LEN_CODES]                         = {};
   ui8 coded[INFLATE_MAX_LIT_CODES + INFLATE_MAX_DIST_CODES] = {};

   cui32 litCount  = InflateBits(state, 5u) + 257u;
   cui32 distCount = InflateBits(state, 5u) + 1u;
   cui32 codeCount = InflateBits(state, 4u) + 4u;
   cui32 total     = litCount + distCount;

   // RFC 1951 3.2.7 caps HLIT at 286 and HDIST at 30, and zlib enforces both unless it is built with
   // PKZIP_BUG_WORKAROUND for the very old encoders that emitted 31 or 32 unused distance codes. The
   // alphabet constants above stay at 288 and 32 because the *fixed* tables really are that wide.
   if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
   if(litCount > INFLATE_MAX_HLIT || distCount > INFLATE_MAX_HDIST) return INFLATE_ERROR_CODE_LENGTHS;

   // The code-length alphabet, stored in the format's own scrambled order so the lengths that matter most
   // come first and a short header can leave the rest at zero.
   for(ui32 i = 0; i < codeCount; ++i) order[INFLATE_LENGTH_ORDER[i]] = ui8(InflateBits(state, 3u));
   if(state->exhausted) return INFLATE_ERROR_TRUNCATED;

   // lengthCode doubles as the code-length decoder: it is rebuilt from `coded` below, and reusing it keeps
   // a third 1.6 KiB table off the stack. This one must be complete -- an incomplete set is never legal.
   if(InflateBuildHuffman(lengthCode, order, INFLATE_CODE_LEN_CODES)) return INFLATE_ERROR_CODE_LENGTHS;

   ui32 filled = 0;

   while(filled < total) {
      csi32 symbol = InflateDecode(state, lengthCode);

      if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
      if(symbol < 0) return INFLATE_ERROR_CODE_LENGTHS;
      if(symbol < 16) {
         coded[filled++] = ui8(symbol);
         continue;
      }

      ui32 repeat = 0;
      ui8  value  = 0;

      if(symbol == 16) {
         if(!filled) return INFLATE_ERROR_CODE_LENGTHS; // Nothing to repeat yet
         value  = coded[filled - 1u];
         repeat = 3u + InflateBits(state, 2u);
      } else if(symbol == 17) repeat = 3u + InflateBits(state, 3u);
      else repeat = 11u + InflateBits(state, 7u);

      if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
      if(filled + repeat > total) return INFLATE_ERROR_CODE_LENGTHS;
      while(repeat--) coded[filled++] = value;
   }

   // Nothing but symbol 256 ends a compressed block, so a literal alphabet that cannot spell it describes
   // a block with no end -- corrupt however well formed the rest of the header is.
   if(!coded[INFLATE_END_OF_BLOCK]) return INFLATE_ERROR_CODE_LENGTHS;

   csi32 litLeft = InflateBuildHuffman(lengthCode, coded, litCount);

   // An incomplete code is legal in exactly one shape: the alphabet holds a single one-bit code, which
   // encoders emit for a block with one distance or one literal. Anything else is over- or under-subscribed.
   if(litLeft && (litLeft < 0 || litCount != ui32(lengthCode->count[0]) + ui32(lengthCode->count[1]))) return INFLATE_ERROR_HUFFMAN;

   csi32 distLeft = InflateBuildHuffman(distCode, coded + litCount, distCount);

   if(distLeft && (distLeft < 0 || distCount != ui32(distCode->count[0]) + ui32(distCode->count[1]))) return INFLATE_ERROR_HUFFMAN;
   return INFLATE_OK;
}

// Decodes literals and matches until the end-of-block symbol (RFC 1951 3.2.3).
static cINFLATE_RESULT InflateCodes(INFLATE_STATEptrc state, cINFLATE_HUFFMANptrc lengthCode, cINFLATE_HUFFMANptrc distCode) {
   for(;;) {
      csi32 symbol = InflateDecode(state, lengthCode);

      if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
      if(symbol < 0) return INFLATE_ERROR_HUFFMAN;
      if(ui32(symbol) < INFLATE_END_OF_BLOCK) {
         if(state->destIndex >= state->destBytes) return INFLATE_ERROR_OVERFLOW;
         state->dest[state->destIndex++] = ui8(symbol);
         continue;
      }
      if(ui32(symbol) == INFLATE_END_OF_BLOCK) return INFLATE_OK;
      if(ui32(symbol) > INFLATE_LAST_LIT_SYMBOL) return INFLATE_ERROR_SYMBOL;

      cui32 lengthIndex = ui32(symbol) - 257u;
      cui32 length      = ui32(INFLATE_LENGTH_BASE[lengthIndex]) + InflateBits(state, ui32(INFLATE_LENGTH_EXTRA[lengthIndex]));
      csi32 distSymbol  = InflateDecode(state, distCode);

      if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
      if(distSymbol < 0) return INFLATE_ERROR_HUFFMAN;
      if(ui32(distSymbol) > INFLATE_LAST_DIST_CODE) return INFLATE_ERROR_DISTANCE;

      cui32 distance = ui32(INFLATE_DIST_BASE[distSymbol]) + InflateBits(state, ui32(INFLATE_DIST_EXTRA[distSymbol]));

      if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
      if(ui64(distance) > state->destIndex) return INFLATE_ERROR_DISTANCE;
      if(state->destIndex + ui64(length) > state->destBytes) return INFLATE_ERROR_OVERFLOW;

      // Byte at a time on purpose: a match may overlap the bytes it is still producing, which is how
      // DEFLATE spells a run, so a wider copy would need a distance test first. bd1/bd2 gate that change.
      ui64 from = state->destIndex - ui64(distance);

      for(ui32 i = 0; i < length; ++i) state->dest[state->destIndex++] = state->dest[from++];
   }
}

// Walks the block chain until the one flagged final (RFC 1951 3.2.3).
static cINFLATE_RESULT InflateBlocks(INFLATE_STATEptrc state) {
   INFLATE_HUFFMAN lengthCode;
   INFLATE_HUFFMAN distCode;
   ui32            final      = 0;
   bool            fixedBuilt = false; // Whether lengthCode and distCode currently hold the fixed tables

   do {
      final = InflateBits(state, 1u);

      cui32 type = InflateBits(state, 2u);

      if(state->exhausted) return INFLATE_ERROR_TRUNCATED;
      if(type == 3u) return INFLATE_ERROR_BLOCK_TYPE;

      if(type == 0u) {
         cINFLATE_RESULT stored = InflateStoredBlock(state);

         if(stored != INFLATE_OK) return stored;
      } else {
         if(type == 1u) {
            if(!fixedBuilt) {
               InflateBuildFixed(&lengthCode, &distCode);
               fixedBuilt = true;
            }
         } else {
            cINFLATE_RESULT built = InflateBuildDynamic(state, &lengthCode, &distCode);

            if(built != INFLATE_OK) return built;

            // Load-bearing, not defensive: InflateBuildDynamic overwrites both tables, so a fixed block
            // after a dynamic one would otherwise be decoded with the dynamic block's codes.
            fixedBuilt = false;
         }

         cINFLATE_RESULT coded = InflateCodes(state, &lengthCode, &distCode);

         if(coded != INFLATE_OK) return coded;
      }
   } while(!final);
   return INFLATE_OK;
}

//== Entry points

cINFLATE_RESULT InflateRaw(cui8ptr source, cui64 sourceBytes, ui8ptrc dest, cui64 destBytes, ui64ptrc producedBytes) {
   INFLATE_STATE state;

   state.source      = source;
   state.sourceBytes = (source ? sourceBytes : 0);
   state.sourceIndex = 0;
   state.bitBuffer   = 0;
   state.bitCount    = 0;
   state.realBits    = 0;
   state.exhausted   = false;
   state.dest        = dest;
   state.destBytes   = (dest ? destBytes : 0);
   state.destIndex   = 0;

   cINFLATE_RESULT result = InflateBlocks(&state);

   *producedBytes = state.destIndex;
   return result;
}
