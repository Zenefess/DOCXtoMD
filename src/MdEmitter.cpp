/*
 * File: MdEmitter.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: Line assembly, inline delimiters, the blank-line discipline and every block kind's shape.
 * To Do: 1) Keep a per-line prefix stack when list items nest at M8 and a quote holds one at M8 or M9.
 *        2) Emit a table's pipe rows through MD_CONTEXT_TABLE_CELL at M9, which has no caller yet.
 *        3) Emit the link and image contexts at M7, which have no caller yet either.
 *        4) Size the buffer from the part's byte count rather than growing from a fixed first block.
 * Dependencies: BuildGuards.h, CliOptions.h, Ir.h, MdEmitter.h, MdEscape.h, Utf.h, typedefs.h,
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
#include "CliOptions.h"
#include "Ir.h"
#include "MdEscape.h"
#include "Utf.h"
#include "MdEmitter.h"

//-- Constants

// The first allocation of each buffer. A document of a few paragraphs never needs a second one, and a
// large one reaches its size in a handful of doublings.
constexpr cui64 MD_FIRST_BYTES = 4096u;

// The deepest ATX heading GitHub-Flavored Markdown has.
constexpr cui32 MD_MAX_HEADING = 6u;

// The shortest fence CommonMark accepts, whatever the content holds.
constexpr cui64 MD_MIN_FENCE = 3u;

// The prefix every line of a blockquote carries, and the line that joins two of them into one quote.
static constexpr cchptr MD_QUOTE_PREFIX = "> ";
static constexpr cchptr MD_QUOTE_JOIN   = ">";

//-- Buffers

// Grows one buffer to hold at least the requested number of bytes, doubling so that filling it costs
// amortised constant time.
static cbool MdGrow(MD_EMITTERptrc emitter, chptrptrc block, ui64ptrc capacity, cui64 needed) {
   if(needed <= *capacity) return true;

   ui64 grown = (*capacity ? *capacity : MD_FIRST_BYTES);

   while(grown < needed) grown *= 2u;

   chptr fresh = (chptr)amalloc(grown, 32u);

   if(!fresh) {
      emitter->failed = true;
      return false;
   }
   if(*block) Copy(*block, fresh, *capacity);
   mdealloc(*block);
   *block    = fresh;
   *capacity = grown;
   return true;
}

// Appends raw bytes to the output, which are markup this module wrote itself or text already escaped.
static cbool MdAppend(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount) {
   if(!byteCount) return true;
   if(!MdGrow(emitter, &emitter->out, &emitter->capacity, emitter->used + byteCount)) return false;
   Copy(bytes, emitter->out + emitter->used, byteCount);
   emitter->used += byteCount;
   return true;
}

// Appends one byte to the output.
static cbool MdAppendByte(MD_EMITTERptrc emitter, cchar byte) { return MdAppend(emitter, &byte, 1u); }

// Appends a NUL-terminated literal to the output.
static cbool MdAppendText(MD_EMITTERptrc emitter, cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return MdAppend(emitter, text, length);
}

// Escapes text straight into the output, for a context whose content is not assembled into a line. The
// fenced code block is the only one: its content is literal, so it needs neither the line-start pass nor
// a delimiter around it -- but it still goes through the escaping writer, because correctness rule 6
// says walker and emitter code never concatenate raw text into the output.
static cbool MdAppendEscaped(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cMD_CONTEXT context) {
   cui64 wanted = MdEscapeMeasure(bytes, byteCount, context, false);

   if(!wanted) return true;
   if(!MdGrow(emitter, &emitter->out, &emitter->capacity, emitter->used + wanted)) return false;
   emitter->used += MdEscapeWrite(emitter->out + emitter->used, wanted, bytes, byteCount, context, false);
   return true;
}

// Appends the same byte several times to the output, which is how a fence and a heading are written.
static cbool MdAppendRun(MD_EMITTERptrc emitter, cchar byte, cui64 count) {
   for(ui64 index = 0; index < count; ++index) {
      if(!MdAppendByte(emitter, byte)) return false;
   }
   return true;
}

//-- The line being assembled

// Appends finished bytes to the line: either markup this module wrote or text already escaped. Since M6
// a line is assembled in its output form rather than raw, because there is now markup between the spans
// and an escaping pass over the whole line would escape the markup along with the text.
static cbool MdLineAppend(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount) {
   if(!byteCount) return true;
   if(!MdGrow(emitter, &emitter->line, &emitter->lineCapacity, emitter->lineUsed + byteCount)) return false;
   Copy(bytes, emitter->line + emitter->lineUsed, byteCount);
   emitter->lineUsed += byteCount;
   return true;
}

// Appends a NUL-terminated literal of markup to the line.
static cbool MdLineText(MD_EMITTERptrc emitter, cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return MdLineAppend(emitter, text, length);
}

// Appends the same byte several times to the line, which is how a backtick delimiter is written.
static cbool MdLineRun(MD_EMITTERptrc emitter, cchar byte, cui64 count) {
   for(ui64 index = 0; index < count; ++index) {
      if(!MdLineAppend(emitter, &byte, 1u)) return false;
   }
   return true;
}

// Escapes text into the line for the context it is standing in.
static cbool MdLineEscaped(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cMD_CONTEXT context, cbool dollars) {
   cui64 wanted = MdEscapeMeasure(bytes, byteCount, context, dollars);

   if(!wanted) return true;
   if(!MdGrow(emitter, &emitter->line, &emitter->lineCapacity, emitter->lineUsed + wanted)) return false;
   emitter->lineUsed += MdEscapeWrite(emitter->line + emitter->lineUsed, wanted, bytes, byteCount, context, dollars);
   return true;
}

// Inserts one byte into the line at an offset, shifting whatever follows it up by one. Only ever used to
// put the single backslash the line-start pass asks for at the head of a line, so the tail moved is short.
static cbool MdLineInsert(MD_EMITTERptrc emitter, cui64 at, cchar byte) {
   if(!MdGrow(emitter, &emitter->line, &emitter->lineCapacity, emitter->lineUsed + 1u)) return false;
   for(ui64 index = emitter->lineUsed; index > at; --index) emitter->line[index] = emitter->line[index - 1u];
   emitter->line[at] = byte;
   emitter->lineUsed += 1u;
   return true;
}

//-- Small tests

// Whether a byte is one of the two Markdown treats as insignificant at the ends of a line.
static cbool MdIsPad(cchar byte) { return byte == ' ' || byte == '\t'; }

// The longest run of backticks in a range, which is what a code delimiter has to be longer than.
static cui64 MdLongestTickRun(cchptr bytes, cui64 byteCount) {
   ui64 longest = 0;
   ui64 run     = 0;

   for(ui64 index = 0; index < byteCount; ++index) {
      run = (bytes[index] == '`' ? run + 1u : 0);
      if(run > longest) longest = run;
   }
   return longest;
}

//-- Flanking

// The three classes CommonMark's flanking rules divide characters into. The beginning and the end of a
// line count as whitespace, which is what makes a delimiter at either edge of a line always safe.
enum MD_EDGE : si32 {
   MD_EDGE_SPACE = 0, ///< Unicode whitespace, or the edge of the line
   MD_EDGE_PUNCT,     ///< Unicode punctuation
   MD_EDGE_WORD       ///< Anything else, which is what "a letter" means here
};

typedef const MD_EDGE cMD_EDGE;

// Every range of code points CommonMark counts as punctuation, which for the flanking rules means the
// Unicode general categories P and S together. The table is the whole of them rather than a chosen
// subset, because both ways of being wrong cost something real: a letter called punctuation writes one
// HTML element where a delimiter would have done, and a punctuation character called a letter writes a
// delimiter that does not render at all. It is generated from the Unicode character database and sorted,
// so a reader who doubts a row can regenerate the table and diff it, and a binary search answers it.
struct MD_RANGE {
   ui32 first; ///< First code point of the range
   ui32 last;  ///< Last code point of the range
};

static constexpr MD_RANGE MD_PUNCTUATION[] = {
    {0x0021u, 0x002Fu},   {0x003Au, 0x0040u},   {0x005Bu, 0x0060u},   {0x007Bu, 0x007Eu},   // Exclamation Mark
    {0x00A1u, 0x00A9u},   {0x00ABu, 0x00ACu},   {0x00AEu, 0x00B1u},   {0x00B4u, 0x00B4u},   // Inverted Exclamation Mark
    {0x00B6u, 0x00B8u},   {0x00BBu, 0x00BBu},   {0x00BFu, 0x00BFu},   {0x00D7u, 0x00D7u},   // Pilcrow Sign
    {0x00F7u, 0x00F7u},   {0x02C2u, 0x02C5u},   {0x02D2u, 0x02DFu},   {0x02E5u, 0x02EBu},   // Division Sign
    {0x02EDu, 0x02EDu},   {0x02EFu, 0x02FFu},   {0x0375u, 0x0375u},   {0x037Eu, 0x037Eu},   // Modifier Letter Unaspirated
    {0x0384u, 0x0385u},   {0x0387u, 0x0387u},   {0x03F6u, 0x03F6u},   {0x0482u, 0x0482u},   // Greek Tonos
    {0x055Au, 0x055Fu},   {0x0589u, 0x058Au},   {0x058Du, 0x058Fu},   {0x05BEu, 0x05BEu},   // Armenian Apostrophe
    {0x05C0u, 0x05C0u},   {0x05C3u, 0x05C3u},   {0x05C6u, 0x05C6u},   {0x05F3u, 0x05F4u},   // Hebrew Punctuation Paseq
    {0x0606u, 0x060Fu},   {0x061Bu, 0x061Bu},   {0x061Du, 0x061Fu},   {0x066Au, 0x066Du},   // Arabic-Indic Cube Root
    {0x06D4u, 0x06D4u},   {0x06DEu, 0x06DEu},   {0x06E9u, 0x06E9u},   {0x06FDu, 0x06FEu},   // Arabic Full Stop
    {0x0700u, 0x070Du},   {0x07F6u, 0x07F9u},   {0x07FEu, 0x07FFu},   {0x0830u, 0x083Eu},   // Syriac End Of Paragraph
    {0x085Eu, 0x085Eu},   {0x0888u, 0x0888u},   {0x0964u, 0x0965u},   {0x0970u, 0x0970u},   // Mandaic Punctuation
    {0x09F2u, 0x09F3u},   {0x09FAu, 0x09FBu},   {0x09FDu, 0x09FDu},   {0x0A76u, 0x0A76u},   // Bengali Rupee Mark
    {0x0AF0u, 0x0AF1u},   {0x0B70u, 0x0B70u},   {0x0BF3u, 0x0BFAu},   {0x0C77u, 0x0C77u},   // Gujarati Abbreviation Sign
    {0x0C7Fu, 0x0C7Fu},   {0x0C84u, 0x0C84u},   {0x0D4Fu, 0x0D4Fu},   {0x0D79u, 0x0D79u},   // Telugu Sign Tuumu
    {0x0DF4u, 0x0DF4u},   {0x0E3Fu, 0x0E3Fu},   {0x0E4Fu, 0x0E4Fu},   {0x0E5Au, 0x0E5Bu},   // Sinhala Punctuation Kunddaliya
    {0x0F01u, 0x0F17u},   {0x0F1Au, 0x0F1Fu},   {0x0F34u, 0x0F34u},   {0x0F36u, 0x0F36u},   // Tibetan Mark Gter Yig Mgo Truncated
    {0x0F38u, 0x0F38u},   {0x0F3Au, 0x0F3Du},   {0x0F85u, 0x0F85u},   {0x0FBEu, 0x0FC5u},   // Tibetan Mark Che Mgo
    {0x0FC7u, 0x0FCCu},   {0x0FCEu, 0x0FDAu},   {0x104Au, 0x104Fu},   {0x109Eu, 0x109Fu},   // Tibetan Symbol Rdo Rje Rgya Gram
    {0x10FBu, 0x10FBu},   {0x1360u, 0x1368u},   {0x1390u, 0x1399u},   {0x1400u, 0x1400u},   // Georgian Paragraph Separator
    {0x166Du, 0x166Eu},   {0x169Bu, 0x169Cu},   {0x16EBu, 0x16EDu},   {0x1735u, 0x1736u},   // Canadian Syllabics Chi Sign
    {0x17D4u, 0x17D6u},   {0x17D8u, 0x17DBu},   {0x1800u, 0x180Au},   {0x1940u, 0x1940u},   // Khmer Sign Khan
    {0x1944u, 0x1945u},   {0x19DEu, 0x19FFu},   {0x1A1Eu, 0x1A1Fu},   {0x1AA0u, 0x1AA6u},   // Limbu Exclamation Mark
    {0x1AA8u, 0x1AADu},   {0x1B5Au, 0x1B6Au},   {0x1B74u, 0x1B7Eu},   {0x1BFCu, 0x1BFFu},   // Tai Tham Sign Kaan
    {0x1C3Bu, 0x1C3Fu},   {0x1C7Eu, 0x1C7Fu},   {0x1CC0u, 0x1CC7u},   {0x1CD3u, 0x1CD3u},   // Lepcha Punctuation Ta-Rol
    {0x1FBDu, 0x1FBDu},   {0x1FBFu, 0x1FC1u},   {0x1FCDu, 0x1FCFu},   {0x1FDDu, 0x1FDFu},   // Greek Koronis
    {0x1FEDu, 0x1FEFu},   {0x1FFDu, 0x1FFEu},   {0x2010u, 0x2027u},   {0x2030u, 0x205Eu},   // Greek Dialytika And Varia
    {0x207Au, 0x207Eu},   {0x208Au, 0x208Eu},   {0x20A0u, 0x20C0u},   {0x2100u, 0x2101u},   // Superscript Plus Sign
    {0x2103u, 0x2106u},   {0x2108u, 0x2109u},   {0x2114u, 0x2114u},   {0x2116u, 0x2118u},   // Degree Celsius
    {0x211Eu, 0x2123u},   {0x2125u, 0x2125u},   {0x2127u, 0x2127u},   {0x2129u, 0x2129u},   // Prescription Take
    {0x212Eu, 0x212Eu},   {0x213Au, 0x213Bu},   {0x2140u, 0x2144u},   {0x214Au, 0x214Du},   // Estimated Symbol
    {0x214Fu, 0x214Fu},   {0x218Au, 0x218Bu},   {0x2190u, 0x2426u},   {0x2440u, 0x244Au},   // Symbol For Samaritan Source
    {0x249Cu, 0x24E9u},   {0x2500u, 0x2775u},   {0x2794u, 0x2B73u},   {0x2B76u, 0x2B95u},   // Parenthesized Latin Small Letter A
    {0x2B97u, 0x2BFFu},   {0x2CE5u, 0x2CEAu},   {0x2CF9u, 0x2CFCu},   {0x2CFEu, 0x2CFFu},   // Symbol For Type A Electronics
    {0x2D70u, 0x2D70u},   {0x2E00u, 0x2E2Eu},   {0x2E30u, 0x2E5Du},   {0x2E80u, 0x2E99u},   // Tifinagh Separator Mark
    {0x2E9Bu, 0x2EF3u},   {0x2F00u, 0x2FD5u},   {0x2FF0u, 0x2FFBu},   {0x3001u, 0x3004u},   // Cjk Radical Choke
    {0x3008u, 0x3020u},   {0x3030u, 0x3030u},   {0x3036u, 0x3037u},   {0x303Du, 0x303Fu},   // Left Angle Bracket
    {0x309Bu, 0x309Cu},   {0x30A0u, 0x30A0u},   {0x30FBu, 0x30FBu},   {0x3190u, 0x3191u},   // Katakana-Hiragana Voiced Sound Mark
    {0x3196u, 0x319Fu},   {0x31C0u, 0x31E3u},   {0x3200u, 0x321Eu},   {0x322Au, 0x3247u},   // Ideographic Annotation Top Mark
    {0x3250u, 0x3250u},   {0x3260u, 0x327Fu},   {0x328Au, 0x32B0u},   {0x32C0u, 0x33FFu},   // Partnership Sign
    {0x4DC0u, 0x4DFFu},   {0xA490u, 0xA4C6u},   {0xA4FEu, 0xA4FFu},   {0xA60Du, 0xA60Fu},   // Hexagram For The Creative Heaven
    {0xA673u, 0xA673u},   {0xA67Eu, 0xA67Eu},   {0xA6F2u, 0xA6F7u},   {0xA700u, 0xA716u},   // Slavonic Asterisk
    {0xA720u, 0xA721u},   {0xA789u, 0xA78Au},   {0xA828u, 0xA82Bu},   {0xA836u, 0xA839u},   // Modifier Letter Stress And High Tone
    {0xA874u, 0xA877u},   {0xA8CEu, 0xA8CFu},   {0xA8F8u, 0xA8FAu},   {0xA8FCu, 0xA8FCu},   // Phags-Pa Single Head Mark
    {0xA92Eu, 0xA92Fu},   {0xA95Fu, 0xA95Fu},   {0xA9C1u, 0xA9CDu},   {0xA9DEu, 0xA9DFu},   // Kayah Li Sign Cwi
    {0xAA5Cu, 0xAA5Fu},   {0xAA77u, 0xAA79u},   {0xAADEu, 0xAADFu},   {0xAAF0u, 0xAAF1u},   // Cham Punctuation Spiral
    {0xAB5Bu, 0xAB5Bu},   {0xAB6Au, 0xAB6Bu},   {0xABEBu, 0xABEBu},   {0xFB29u, 0xFB29u},   // Modifier Breve With Inverted Breve
    {0xFBB2u, 0xFBC2u},   {0xFD3Eu, 0xFD4Fu},   {0xFDCFu, 0xFDCFu},   {0xFDFCu, 0xFDFFu},   // Arabic Symbol Dot Above
    {0xFE10u, 0xFE19u},   {0xFE30u, 0xFE52u},   {0xFE54u, 0xFE66u},   {0xFE68u, 0xFE6Bu},   // Presentation Form For Vertical Comma
    {0xFF01u, 0xFF0Fu},   {0xFF1Au, 0xFF20u},   {0xFF3Bu, 0xFF40u},   {0xFF5Bu, 0xFF65u},   // Fullwidth Exclamation Mark
    {0xFFE0u, 0xFFE6u},   {0xFFE8u, 0xFFEEu},   {0xFFFCu, 0xFFFDu},   {0x10100u, 0x10102u}, // Fullwidth Cent Sign
    {0x10137u, 0x1013Fu}, {0x10179u, 0x10189u}, {0x1018Cu, 0x1018Eu}, {0x10190u, 0x1019Cu}, // Aegean Weight Base Unit
    {0x101A0u, 0x101A0u}, {0x101D0u, 0x101FCu}, {0x1039Fu, 0x1039Fu}, {0x103D0u, 0x103D0u}, // Greek Symbol Tau Rho
    {0x1056Fu, 0x1056Fu}, {0x10857u, 0x10857u}, {0x10877u, 0x10878u}, {0x1091Fu, 0x1091Fu}, // Caucasian Albanian Citation Mark
    {0x1093Fu, 0x1093Fu}, {0x10A50u, 0x10A58u}, {0x10A7Fu, 0x10A7Fu}, {0x10AC8u, 0x10AC8u}, // Lydian Triangular Mark
    {0x10AF0u, 0x10AF6u}, {0x10B39u, 0x10B3Fu}, {0x10B99u, 0x10B9Cu}, {0x10EADu, 0x10EADu}, // Manichaean Punctuation Star
    {0x10F55u, 0x10F59u}, {0x10F86u, 0x10F89u}, {0x11047u, 0x1104Du}, {0x110BBu, 0x110BCu}, // Sogdian Punctuation Two Vertical Bar
    {0x110BEu, 0x110C1u}, {0x11140u, 0x11143u}, {0x11174u, 0x11175u}, {0x111C5u, 0x111C8u}, // Kaithi Section Mark
    {0x111CDu, 0x111CDu}, {0x111DBu, 0x111DBu}, {0x111DDu, 0x111DFu}, {0x11238u, 0x1123Du}, // Sharada Sutra Mark
    {0x112A9u, 0x112A9u}, {0x1144Bu, 0x1144Fu}, {0x1145Au, 0x1145Bu}, {0x1145Du, 0x1145Du}, // Multani Section Mark
    {0x114C6u, 0x114C6u}, {0x115C1u, 0x115D7u}, {0x11641u, 0x11643u}, {0x11660u, 0x1166Cu}, // Tirhuta Abbreviation Sign
    {0x116B9u, 0x116B9u}, {0x1173Cu, 0x1173Fu}, {0x1183Bu, 0x1183Bu}, {0x11944u, 0x11946u}, // Takri Abbreviation Sign
    {0x119E2u, 0x119E2u}, {0x11A3Fu, 0x11A46u}, {0x11A9Au, 0x11A9Cu}, {0x11A9Eu, 0x11AA2u}, // Nandinagari Sign Siddham
    {0x11C41u, 0x11C45u}, {0x11C70u, 0x11C71u}, {0x11EF7u, 0x11EF8u}, {0x11FD5u, 0x11FF1u}, // Bhaiksuki Danda
    {0x11FFFu, 0x11FFFu}, {0x12470u, 0x12474u}, {0x12FF1u, 0x12FF2u}, {0x16A6Eu, 0x16A6Fu}, // Tamil Punctuation End Of Text
    {0x16AF5u, 0x16AF5u}, {0x16B37u, 0x16B3Fu}, {0x16B44u, 0x16B45u}, {0x16E97u, 0x16E9Au}, // Bassa Vah Full Stop
    {0x16FE2u, 0x16FE2u}, {0x1BC9Cu, 0x1BC9Cu}, {0x1BC9Fu, 0x1BC9Fu}, {0x1CF50u, 0x1CFC3u}, // Old Chinese Hook Mark
    {0x1D000u, 0x1D0F5u}, {0x1D100u, 0x1D126u}, {0x1D129u, 0x1D164u}, {0x1D16Au, 0x1D16Cu}, // Byzantine Musical Symbol Psili
    {0x1D183u, 0x1D184u}, {0x1D18Cu, 0x1D1A9u}, {0x1D1AEu, 0x1D1EAu}, {0x1D200u, 0x1D241u}, // Musical Symbol Arpeggiato Up
    {0x1D245u, 0x1D245u}, {0x1D300u, 0x1D356u}, {0x1D6C1u, 0x1D6C1u}, {0x1D6DBu, 0x1D6DBu}, // Greek Musical Leimma
    {0x1D6FBu, 0x1D6FBu}, {0x1D715u, 0x1D715u}, {0x1D735u, 0x1D735u}, {0x1D74Fu, 0x1D74Fu}, // Mathematical Italic Nabla
    {0x1D76Fu, 0x1D76Fu}, {0x1D789u, 0x1D789u}, {0x1D7A9u, 0x1D7A9u}, {0x1D7C3u, 0x1D7C3u}, // Mathematical Sans-Serif Bold Nabla
    {0x1D800u, 0x1D9FFu}, {0x1DA37u, 0x1DA3Au}, {0x1DA6Du, 0x1DA74u}, {0x1DA76u, 0x1DA83u}, // Signwriting Hand-Fist Index
    {0x1DA85u, 0x1DA8Bu}, {0x1E14Fu, 0x1E14Fu}, {0x1E2FFu, 0x1E2FFu}, {0x1E95Eu, 0x1E95Fu}, // Signwriting Location Torso
    {0x1ECACu, 0x1ECACu}, {0x1ECB0u, 0x1ECB0u}, {0x1ED2Eu, 0x1ED2Eu}, {0x1EEF0u, 0x1EEF1u}, // Indic Siyaq Placeholder
    {0x1F000u, 0x1F02Bu}, {0x1F030u, 0x1F093u}, {0x1F0A0u, 0x1F0AEu}, {0x1F0B1u, 0x1F0BFu}, // Mahjong Tile East Wind
    {0x1F0C1u, 0x1F0CFu}, {0x1F0D1u, 0x1F0F5u}, {0x1F10Du, 0x1F1ADu}, {0x1F1E6u, 0x1F202u}, // Playing Card Ace Of Diamonds
    {0x1F210u, 0x1F23Bu}, {0x1F240u, 0x1F248u}, {0x1F250u, 0x1F251u}, {0x1F260u, 0x1F265u}, // Squared Cjk Unified Ideograph-624B
    {0x1F300u, 0x1F6D7u}, {0x1F6DDu, 0x1F6ECu}, {0x1F6F0u, 0x1F6FCu}, {0x1F700u, 0x1F773u}, // Cyclone
    {0x1F780u, 0x1F7D8u}, {0x1F7E0u, 0x1F7EBu}, {0x1F7F0u, 0x1F7F0u}, {0x1F800u, 0x1F80Bu}, // Black Left-Pointing Isosceles Right
    {0x1F810u, 0x1F847u}, {0x1F850u, 0x1F859u}, {0x1F860u, 0x1F887u}, {0x1F890u, 0x1F8ADu}, // Leftwards Arrow With Small Equilater
    {0x1F8B0u, 0x1F8B1u}, {0x1F900u, 0x1FA53u}, {0x1FA60u, 0x1FA6Du}, {0x1FA70u, 0x1FA74u}, // Arrow Pointing Upwards Then North We
    {0x1FA78u, 0x1FA7Cu}, {0x1FA80u, 0x1FA86u}, {0x1FA90u, 0x1FAACu}, {0x1FAB0u, 0x1FABAu}, // Drop Of Blood
    {0x1FAC0u, 0x1FAC5u}, {0x1FAD0u, 0x1FAD9u}, {0x1FAE0u, 0x1FAE7u}, {0x1FAF0u, 0x1FAF6u}, // Anatomical Heart
    {0x1FB00u, 0x1FB92u}, {0x1FB94u, 0x1FBCAu}                                              // Block Sextant-1
};

// Which class one code point falls into. Whitespace is only what CommonMark's flanking rules ever meet
// at a span's edge: the emitter has already trimmed a line's own padding and hoisted the rest, and the
// three below are what a document can still put there.
static cMD_EDGE MdEdgeOfPoint(cui32 point) {
   if(point == ' ' || point == '\t' || point == 0x00A0u || point == 0x3000u) return MD_EDGE_SPACE;

   ui64 low  = 0;
   ui64 high = sizeof(MD_PUNCTUATION) / sizeof(MD_PUNCTUATION[0]);

   while(low < high) {
      cui64 middle = low + (high - low) / 2u;

      if(point < MD_PUNCTUATION[middle].first) high = middle;
      else if(point > MD_PUNCTUATION[middle].last) low = middle + 1u;
      else return MD_EDGE_PUNCT;
   }
   return MD_EDGE_WORD;
}

// Which class the character a run of bytes begins with falls into.
static cMD_EDGE MdEdgeAt(cchptr bytes, cui64 byteCount) {
   ui32 point = 0;

   if(!byteCount) return MD_EDGE_SPACE;
   if(!UtfDecode((cui8ptr)bytes, byteCount, &point)) return MD_EDGE_WORD;
   return MdEdgeOfPoint(point);
}

// Which class the character a run of bytes *ends* with falls into. The last byte may be a continuation
// byte, so the scan steps back to the lead byte before decoding forward from it.
static cMD_EDGE MdEdgeBefore(cchptr bytes, cui64 byteCount) {
   ui64 at = byteCount;

   if(!byteCount) return MD_EDGE_SPACE;
   while(at && (ui8(bytes[at - 1u]) & 0xC0u) == 0x80u) --at;
   if(!at) return MD_EDGE_WORD;
   return MdEdgeAt(bytes + at - 1u, byteCount - at + 1u);
}

// The class of the character a span's opening delimiter would stand behind.
//
// Not simply the last byte of the line: CommonMark reads a run of identical delimiter characters as one
// delimiter run, so an opening "**" written straight after a closing "***" is not a run of two preceded
// by an asterisk -- it is part of a run of five, and what precedes *that* is what the flanking rules
// look at. Stepping back over the run is what makes "***a*****b**" come out judged the way a parser
// judges it, and it is also why a backslash-escaped asterisk in the text before does not fool the test:
// the step lands on the backslash, which is punctuation either way.
static cMD_EDGE MdEdgeBehind(cchptr line, cui64 used, cchar delimiter) {
   ui64 at = used;

   while(at && line[at - 1u] == delimiter) --at;
   return MdEdgeBefore(line, at);
}

// Whether a delimiter run may open and close where it is being put.
//
// CommonMark will not let a delimiter run open where it is both preceded by a letter and followed by
// punctuation, nor close where it is both followed by a letter and preceded by punctuation -- so
// "word**(a)**" emits four literal asterisks and loses the emphasis entirely, exactly as "**bold **"
// does for the whitespace CONVERSION_REFERENCE 5.3 hoists. Hoisting has already removed the whitespace
// cases; this is the punctuation half of the same rule, and what it cannot fix it reports, so the
// caller can reach for an HTML element instead, which has no flanking rule at all.
static cbool MdFlankingSafe(cMD_EDGE behind, cchptr bytes, cui64 byteCount, cMD_EDGE ahead) {
   cMD_EDGE first = MdEdgeAt(bytes, byteCount);
   cMD_EDGE last  = MdEdgeBefore(bytes, byteCount);

   if(first == MD_EDGE_PUNCT && behind == MD_EDGE_WORD) return false;
   if(last == MD_EDGE_PUNCT && ahead == MD_EDGE_WORD) return false;
   return true;
}

// Whether a strikethrough has to be written as raw HTML rather than as "~~".
//
// It does whenever it wraps another delimiter, and the reason is CommonMark's flanking rules. A "~~"
// immediately followed by a "*" or a backtick is followed by punctuation, so it may only open where the
// character before it is whitespace or punctuation too -- and mid-sentence that character is a letter,
// so "word~~**x**~~" opens nothing and four literal tildes reach the reader. The closing run fails the
// mirror-image test at the same moment. Two "~~" runs that meet fail differently and just as
// completely: "~~a~~~~b~~" is a run of four tildes, which GFM's strikethrough does not recognise at
// all. Raw HTML has no flanking rule of any kind, so <del> is immune to both, and "~~" is kept wherever
// it wraps nothing but text -- which is the spelling the mapping table rules and the one a reader of
// the .md expects to see. Every one of the 260 combinations a differential test found broken carried a
// strikethrough, and none that did not.
static cbool MdStrikeAsHtml(cui32 fmt, cbool safe) {
   if(!(fmt & IR_FMT_STRIKE)) return false;
   if(fmt & (IR_FMT_BOLD | IR_FMT_ITALIC | IR_FMT_CODE)) return true;
   return !safe;
}

// Whether the emphasis has to be written as raw HTML, for the same reason and in the same position.
// Only when there is no strikethrough: a strikethrough that wraps emphasis is an element by the rule
// above, and an element shields everything inside it from the flanking rules.
static cbool MdEmphasisAsHtml(cui32 fmt, cbool safe) {
   if(fmt & (IR_FMT_STRIKE | IR_FMT_CODE)) return false;
   return (fmt & (IR_FMT_BOLD | IR_FMT_ITALIC)) != 0 && !safe;
}

// Which escaping context a span's own formatting puts its text in. Code wins over everything, because
// nothing inside a code span is escaped at all; a raw-HTML wrapper takes MD_CONTEXT_HTML, because
// GitHub-Flavored Markdown still parses the text between the tags as inline content.
static cMD_CONTEXT MdSpanContext(cui32 fmt, cbool safe) {
   if(fmt & IR_FMT_CODE) return MD_CONTEXT_CODE_SPAN;
   if(fmt & (IR_FMT_SUPER | IR_FMT_SUB)) return MD_CONTEXT_HTML;
   if(MdStrikeAsHtml(fmt, safe) || MdEmphasisAsHtml(fmt, safe)) return MD_CONTEXT_HTML;
   return MD_CONTEXT_INLINE;
}

//-- Spans

// Writes one span's text into the line, wrapped in whatever delimiters its formatting calls for.
//
// The nesting is fixed, outermost first: the raw-HTML wrapper of a superscript or a subscript, then the
// strikethrough, then the emphasis, then the code span. Two of those orderings are choices worth naming.
// Bold and italic together are "***" rather than "**" around "*", which mapping row 5 fixes so that no
// document depends on which of the two the producer happened to specify first. And code drops bold and
// italic, which is CONVERSION_REFERENCE row 11's own ruling on the collision -- but it does *not* drop
// strikethrough or the vertical alignment, because those wrap a code span perfectly well in GFM and
// dropping them would lose formatting the reference never asked to lose. The strikethrough changes
// spelling when it wraps anything at all -- see MdStrikeAsHtml for why "~~" cannot survive there.
static cbool MdWriteSpan(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cui32 fmt, cbool dollars, cMD_EDGE ahead) {
   if(!byteCount) return true;

   // A superscript or a subscript is an HTML element, and an element shields everything inside it from
   // the flanking rules -- so only a span with no such wrapper has to be tested against its neighbours,
   // and only its outermost Markdown delimiter, since that is the one that faces them.
   cbool strike   = (fmt & IR_FMT_STRIKE) != 0;
   cbool emphasis = (fmt & (IR_FMT_BOLD | IR_FMT_ITALIC)) != 0;
   cbool shielded = (fmt & (IR_FMT_SUPER | IR_FMT_SUB)) != 0;
   cbool wrapping = strike && (emphasis || (fmt & IR_FMT_CODE) != 0);
   cbool tested   = !shielded && !wrapping && (strike || emphasis);
   // Which character the outermost delimiter is made of is settled before the verdict and not by it: the
   // verdict only decides whether that delimiter is written or an HTML element takes its place.
   cchar    delimiter  = (strike ? '~' : '*');
   cMD_EDGE behind     = MdEdgeBehind(emitter->line, emitter->lineUsed, delimiter);
   cbool    safe       = !tested || MdFlankingSafe(behind, bytes, byteCount, ahead);
   cbool    strikeHtml = MdStrikeAsHtml(fmt, safe);
   cbool    emphHtml   = MdEmphasisAsHtml(fmt, safe);

   cMD_CONTEXT context = MdSpanContext(fmt, safe);

   if(fmt == IR_FMT_NONE) return MdLineEscaped(emitter, bytes, byteCount, context, dollars);
   if(fmt & IR_FMT_SUPER) {
      if(!MdLineText(emitter, "<sup>")) return false;
   } else if(fmt & IR_FMT_SUB) {
      if(!MdLineText(emitter, "<sub>")) return false;
   }
   if((fmt & IR_FMT_STRIKE) && !MdLineText(emitter, (strikeHtml ? "<del>" : "~~"))) return false;
   if(fmt & IR_FMT_CODE) {
      // A literal backtick inside a code span cannot be escaped, so the delimiter is lengthened past
      // the longest run in the content. A space on each side keeps a leading or trailing backtick from
      // joining the delimiter; CommonMark strips exactly one such pair again when it renders. A code
      // span has no flanking rule of its own, which is why it never needs an HTML form.
      cui64 ticks = MdLongestTickRun(bytes, byteCount) + 1u;
      cbool pad   = (bytes[0] == '`' || bytes[byteCount - 1u] == '`');

      if(!MdLineRun(emitter, '`', ticks)) return false;
      if(pad && !MdLineText(emitter, " ")) return false;
      if(!MdLineEscaped(emitter, bytes, byteCount, context, dollars)) return false;
      if(pad && !MdLineText(emitter, " ")) return false;
      if(!MdLineRun(emitter, '`', ticks)) return false;
   } else {
      cbool bold   = (fmt & IR_FMT_BOLD) != 0;
      cbool italic = (fmt & IR_FMT_ITALIC) != 0;
      // Bold and italic together are one delimiter of three rather than two nested pairs, which mapping
      // row 5 fixes so that no document depends on which of the two its producer named first.
      cchptr markdown = (bold && italic ? "***" : (bold ? "**" : (italic ? "*" : "")));
      cchptr htmlOpen = (bold && italic ? "<strong><em>" : (bold ? "<strong>" : (italic ? "<em>" : "")));
      cchptr htmlShut = (bold && italic ? "</em></strong>" : (bold ? "</strong>" : (italic ? "</em>" : "")));
      cchptr open     = (emphHtml ? htmlOpen : markdown);
      cchptr close    = (emphHtml ? htmlShut : markdown);

      if(!MdLineText(emitter, open)) return false;
      if(!MdLineEscaped(emitter, bytes, byteCount, context, dollars)) return false;
      if(!MdLineText(emitter, close)) return false;
   }
   if((fmt & IR_FMT_STRIKE) && !MdLineText(emitter, (strikeHtml ? "</del>" : "~~"))) return false;
   if(fmt & IR_FMT_SUPER) return MdLineText(emitter, "</sup>");
   if(fmt & IR_FMT_SUB) return MdLineText(emitter, "</sub>");
   return true;
}

//-- Line groups

// Where one line of a block ends: at the next hard break, or at the block's last span.
static cui32 MdLineEnd(cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from) {
   ui32 stop = from;

   while(stop < block->spanCount) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + stop);

      if(!span || span->kind == IR_SPAN_BREAK) break;
      ++stop;
   }
   return stop;
}

// Whether a range of spans holds two or more dollar signs, which is D12's whole rule. The scope is the
// assembled line and not the span, because a line built from "costs $5" and " and $10" holds two even
// though neither run does -- see the dollar note on MdEscapeWrite, which this is the caller of.
// A dollar inside a code span does not count: it cannot be escaped there and CONVERSION_REFERENCE 4.1
// records that it is inert, so counting it would only put a backslash in front of an unrelated one.
static cbool MdDollarPair(cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from, cui32 to) {
   ui64 found = 0;

   for(ui32 index = from; index < to; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || span->kind != IR_SPAN_TEXT || (span->fmt & IR_FMT_CODE)) continue;
      found += MdEscapeCountDollars(IrText(document, span->textAt), span->textBytes);
   }
   return found >= 2u;
}

// The class of the character that will follow a span, which its closing delimiter has to flank against.
// The next span's own first byte of *text*, whatever formatting stands in front of it. Where that
// formatting is a delimiter of the same character the two runs merge, exactly as they do behind, and
// the text really is what follows; where it is anything else the true neighbour is a delimiter or a
// tag, which is punctuation, and reading the text instead can only make the verdict stricter. Nothing
// after it at all is the end of the line, which CommonMark counts as whitespace.
static cMD_EDGE MdEdgeAhead(cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from, cui32 to) {
   for(ui32 index = from; index < to; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || span->kind != IR_SPAN_TEXT || !span->textBytes) continue;
      return MdEdgeAt(IrText(document, span->textAt), span->textBytes);
   }
   return MD_EDGE_SPACE;
}

// Assembles one line out of a range of spans, trimming the padding at both of its ends. A line's own
// leading padding goes because four leading spaces would be an indented code block, and its trailing
// padding because two trailing spaces are Markdown's other spelling of a hard line break.
static cbool MdAssembleLine(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from, cui32 to) {
   cbool dollars = MdDollarPair(document, block, from, to);
   bool  started = false;

   emitter->lineUsed = 0;
   for(ui32 index = from; index < to; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || span->kind != IR_SPAN_TEXT) continue;

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      if(!started) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      if(start >= span->textBytes) continue;

      cMD_EDGE ahead = MdEdgeAhead(document, block, index + 1u, to);

      if(!MdWriteSpan(emitter, bytes + start, span->textBytes - start, span->fmt, dollars, ahead)) return false;
      started = true;
   }
   while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
   return true;
}

// Assembles a heading's whole content as one line. An ATX heading is a single line by construction, so
// a hard break inside one becomes exactly one space rather than continuing anywhere -- which also makes
// the whole block one scope for D12's dollar count, and tests/fixtures/dollars pins that it is.
static cbool MdAssembleHeading(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   cbool dollars = MdDollarPair(document, block, 0, block->spanCount);
   bool  started = false;
   bool  pending = false;

   emitter->lineUsed = 0;
   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span) continue;
      if(span->kind == IR_SPAN_BREAK) {
         if(started) pending = true;
         continue;
      }

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      // A break becomes exactly one space, so the padding on either side of it goes the way a line's own
      // leading and trailing padding does. Two spaces would render as one anyway; what they would really
      // do is put an invisible difference in a file that is compared byte for byte.
      if(!started || pending) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      if(start >= span->textBytes) continue;
      if(pending) {
         while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
         if(!MdLineText(emitter, " ")) return false;
         pending = false;
      }
      // A break inside a heading becomes one space, so the whole block is one line and what stands
      // after a span is simply the next span, wherever the break happened to fall.
      cMD_EDGE ahead = MdEdgeAhead(document, block, index + 1u, block->spanCount);

      if(!MdWriteSpan(emitter, bytes + start, span->textBytes - start, span->fmt, dollars, ahead)) return false;
      started = true;
   }
   while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
   return true;
}

// Writes the hard-break marker that continues a line.
static cbool MdBreakLine(MD_EMITTERptrc emitter) {
   // A trailing backslash survives an editor that strips trailing whitespace; two trailing spaces do
   // not, which is why the backslash is the default and the two-space form is opt-in.
   if(emitter->hardBreak == HARD_BREAK_SPACES) {
      if(!MdAppend(emitter, "  ", 2u)) return false;
   } else if(!MdAppendByte(emitter, '\\')) {
      return false;
   }
   return MdAppendByte(emitter, '\n');
}

//-- Blocks

// Emits a paragraph or a blockquote: one line per range of spans between hard breaks, each carrying the
// block's prefix, and each put through the line-start pass so it cannot open a block it should not.
static cbool MdEmitLines(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block, cchptr prefix) {
   ui32 index = 0;
   bool wrote = false;

   while(index < block->spanCount) {
      cui32 stop = MdLineEnd(document, block, index);

      if(!MdAssembleLine(emitter, document, block, index, stop)) return false;
      // A line that came to nothing is dropped along with the break that would have continued it: an
      // empty Markdown line ends the paragraph, so neither spelling of a hard break can carry one.
      if(emitter->lineUsed) {
         if(wrote && !MdBreakLine(emitter)) return false;
         if(prefix && !MdAppendText(emitter, prefix)) return false;

         csi64 at = MdEscapeLineStartAt(emitter->line, emitter->lineUsed, wrote);

         if(at >= 0 && !MdLineInsert(emitter, ui64(at), '\\')) return false;
         if(!MdAppend(emitter, emitter->line, emitter->lineUsed)) return false;
         wrote = true;
      }
      index = stop;
      while(index < block->spanCount) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

         if(!span || span->kind != IR_SPAN_BREAK) break;
         ++index;
      }
   }
   emitter->lineUsed = 0;
   return MdAppendByte(emitter, '\n');
}

// Emits one heading. Its content is not at the start of a line -- the hashes and their space are -- so
// the line-start rules do not apply to it; the closing-sequence rule takes their place.
static cbool MdEmitHeading(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   ui32 level = (block->headingLevel ? block->headingLevel : 1u);

   if(level > MD_MAX_HEADING) level = MD_MAX_HEADING;
   if(!MdAppendRun(emitter, '#', level)) return false;
   if(!MdAppendByte(emitter, ' ')) return false;

   cui64 contentAt = emitter->used;

   // An ATX heading is one line by construction, so every hard break inside one folds to a single space
   // and the whole block is assembled as one line -- which is also the scope D12's dollar count takes.
   if(!MdAssembleHeading(emitter, document, block)) return false;

   csi64 tail = MdEscapeHeadingTailAt(emitter->line, emitter->lineUsed);

   if(tail >= 0 && !MdLineInsert(emitter, ui64(tail), '\\')) return false;
   if(!MdAppend(emitter, emitter->line, emitter->lineUsed)) return false;
   // Trimming back to the head of the line rather than to the head of the content is deliberate: a
   // heading whose content came to nothing must not be left with the space after its hashes.
   if(!emitter->lineUsed && emitter->used > contentAt - 1u) emitter->used = contentAt - 1u;
   emitter->lineUsed = 0;
   return MdAppendByte(emitter, '\n');
}

// Emits one horizontal rule. The blank lines mapping row 25 asks for on either side are the block
// separator's own doing, which is what keeps a rule from being read as a setext underline for the
// paragraph above it.
static cbool MdEmitRule(MD_EMITTERptrc emitter) { return MdAppendText(emitter, "---\n"); }

//-- Fenced code blocks

// How many bytes of text one block holds, which is what decides whether a code paragraph is a blank
// line rather than a line of code. Whitespace counts: inside a fence it is the indentation.
static cui64 MdBlockBytes(cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   ui64 total = 0;

   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(span && span->kind == IR_SPAN_TEXT) total += span->textBytes;
   }
   return total;
}

// Emits one fenced block out of a run of consecutive code paragraphs, which mapping row 12 merges into
// a single fence. The fence is longer than the longest run of backticks anywhere inside it, because a
// shorter one would be closed by the content; there is no info string, because the language a Word
// document was written about is never recoverable from it.
static cbool MdEmitFence(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cui32 first, cui32 last) {
   ui64 ticks = MD_MIN_FENCE;

   for(ui32 index = first; index < last; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;
      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(!span || span->kind != IR_SPAN_TEXT) continue;

         cui64 run = MdLongestTickRun(IrText(document, span->textAt), span->textBytes) + 1u;

         if(run > ticks) ticks = run;
      }
   }
   if(!MdAppendRun(emitter, '`', ticks)) return false;
   if(!MdAppendByte(emitter, '\n')) return false;
   for(ui32 index = first; index < last; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;
      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(!span) continue;
         // A hard break inside a code paragraph is simply the next line of the code: there is no
         // marker to write, because a fence has no other way to continue.
         if(span->kind == IR_SPAN_BREAK) {
            if(!MdAppendByte(emitter, '\n')) return false;
            continue;
         }
         if(!MdAppendEscaped(emitter, IrText(document, span->textAt), span->textBytes, MD_CONTEXT_CODE_BLOCK)) return false;
      }
      if(!MdAppendByte(emitter, '\n')) return false;
   }
   if(!MdAppendRun(emitter, '`', ticks)) return false;
   return MdAppendByte(emitter, '\n');
}

//-- Separation

// Writes what stands between two blocks: exactly one blank line, with one exception. Two consecutive
// quote paragraphs are one quotation that a producer happened to break in two, and a blank line between
// them would close the blockquote and open a second; a bare ">" keeps them inside one, which is what a
// reader of the .docx sees. That is the only place the one-blank-line rule bends, and it bends towards
// the same shape: the separator is still exactly one line.
static cbool MdSeparate(MD_EMITTERptrc emitter, cIR_BLOCK_KIND previous, cIR_BLOCK_KIND next) {
   if(previous == IR_BLOCK_QUOTE && next == IR_BLOCK_QUOTE) {
      if(!MdAppendText(emitter, MD_QUOTE_JOIN)) return false;
   }
   return MdAppendByte(emitter, '\n');
}

//== Entry points

void MdOpen(MD_EMITTERptrc emitter, cHARD_BREAK hardBreak) {
   mzero(emitter, sizeof(MD_EMITTER));
   emitter->hardBreak = hardBreak;
}

void MdClose(MD_EMITTERptrc emitter) {
   cHARD_BREAK hardBreak = emitter->hardBreak;

   mdealloc(emitter->out);
   mdealloc(emitter->line);
   MdOpen(emitter, hardBreak);
}

cMD_RESULT MdEmitDocument(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document) {
   cui32         blocks   = IrBlockCount(document);
   ui32          index    = 0;
   IR_BLOCK_KIND previous = IR_BLOCK_PARAGRAPH;
   bool          wrote    = (emitter->used != 0);

   while(index < blocks) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) {
         ++index;
         continue;
      }
      if(block->kind == IR_BLOCK_CODE) {
         ui32 last = index;

         while(last < blocks) {
            cIR_BLOCKptr next = IrBlockAt(document, last);

            if(!next || next->kind != IR_BLOCK_CODE) break;
            ++last;
         }

         ui32 from = index;
         ui32 to   = last;

         // A blank code paragraph is a blank line of code, which is worth keeping inside the fence and
         // is nothing at all at either end of it. Trimming here rather than after the separator is
         // written is what keeps a run of blank ones from leaving a stray blank line behind.
         while(from < to && !MdBlockBytes(document, IrBlockAt(document, from))) ++from;
         while(to > from && !MdBlockBytes(document, IrBlockAt(document, to - 1u))) --to;
         if(from < to) {
            if(wrote && !MdSeparate(emitter, previous, IR_BLOCK_CODE)) return MD_ERROR_MEMORY;
            if(!MdEmitFence(emitter, document, from, to)) return MD_ERROR_MEMORY;
            previous = IR_BLOCK_CODE;
            wrote    = true;
         }
         index = last;
         continue;
      }
      // Each block writes its own closing newline, so the separator is one more of them and the last
      // block leaves the file ending in a single newline. No block but a trimmed-away run of code can
      // come to nothing here -- IrEndBlock drops one that holds no printable byte -- so the separator
      // can be written before the block rather than unwound again afterwards.
      if(wrote && !MdSeparate(emitter, previous, block->kind)) return MD_ERROR_MEMORY;
      if(block->kind == IR_BLOCK_HEADING) {
         if(!MdEmitHeading(emitter, document, block)) return MD_ERROR_MEMORY;
      } else if(block->kind == IR_BLOCK_RULE) {
         if(!MdEmitRule(emitter)) return MD_ERROR_MEMORY;
      } else if(block->kind == IR_BLOCK_QUOTE) {
         if(!MdEmitLines(emitter, document, block, MD_QUOTE_PREFIX)) return MD_ERROR_MEMORY;
      } else if(!MdEmitLines(emitter, document, block, nullptr)) {
         return MD_ERROR_MEMORY;
      }
      previous = block->kind;
      wrote    = true;
      ++index;
   }
   return (emitter->failed ? MD_ERROR_MEMORY : MD_OK);
}

cchptr MdBytes(cMD_EMITTERptr emitter) { return (emitter->out ? emitter->out : ""); }

cui64 MdByteCount(cMD_EMITTERptr emitter) { return emitter->used; }
