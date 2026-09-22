/*
 * File: MdEmitter.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-22
 * Description: Line assembly, inline delimiters, the blank-line discipline and every block kind's shape.
 * To Do: 1) Emit a fenced block inside a *quote*, which no block kind can express today: a paragraph
 *           is a quotation or a fence and never both, so only a list item reaches a prefixed fence.
 *        2) Size the buffer from the part's byte count rather than growing from a fixed first block.
 *        3) Show a cell's nested list structure, which the pipe form flattens because GFM has no
 *           spelling for indentation inside a cell.
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

// How many list levels there are. ISO/IEC 29500-1 fixes w:ilvl at 0 to 8, which is nine.
constexpr cui32 MD_MAX_LIST_LEVELS = 9u;

// The widest one marker is: nine digits, a dot and the single space after it are eleven bytes, and the
// twelfth is the terminator, because a marker is measured with MdListMarker and then copied as a string.
// CommonMark accepts a start of at most nine digits, and a longer run of them is not a list marker at
// all -- the line would stop being a list rather than merely look wrong.
constexpr cui64 MD_MAX_MARKER = 12u;
constexpr cui32 MD_MAX_NUMBER = 999999999u;

// What kind of Markdown list a reader still has open at one depth. It is not the same question as which
// depths the ilvl stack has open: a rendered list closes only when something is emitted *above* it, so
// two items of two different lists can meet at one depth with a nested item written between them.
constexpr cui8 MD_LIST_SHUT    = 0u; // Nothing open at this depth
constexpr cui8 MD_LIST_BULLETS = 1u; // A bullet list is open
constexpr cui8 MD_LIST_NUMBERS = 2u; // An ordered list is open, and its start is the number it began at

// The widest prefix a line may carry: nine levels of the widest marker, plus a quote marker the list
// may sit inside, plus room to spare. A document nested past this is clamped rather than refused --
// losing a document's text over its indentation would be a poor trade.
constexpr cui64 MD_MAX_PREFIX = 160u;

// What a cell's paragraphs are joined by, and what a hard break inside one becomes. A pipe table's row
// is one line by construction, so every break in it has to be the element (mapping row 26).
static constexpr cchptr MD_CELL_BREAK = "<br>";

// The shortest run of hyphens a GFM delimiter row's cell may hold. Three is not required -- one would
// do -- but three is what every producer writes and what a reader expects to see.
constexpr cui64 MD_DELIMITER_DASHES = 3u;

// The separator that keeps two adjacent lists from becoming one. Mapping row 17 names it, and it needs
// no blank line on either side: an HTML block start line is not paragraph-continuation text, so it
// closes the list above it where it stands and leaves both lists tight.
static constexpr cchptr MD_LIST_SPLIT = "<!-- -->";

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
static cbool MdAppendEscaped(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cMD_CONTEXT context, cbool pipes) {
   cui64 wanted = MdEscapeMeasure(bytes, byteCount, context, false, pipes);

   if(!wanted) return true;
   if(!MdGrow(emitter, &emitter->out, &emitter->capacity, emitter->used + wanted)) return false;
   emitter->used += MdEscapeWrite(emitter->out + emitter->used, wanted, bytes, byteCount, context, false, pipes);
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
static cbool MdLineEscaped(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cMD_CONTEXT context, cbool dollars, cbool pipes) {
   cui64 wanted = MdEscapeMeasure(bytes, byteCount, context, dollars, pipes);

   if(!wanted) return true;
   if(!MdGrow(emitter, &emitter->line, &emitter->lineCapacity, emitter->lineUsed + wanted)) return false;
   emitter->lineUsed += MdEscapeWrite(emitter->line + emitter->lineUsed, wanted, bytes, byteCount, context, dollars, pipes);
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

// The longest run of backticks in a range, which is what a code delimiter has to be longer than. The
// run may begin before the range and continue past it, so it carries in and out: a caller measuring one
// span at a time would otherwise see two short runs where the reader sees one long one.
static cui64 MdLongestTickRun(cchptr bytes, cui64 byteCount, cui64 carryIn, ui64ptrc carryOut) {
   ui64 longest = carryIn;
   ui64 run     = carryIn;

   for(ui64 index = 0; index < byteCount; ++index) {
      run = (bytes[index] == '`' ? run + 1u : 0);
      if(run > longest) longest = run;
   }
   if(carryOut) *carryOut = run;
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

// Whether a code point is one CommonMark counts as whitespace for flanking: the Unicode Zs category,
// plus the tab. It is spelled out rather than looked up in MD_PUNCTUATION because Zs is neither P nor S,
// so a space left in the word class would make a delimiter beside it look like one beside a letter --
// and Zs is seventeen code points in seven contiguous groups, which with the tab is smaller than a table.
// U+200B is deliberately absent: it is Cf, not Zs, and CommonMark does not count it.
static cbool MdIsSpacePoint(cui32 point) {
   if(point == ' ' || point == '\t' || point == 0x00A0u) return true;
   if(point == 0x1680u || point == 0x202Fu || point == 0x205Fu || point == 0x3000u) return true;
   return point >= 0x2000u && point <= 0x200Au;
}

// Which class one code point falls into, for the flanking rules.
static cMD_EDGE MdEdgeOfPoint(cui32 point) {
   if(MdIsSpacePoint(point)) return MD_EDGE_SPACE;

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
//
// Text between a link's brackets takes MD_CONTEXT_LINK_TEXT, which is the inline set and one more
// obligation -- the closing bracket may not appear unescaped -- and the inline set already escapes
// that bracket unconditionally, so the two produce identical bytes today. Naming it anyway is the
// point: M7 is where the context gets its first caller, and a later change to the link rules must
// reach the text inside a link without having to find every place that meant it.
static cMD_CONTEXT MdSpanContext(cui32 fmt, cbool safe, cbool inLink) {
   if(fmt & IR_FMT_CODE) return MD_CONTEXT_CODE_SPAN;
   if(fmt & (IR_FMT_SUPER | IR_FMT_SUB)) return MD_CONTEXT_HTML;
   if(MdStrikeAsHtml(fmt, safe) || MdEmphasisAsHtml(fmt, safe)) return MD_CONTEXT_HTML;
   return (inLink ? MD_CONTEXT_LINK_TEXT : MD_CONTEXT_INLINE);
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
static cbool MdWriteSpan(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cui32 fmt,          // The span itself
                         cbool dollars, cMD_EDGE ahead, cui32 nextFmt, cbool inLink, cbool pipes) { // What stands around it
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
   cchar    delimiter = (strike ? '~' : '*');
   cMD_EDGE behind    = MdEdgeBehind(emitter->line, emitter->lineUsed, delimiter);
   // CommonMark reads adjacent runs of the same delimiter character as one run and then matches openers
   // to closers by length -- its rule of three -- so three emphasis spans meeting with no text between
   // them can leave a run that no pairing resolves: "**bo*****th****ree*" comes out as six literal
   // asterisks in the reader's text, with the middle span lost outright. The flanking test models that
   // merge for the character classes
   // but not for the length arithmetic, which no character class can express. A span abutted by an
   // identical run on *both* sides is therefore written as an element instead: HTML has neither a run
   // length nor a flanking rule, and an element between two Markdown runs also keeps those two apart.
   cbool asterisk   = !strike && emphasis;
   cbool runAhead   = (nextFmt & (IR_FMT_BOLD | IR_FMT_ITALIC)) != 0 && (nextFmt & (IR_FMT_STRIKE | IR_FMT_SUPER | IR_FMT_SUB | IR_FMT_CODE)) == 0;
   cbool abutted    = asterisk && emitter->lineUsed && emitter->line[emitter->lineUsed - 1u] == '*' && runAhead;
   cbool safe       = (!tested || MdFlankingSafe(behind, bytes, byteCount, ahead)) && !abutted;
   cbool strikeHtml = MdStrikeAsHtml(fmt, safe);
   cbool emphHtml   = MdEmphasisAsHtml(fmt, safe);

   cMD_CONTEXT context = MdSpanContext(fmt, safe, inLink);

   if(fmt == IR_FMT_NONE) return MdLineEscaped(emitter, bytes, byteCount, context, dollars, pipes);
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
      cui64 ticks = MdLongestTickRun(bytes, byteCount, 0, nullptr) + 1u;
      cbool pad   = (bytes[0] == '`' || bytes[byteCount - 1u] == '`');

      if(!MdLineRun(emitter, '`', ticks)) return false;
      if(pad && !MdLineText(emitter, " ")) return false;
      if(!MdLineEscaped(emitter, bytes, byteCount, context, dollars, pipes)) return false;
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
      if(!MdLineEscaped(emitter, bytes, byteCount, context, dollars, pipes)) return false;
      if(!MdLineText(emitter, close)) return false;
   }
   if((fmt & IR_FMT_STRIKE) && !MdLineText(emitter, (strikeHtml ? "</del>" : "~~"))) return false;
   if(fmt & IR_FMT_SUPER) return MdLineText(emitter, "</sup>");
   if(fmt & IR_FMT_SUB) return MdLineText(emitter, "</sub>");
   return true;
}

//-- Links, images and anchors

// What an open link carries while the line it began on is still being assembled. A link may run over
// a hard break, and Markdown has no way to spell one that does, so the emitter closes it at the end
// of the line and opens it again on the next -- two links to one destination, which is what the
// document showed and what a reader can click.
struct MD_LINK {
   ui64 contentAt; ///< Where the content begins in the line buffer, one past the '['
   ui32 destAt;    ///< Destination-arena offset of the link's destination
   ui32 destBytes; ///< How many bytes it is
   bool open;      ///< Whether a '[' has been written and not yet closed
};

typedef MD_LINK *const MD_LINKptrc;

// Writes the closing half of a link: the bracket, and the destination in parentheses.
//
// The destination is percent-encoded rather than backslash-escaped, which is MD_CONTEXT_LINK_DEST and
// CONVERSION_REFERENCE 4.1's rule for it: a backslash inside a destination is a literal byte of the
// URL, so the only escape a destination has is the one the URL syntax already provides.
static cbool MdCloseLink(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, MD_LINKptrc link, cbool pipes) {
   if(!link->open) return true;
   link->open = false;
   // Nothing was written between the brackets. LinkResolve mutes a link whose content is empty, but a
   // hard break at the very edge of one leaves that content on the *other* line, and "[](url)" is a
   // link a reader cannot see and cannot click. Unwind the bracket instead of closing it.
   if(emitter->lineUsed <= link->contentAt) {
      emitter->lineUsed = (link->contentAt ? link->contentAt - 1u : 0);
      return true;
   }
   if(!MdLineText(emitter, "](")) return false;
   if(!MdLineEscaped(emitter, IrDest(document, link->destAt), link->destBytes, MD_CONTEXT_LINK_DEST, false, pipes)) return false;
   return MdLineText(emitter, ")");
}

// Writes one of M7's marker spans, and reports whether the buffer survived.
//
// An image is one piece: the exclamation mark, the alt text between brackets and the source between
// parentheses. An anchor is the raw HTML element mapping row 22 asks for where a bookmark does not
// sit at a heading; its name has already been sanitised to the bytes an attribute and a fragment can
// both carry, so there is nothing left here to escape.
static cbool MdWriteMarker(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_SPANptr span, MD_LINKptrc link, cbool dollars, cbool pipes) {
   if(span->kind == IR_SPAN_LINK_START) {
      if(!MdCloseLink(emitter, document, link, pipes)) return false;
      // An exclamation mark immediately in front of a link's '[' makes the pair an *image* marker, so
      // "see this!" followed by a link renders as a broken picture and the link text disappears. This
      // is CONVERSION_REFERENCE 4.2's pitfall 7, and MdEscape leaves it here on purpose: the mark is
      // only dangerous next to a bracket the emitter itself writes, which is knowledge a run does not
      // have. A '\' before the mark cannot be an escape of it -- nothing escapes an exclamation mark
      // into this buffer -- so the last byte being '!' is the whole test.
      if(emitter->lineUsed && emitter->line[emitter->lineUsed - 1u] == '!') {
         emitter->lineUsed -= 1u;
         if(!MdLineText(emitter, "\\!")) return false;
      }
      if(!MdLineText(emitter, "[")) return false;
      link->contentAt = emitter->lineUsed;
      link->destAt    = span->destAt;
      link->destBytes = span->destBytes;
      link->open      = true;
      return true;
   }
   if(span->kind == IR_SPAN_LINK_END) return MdCloseLink(emitter, document, link, pipes);
   if(span->kind == IR_SPAN_ANCHOR) {
      if(!MdLineText(emitter, "<a id=\"")) return false;
      if(!MdLineAppend(emitter, IrDest(document, span->destAt), span->destBytes)) return false;
      return MdLineText(emitter, "\"></a>");
   }
   if(!MdLineText(emitter, "![")) return false;
   if(!MdLineEscaped(emitter, IrText(document, span->textAt), span->textBytes, MD_CONTEXT_ALT_TEXT, dollars, pipes)) return false;
   if(!MdLineText(emitter, "](")) return false;
   if(!MdLineEscaped(emitter, IrDest(document, span->destAt), span->destBytes, MD_CONTEXT_LINK_DEST, false, pipes)) return false;
   return MdLineText(emitter, ")");
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

// Whether a span writes nothing at all, so that a lookahead must read straight past it.
//
// A muted span is one LinkResolve settled: a link with no destination or no content, an anchor
// nothing points at. An empty text span is a run that carried properties and no text.
static cbool MdSpanIsSilent(cIR_SPANptr span) {
   if(span->flags & IR_SPAN_FLAG_MUTE) return true;
   return span->kind == IR_SPAN_TEXT && !span->textBytes;
}

// The class of the character that will follow a span, which its closing delimiter has to flank against.
//
// Where the next span is text, its own first byte, whatever formatting stands in front of it: where
// that formatting is a delimiter of the same character the two runs merge, exactly as they do behind,
// and the text really is what follows; where it is anything else the true neighbour is a delimiter or
// a tag, which is punctuation, and reading the text instead can only make the verdict stricter.
// Where the next span is one of M7's markers the answer is not a guess at all: a link start writes
// '[', a link end ']', an image '!' and an anchor '<', and every one of those is punctuation. That is
// why this had to be re-cut at M7 -- before it, every neighbour was text, and reading past a bracket
// to the text behind it would report a letter where a bracket stands.
// Nothing after it at all is the end of the line, which CommonMark counts as whitespace.
static cMD_EDGE MdEdgeAhead(cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from, cui32 to) {
   for(ui32 index = from; index < to; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || MdSpanIsSilent(span)) continue;
      if(span->kind != IR_SPAN_TEXT) return MD_EDGE_PUNCT;
      return MdEdgeAt(IrText(document, span->textAt), span->textBytes);
   }
   return MD_EDGE_SPACE;
}

// The formatting the next text span carries, which is what says whether it will open with an asterisk.
// Markup between the two ends the question: a delimiter run cannot merge with one on the far side of a
// bracket, so anything but text ahead is reported as no formatting at all.
static cui32 MdFormatAhead(cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from, cui32 to) {
   for(ui32 index = from; index < to; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || MdSpanIsSilent(span)) continue;
      if(span->kind != IR_SPAN_TEXT) return IR_FMT_NONE;
      return span->fmt;
   }
   return IR_FMT_NONE;
}

// Assembles one line out of a range of spans, trimming the padding at both of its ends. A line's own
// leading padding goes because four leading spaces would be an indented code block, and its trailing
// padding because two trailing spaces are Markdown's other spelling of a hard line break.
static cbool MdAssembleLine(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block, cui32 from, cui32 to, MD_LINKptrc link) {
   cbool dollars = MdDollarPair(document, block, from, to);
   bool  started = false;

   emitter->lineUsed = 0;
   // A link the previous line left open is opened again here, so that a hyperlink broken by a hard
   // break reaches the reader as two clickable halves rather than as one bracket with no partner.
   if(link->open) {
      if(!MdLineText(emitter, "[")) return false;
      link->contentAt = emitter->lineUsed;
      started         = true;
   }
   for(ui32 index = from; index < to; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || MdSpanIsSilent(span)) continue;
      if(span->kind != IR_SPAN_TEXT) {
         if(span->kind == IR_SPAN_BREAK) continue;
         if(!MdWriteMarker(emitter, document, span, link, dollars, false)) return false;
         started = true;
         continue;
      }

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      if(!started) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      if(start >= span->textBytes) continue;

      cMD_EDGE ahead   = MdEdgeAhead(document, block, index + 1u, to);
      cui32    nextFmt = MdFormatAhead(document, block, index + 1u, to);

      if(!MdWriteSpan(emitter, bytes + start, span->textBytes - start, span->fmt, dollars, ahead, nextFmt, link->open, false)) return false;
      started = true;
   }
   // The line ends, so anything still open has to be closed on it. The trailing padding goes after
   // that, because a destination never ends in one and closing first would leave the brackets behind
   // a space the trim would then have to reach past.
   cbool carried = link->open;

   if(!MdCloseLink(emitter, document, link, false)) return false;
   link->open = carried;
   while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
   return true;
}

// Assembles a heading's whole content as one line. An ATX heading is a single line by construction, so
// a hard break inside one becomes exactly one space rather than continuing anywhere -- which also makes
// the whole block one scope for D12's dollar count, and tests/fixtures/dollars pins that it is.
static cbool MdAssembleHeading(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   cbool   dollars = MdDollarPair(document, block, 0, block->spanCount);
   MD_LINK link    = {0, 0, 0, false};
   bool    started = false;
   bool    pending = false;

   emitter->lineUsed = 0;
   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || MdSpanIsSilent(span)) continue;
      if(span->kind == IR_SPAN_BREAK) {
         if(started) pending = true;
         continue;
      }
      if(span->kind != IR_SPAN_TEXT) {
         // A heading is one line by construction, so a break inside it never separates a link from
         // its own end: the marker is written where it stands and the space stands in for the break.
         if(pending) {
            while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
            if(!MdLineText(emitter, " ")) return false;
            pending = false;
         }
         if(!MdWriteMarker(emitter, document, span, &link, dollars, false)) return false;
         started = true;
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
      cMD_EDGE ahead   = MdEdgeAhead(document, block, index + 1u, block->spanCount);
      cui32    nextFmt = MdFormatAhead(document, block, index + 1u, block->spanCount);

      if(!MdWriteSpan(emitter, bytes + start, span->textBytes - start, span->fmt, dollars, ahead, nextFmt, link.open, false)) return false;
      started = true;
   }
   if(!MdCloseLink(emitter, document, &link, false)) return false;
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

//-- Prefixes

// What stands in front of each line of one block. The two forms are the whole reason a single string
// could not carry this any longer: a quote writes "> " on every line of its block, while a list item
// writes its marker on the first line and the same width in spaces on every line after it -- and that
// width is the content column CommonMark measures a continuation line, and a nested item's own marker,
// against. The quote was the degenerate case where the two happened to be equal.
struct MD_PREFIX {
   char first[MD_MAX_PREFIX]; ///< What the block's first line takes
   char cont[MD_MAX_PREFIX];  ///< What every line after it takes, and every line of a block nested in it
   ui64 firstUsed;            ///< Bytes of first
   ui64 contUsed;             ///< Bytes of cont
};

typedef MD_PREFIX *const       MD_PREFIXptrc;
typedef const MD_PREFIX *const cMD_PREFIXptrc;

// Where the content of the open item at each list level starts. It is maintained across a whole run of
// items rather than computed per block, because a child's indentation is a fact about the marker its
// parent actually wrote: "10. " is four columns and "9. " is three, so two siblings of one list have
// children indented differently, and a child indented to the narrower of the two is not a child at all.
struct MD_LIST {
   ui32 column[MD_MAX_LIST_LEVELS]; ///< Where the content of the item open at each depth starts
   ui8  level[MD_MAX_LIST_LEVELS];  ///< Which w:ilvl opened each of those depths
   ui8  open[MD_MAX_LIST_LEVELS];   ///< Which kind of Markdown list a reader still has open at each depth
   ui32 depth;                      ///< How many depths are open
};

typedef const MD_LIST *cMD_LISTptr;
typedef MD_LIST *const MD_LISTptrc;

// Empties a prefix.
static void MdPrefixClear(MD_PREFIXptrc prefix) {
   prefix->firstUsed = 0;
   prefix->contUsed  = 0;
}

// Appends bytes that stand on every line alike: an enclosing item's indentation, or a quote's marker.
// A contribution that would not fit is dropped whole rather than truncated, so a line never carries
// half a marker; the ceiling is far past any document, and losing a document's text over its own
// indentation would be the worse trade.
static void MdPrefixSame(MD_PREFIXptrc prefix, cchptr text, cui64 byteCount) {
   if(prefix->firstUsed + byteCount > MD_MAX_PREFIX || prefix->contUsed + byteCount > MD_MAX_PREFIX) return;
   for(ui64 index = 0; index < byteCount; ++index) {
      prefix->first[prefix->firstUsed + index] = text[index];
      prefix->cont[prefix->contUsed + index]   = text[index];
   }
   prefix->firstUsed += byteCount;
   prefix->contUsed += byteCount;
}

// Appends one item's marker: the marker itself on the block's first line, and that many spaces on every
// line after it, which is the item's content column.
static void MdPrefixMarker(MD_PREFIXptrc prefix, cchptr marker, cui64 byteCount) {
   if(prefix->firstUsed + byteCount > MD_MAX_PREFIX || prefix->contUsed + byteCount > MD_MAX_PREFIX) return;
   for(ui64 index = 0; index < byteCount; ++index) {
      prefix->first[prefix->firstUsed + index] = marker[index];
      prefix->cont[prefix->contUsed + index]   = ' ';
   }
   prefix->firstUsed += byteCount;
   prefix->contUsed += byteCount;
}

// Appends a run of spaces to both forms.
static void MdPrefixIndent(MD_PREFIXptrc prefix, cui32 columns) {
   for(ui32 index = 0; index < columns; ++index) MdPrefixSame(prefix, " ", 1u);
}

// Writes the prefix in force in front of one line.
static cbool MdWritePrefix(MD_EMITTERptrc emitter, cMD_PREFIXptrc prefix, cbool first) {
   cchptr bytes = (first ? prefix->first : prefix->cont);
   cui64  used  = (first ? prefix->firstUsed : prefix->contUsed);

   return (used ? MdAppend(emitter, bytes, used) : true);
}

// Writes what a block with no content at all leaves behind: its marker, with the padding after it
// trimmed off. "- " with nothing following it would put a space before a newline, which is Markdown's
// other spelling of a hard break and which this emitter's own invariants forbid.
static cbool MdWriteBareMarker(MD_EMITTERptrc emitter, cMD_PREFIXptrc prefix) {
   ui64 used = prefix->firstUsed;

   while(used && MdIsPad(prefix->first[used - 1u])) --used;
   return (used ? MdAppend(emitter, prefix->first, used) : true);
}

//-- Blocks

// Emits a paragraph, a blockquote or a list item: one line per range of spans between hard breaks, each
// carrying the block's prefix, and each put through the line-start pass so it cannot open a block it
// should not.
//
// The order -- break, then prefix, then content -- is what puts a continuation line at the item's own
// content column, and the line-start pass still runs over emitter->line, which holds the content alone.
// That separation is load-bearing: a marker written into the line buffer would be escaped into "\\-" by
// the very pass that exists to stop a *run's* text opening a list.
static cbool MdEmitLines(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block, cMD_PREFIXptrc prefix) {
   MD_LINK link  = {0, 0, 0, false};
   ui32    index = 0;
   bool    wrote = false;

   while(index < block->spanCount) {
      cui32 stop = MdLineEnd(document, block, index);

      if(!MdAssembleLine(emitter, document, block, index, stop, &link)) return false;
      // A line that came to nothing is dropped along with the break that would have continued it: an
      // empty Markdown line ends the paragraph, so neither spelling of a hard break can carry one.
      if(emitter->lineUsed) {
         if(wrote && !MdBreakLine(emitter)) return false;
         if(!MdWritePrefix(emitter, prefix, !wrote)) return false;

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
   // A block that produced no line at all is an empty list item, which IrEndBlock keeps on purpose: a
   // marker alone on its line is what the document showed and what CommonMark spells. Nothing else can
   // reach here, because every other empty block was unwound before the emitter ever saw it.
   if(!wrote && prefix->firstUsed && !MdWriteBareMarker(emitter, prefix)) return false;
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

// Whether one block holds a byte worth putting on a line of its own, which is what decides that a code
// paragraph is a blank line rather than a line of code. The test is the one IrEndBlock uses on every
// other kind, so a paragraph of nothing but spaces counts as blank here too -- at the edge of a fence
// that is a line of invisible padding, and trimming it is what keeps the fence opening on real code.
// Inside the fence a blank line stays verbatim, because there a blank line is content.
static cbool MdBlockHasContent(cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || span->kind != IR_SPAN_TEXT) continue;

      cchptr bytes = IrText(document, span->textAt);

      for(ui32 at = 0; at < span->textBytes; ++at) {
         cchar byte = bytes[at];

         if(byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') return true;
      }
   }
   return false;
}

// Emits one fenced block out of a run of consecutive code paragraphs, which mapping row 12 merges into
// a single fence. The fence is longer than the longest run of backticks anywhere inside it, because a
// shorter one would be closed by the content; there is no info string, because the language a Word
// document was written about is never recoverable from it.
static cbool MdEmitFence(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cui32 first, cui32 last, cMD_PREFIXptrc prefix) {
   ui64 ticks = MD_MIN_FENCE;

   for(ui32 index = first; index < last; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);
      ui64         open  = 0; // Backticks the previous span ended on, which the next one continues

      if(!block) continue;
      // The run is counted across the block's spans and not within each: a code block's spans need not
      // carry equal formatting, so RunCoalescer leaves a bold "``" beside a plain "`" as two spans, and
      // measuring them apart would size the fence at three -- which the content's own three would close.
      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(!span) continue;
         if(span->kind != IR_SPAN_TEXT) {
            open = 0; // A break starts a new line, and a backtick run does not cross one
            continue;
         }

         cchptr bytes = IrText(document, span->textAt);
         cui64  run   = MdLongestTickRun(bytes, span->textBytes, open, &open) + 1u;

         if(run > ticks) ticks = run;
      }
   }
   // Every line the fence writes takes the prefix in force, so a fence inside a list item sits at that
   // item's content column. Writing it there rather than one column in also keeps the relative fence
   // indentation at zero: CommonMark strips from each content line as many columns as the opening fence
   // was indented by, and the code's own leading whitespace is exactly what that would eat.
   //
   // A line that came to nothing has its prefix rolled back off again rather than being left as trailing
   // whitespace, which is what keeps a blank line of code blank.
   if(!MdWritePrefix(emitter, prefix, true)) return false;
   if(!MdAppendRun(emitter, '`', ticks)) return false;
   if(!MdAppendByte(emitter, '\n')) return false;
   for(ui32 index = first; index < last; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;

      ui64 lineAt = emitter->used;

      if(!MdWritePrefix(emitter, prefix, false)) return false;

      ui64 bodyAt = emitter->used;

      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(!span) continue;
         // A hard break inside a code paragraph is simply the next line of the code: there is no
         // marker to write, because a fence has no other way to continue.
         if(span->kind == IR_SPAN_BREAK) {
            if(emitter->used == bodyAt) emitter->used = lineAt;
            if(!MdAppendByte(emitter, '\n')) return false;
            lineAt = emitter->used;
            if(!MdWritePrefix(emitter, prefix, false)) return false;
            bodyAt = emitter->used;
            continue;
         }
         // Nothing but text reaches a fence. A link inside one has no brackets to write -- the
         // content is literal -- and a picture has nothing a fence could show, so both are dropped
         // and only the text a code paragraph is made of is written out.
         if(span->kind != IR_SPAN_TEXT) continue;
         if(!MdAppendEscaped(emitter, IrText(document, span->textAt), span->textBytes, MD_CONTEXT_CODE_BLOCK, false)) return false;
      }
      if(emitter->used == bodyAt) emitter->used = lineAt;
      if(!MdAppendByte(emitter, '\n')) return false;
   }
   if(!MdWritePrefix(emitter, prefix, false)) return false;
   if(!MdAppendRun(emitter, '`', ticks)) return false;
   return MdAppendByte(emitter, '\n');
}

//-- Lists

// Builds one item's marker: "- " for a bullet, or its number, a dot and one space for an ordered item.
//
// Exactly one space follows it, and that is load-bearing rather than a matter of taste. CommonMark fixes
// an item's content column from its own first line, and with one space all three of its list-item rules
// -- the ordinary case, an item whose content begins with indented code, and an item that begins with a
// blank line -- give the same column. One formula therefore covers every item this emitter can write,
// the empty one included; two spaces would need a second.
//
// The number is capped at nine digits because a longer run of them is not a list marker at all -- the
// line would silently stop being a list. NumAssignMarkers caps it too; this is the emitter refusing to
// depend on a pass that runs before it.
static cui64 MdListMarker(cIR_BLOCKptr block, chptrc dest) {
   ui64 used = 0;

   if(!(block->listFlags & IR_LIST_ORDERED)) {
      dest[used++] = '-';
      dest[used++] = ' ';
      return used;
   }

   char digits[MD_MAX_MARKER];
   ui64 count  = 0;
   ui32 number = (block->listNumber > MD_MAX_NUMBER ? MD_MAX_NUMBER : block->listNumber);

   do {
      digits[count++] = char('0' + (number % 10u));
      number /= 10u;
   } while(number && count + 2u < MD_MAX_MARKER);
   while(count) dest[used++] = digits[--count];
   dest[used++] = '.';
   dest[used++] = ' ';
   return used;
}

// Whether a block would put anything on the page. It is IrEndBlock's own test and not the narrower one
// MdBlockHasContent applies to a fence: an item holding nothing but a picture, or nothing but a bookmark
// something still points at, emits bytes and must not be trimmed off the edge of a list as empty.
static cbool MdItemHasContent(cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   if(!block) return false;
   return IrHasContent(document, block->spanAt, block->spanAt + block->spanCount);
}

// Emits one run of consecutive list items.
//
// A run is emitted as a group for the same reason a run of code paragraphs is: nothing at all stands
// between two items of one list, and the block separator's contract is that it writes exactly one line.
// Keeping the grouping here leaves that contract intact instead of teaching the separator to write
// nothing, and it is what makes a list tight.
// How far the continuation lines of a marker-less item at one level would be indented, worked out
// without disturbing the stack. It is the lookahead a fence needs: mapping row 12 merges consecutive
// all-monospace paragraphs into one fence, and what says two of them belong to one item is that their
// lines land in one column -- which is a fact about the markers above them and not about their levels.
static cui32 MdPlainIndent(cMD_LISTptr list, cui8 level) {
   cui32 wanted = (level < MD_MAX_LIST_LEVELS ? level : MD_MAX_LIST_LEVELS - 1u);
   ui32  depth  = list->depth;

   while(depth && list->level[depth - 1u] > wanted) --depth;

   cbool reopened = (depth && list->level[depth - 1u] == wanted);
   cui32 opened   = (reopened ? depth - 1u : depth);
   cui32 at       = (opened < MD_MAX_LIST_LEVELS ? opened : MD_MAX_LIST_LEVELS - 1u);

   return (reopened ? list->column[at] : (at ? list->column[at - 1u] : 0u));
}

static cbool MdEmitList(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cui32 first, cui32 last, MD_LISTptrc list, MD_PREFIXptrc prefix) {
   ui32         written = 0;       // The depth the item before this one was emitted at
   cIR_BLOCKptr emitted = nullptr; // The last block that actually put a line out, which is not the block
                                   // before this one: a content-free continuation is skipped whole, and
                                   // asking the *previous block* whether it wrote anything then reads a
                                   // block that was never emitted and suppresses a blank line the one
                                   // before it had earned.

   list->depth = 0;
   for(ui32 level = 0; level < MD_MAX_LIST_LEVELS; ++level) list->open[level] = MD_LIST_SHUT;
   for(ui32 index = first; index < last; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;

      // A marker-less continuation with nothing in it is nothing at all. It has no marker to stand for
      // it the way an empty *marked* item does, so its line lands on top of the blank line the
      // cannot-interrupt-a-paragraph rule below has already written and the pair reads as two. A code
      // paragraph is the one exception, because an empty one is a blank line of its own fence.
      cbool hollow = (block->listFlags & IR_LIST_PLAIN) != 0 && block->kind != IR_BLOCK_CODE;

      if(hollow && !MdItemHasContent(document, block)) continue;

      char  marker[MD_MAX_MARKER];
      cbool plain  = (block->listFlags & IR_LIST_PLAIN) != 0;
      cui32 wanted = (block->listLevel < MD_MAX_LIST_LEVELS ? block->listLevel : MD_MAX_LIST_LEVELS - 1u);

      // A w:ilvl is mapped onto an emitted depth through the stack of levels still open, rather than
      // used as one. Two things force it. A level the document skipped over would otherwise put a marker
      // four columns past its parent's content column, and four columns past it is an indented code
      // block -- the list would not merely look wrong, it would stop being a list; CONVERSION_REFERENCE
      // 5.4 allows a skip to be normalised to one Markdown level per step, and this is that. And a run
      // of items that *begins* at a deep w:ilvl has no parent to be indented under at all, so using the
      // level directly would emit a shallower item further in than the deeper one above it and invert
      // the document's own nesting.
      while(list->depth && list->level[list->depth - 1u] > wanted) --list->depth;

      cbool reopened = (list->depth && list->level[list->depth - 1u] == wanted);
      cui32 opened   = (reopened ? list->depth - 1u : list->depth);
      // The stack's levels strictly increase, so nine of them is every depth there can be; the clamp is
      // a bound the type does not carry rather than a case that arises.
      cui32 depth        = (opened < MD_MAX_LIST_LEVELS ? opened : MD_MAX_LIST_LEVELS - 1u);
      cui32 markerColumn = (depth ? list->column[depth - 1u] : 0u);

      MdPrefixClear(prefix);
      if(plain) {
         // Mapping row 16: no marker at all, indented to the content column of the item it continues.
         MdPrefixIndent(prefix, (reopened ? list->column[depth] : markerColumn));
      } else {
         cui64 used = MdListMarker(block, marker);

         MdPrefixIndent(prefix, markerColumn);
         MdPrefixMarker(prefix, marker, used);
         list->column[depth] = markerColumn + ui32(used);
         list->level[depth]  = ui8(wanted);
         list->depth         = depth + 1u;
      }
      // A quotation that is also an item takes its marker first and its "> " after it, on the item's
      // first line, and both on every line after: "- > quoted".
      if(block->kind == IR_BLOCK_QUOTE) MdPrefixSame(prefix, MD_QUOTE_PREFIX, 2u);

      cbool ordered = (block->listFlags & IR_LIST_ORDERED) != 0;
      cbool content = MdItemHasContent(document, block);
      // Whether this item joins a list a reader already has open is asked of the *open* list at this
      // depth and never of the block immediately above, because the two are not the same question: an
      // item that follows a nested one is above it in the output, so the block before it sits at a
      // deeper depth while the list it is about to join is the one two markers back. Comparing against
      // the previous block let a restart after a nested item merge into the list it was meant to leave,
      // and a renderer then renumbered it from the earlier list's own start.
      //
      // Only an *ordered* pair can need separating, and that is a policy rather than an omission. What
      // a merge costs is the second list's start number, which a renderer takes from its first item and
      // would then discard; two bullet lists that merge lose nothing a reader can see, so a comment
      // between them would be markup written for no one. A pair whose marker kinds differ separates
      // itself, because changing the marker starts a new list.
      cbool sibling = (list->open[depth] == MD_LIST_NUMBERS);
      cbool shaped  = sibling && ordered && !plain;

      // Two lists that meet with the same marker at the same level merge into one, and the second one's
      // start number is then discarded -- so a list the item above it was not part of takes mapping row
      // 17's HTML comment. It carries the level's own indentation, which is what keeps a restart inside
      // a nested list inside the item holding it, and it needs no blank line on either side.
      if(shaped && (block->listFlags & IR_LIST_FIRST)) {
         if(!MdAppendRun(emitter, ' ', markerColumn)) return false;
         if(!MdAppendText(emitter, MD_LIST_SPLIT)) return false;
         if(!MdAppendByte(emitter, '\n')) return false;
      }
      // A block that cannot interrupt a paragraph needs a blank line in front of it, or its own first
      // line is read as more of the paragraph above it. Three shapes arise inside a list: a marker-less
      // continuation paragraph, a nested list whose first number is not 1, and a nested list whose first
      // item is empty. The last is the worst of the three, because a lone "-" under a line of text is a
      // setext underline and turns the item above it into a heading rather than merely losing structure.
      cbool nested = emitted && depth > written;
      cbool opens  = plain || (nested && ((ordered && block->listNumber != 1u) || !content));

      if(opens && emitted && !MdAppendByte(emitter, '\n')) return false;
      written = depth;
      emitted = block;
      // Anything emitted at this depth ends every list a reader had open below it, and a marker opens
      // one here. A marker-less continuation paragraph closes the deeper lists the same way and opens
      // nothing, because it is a paragraph of the item it continues rather than an item of its own.
      for(ui32 level = depth + 1u; level < MD_MAX_LIST_LEVELS; ++level) list->open[level] = MD_LIST_SHUT;
      if(!plain) list->open[depth] = (ordered ? MD_LIST_NUMBERS : MD_LIST_BULLETS);
      if(block->kind == IR_BLOCK_CODE) {
         ui32 run = index + 1u;

         // Row 12's merge, inside an item: a marker-less code paragraph whose lines land in the column
         // this one's already do is another paragraph of the same item rather than a second block of
         // code, so it belongs inside this fence. A code paragraph carrying a marker of its own is an
         // item in its own right and ends the run, because merging it would delete its marker.
         while(run < last) {
            cIR_BLOCKptr next = IrBlockAt(document, run);

            if(!next || next->kind != IR_BLOCK_CODE) break;
            if(!(next->listFlags & IR_LIST_PLAIN)) break;
            if(MdPlainIndent(list, next->listLevel) != prefix->contUsed) break;
            ++run;
         }
         if(!MdEmitFence(emitter, document, index, run, prefix)) return false;
         index = run - 1u;
      } else if(!MdEmitLines(emitter, document, block, prefix)) {
         return false;
      }
   }
   return true;
}

//-- Tables

// Whether one table has to be written as raw HTML. A table holding another has no pipe form at all, so
// it takes the fallback under either policy; a table holding a merge has one that pads, and --tables
// is what chooses between padding it and keeping the merge.
static cbool MdTableAsHtml(cMD_EMITTERptr emitter, cIR_TABLEptr table) {
   if(table->flags & IR_TABLE_NESTED) return true;
   return (table->flags & IR_TABLE_MERGED) && emitter->tables == TABLE_MODE_HTML_ON_MERGE;
}

// Whether a block puts anything in a cell. It is IrHasContent's question rather than the narrower one a
// fence asks, because a cell holding nothing but a picture or a live bookmark is not an empty cell.
static cbool MdCellBlockHasContent(cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   if(!block) return false;
   if(block->kind == IR_BLOCK_TABLE) return true;
   return IrHasContent(document, block->spanAt, block->spanAt + block->spanCount);
}

// Writes one block of a cell into the line, in the spelling a pipe table can carry.
//
// A pipe table's cell is inline content and nothing else, so the block structure inside one is
// flattened rather than dropped: a list item keeps its marker as literal text, because losing "3." from
// a cell loses the document's own count; a code paragraph becomes a code span, which is the inline form
// of the fence it would otherwise have been; and a heading, a quotation and a horizontal rule keep only
// what they say, because "#" and "> " in a cell are literal text a reader would have to ignore.
static cbool MdCellBlock(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block, cbool dollars) {
   MD_LINK link    = {0, 0, 0, false};
   cbool   code    = (block->kind == IR_BLOCK_CODE);
   bool    started = false;
   bool    pending = false; // A break seen but not yet written, in case nothing follows it

   // A marked item keeps its marker as literal text. Losing "3." from a cell loses the document's own
   // count, and there is nowhere else in a pipe table to put it; a marker-less continuation has none.
   if((block->listFlags & IR_LIST_ITEM) && !(block->listFlags & IR_LIST_PLAIN)) {
      char  marker[MD_MAX_MARKER];
      cui64 used = MdListMarker(block, marker);

      if(!MdLineEscaped(emitter, marker, used, MD_CONTEXT_TABLE_CELL, dollars, true)) return false;
      started = true;
   }
   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || MdSpanIsSilent(span)) continue;
      if(span->kind == IR_SPAN_BREAK) {
         // A pipe table's row is one line by construction, so a hard break inside a cell is the
         // element and never the backslash or the two spaces --hard-break chooses between. It is held
         // rather than written, because a break with nothing after it is dropped -- a trailing "<br>"
         // is a line ending inside a cell that has no next line.
         if(started) pending = true;
         continue;
      }
      if(span->kind != IR_SPAN_TEXT) {
         if(pending && !MdLineText(emitter, MD_CELL_BREAK)) return false;
         pending = false;
         if(!MdWriteMarker(emitter, document, span, &link, dollars, true)) return false;
         started = true;
         continue;
      }

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      if(!started) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      if(start >= span->textBytes) continue;
      if(pending && !MdLineText(emitter, MD_CELL_BREAK)) return false;
      pending = false;

      // A code paragraph has no fence to become inside a cell, so it becomes the inline form of one.
      cui32    fmt     = (code ? span->fmt | IR_FMT_CODE : span->fmt);
      cMD_EDGE ahead   = MdEdgeAhead(document, block, index + 1u, block->spanCount);
      cui32    nextFmt = MdFormatAhead(document, block, index + 1u, block->spanCount);

      if(!MdWriteSpan(emitter, bytes + start, span->textBytes - start, fmt, dollars, ahead, nextFmt, link.open, true)) return false;
      started = true;
   }
   if(!MdCloseLink(emitter, document, &link, true)) return false;
   while(emitter->lineUsed && MdIsPad(emitter->line[emitter->lineUsed - 1u])) emitter->lineUsed -= 1u;
   return true;
}

// Whether one cell holds two or more dollar signs, which is D12's whole rule at a cell's own scope.
// A cell is one line of the emitted row however many paragraphs it holds, so the count is taken over
// all of them: counted per block, "costs $5" and "and $10" in two paragraphs are one dollar each and
// neither is escaped, which restores exactly the corruption D12 was ruled to fix. A dollar inside a
// code span does not count, and a code *paragraph* becomes a code span here, so neither does one of
// those.
static cbool MdCellDollars(cIR_DOCUMENTptr document, cIR_CELLptr cell) {
   ui64 found = 0;

   for(ui32 index = 0; index < cell->blockCount; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, cell->blockAt + index);

      if(!block || block->kind == IR_BLOCK_CODE) continue;
      for(ui32 at = 0; at < block->spanCount; ++at) {
         cIR_SPANptr span = IrSpanAt(document, block->spanAt + at);

         if(!span || span->kind != IR_SPAN_TEXT || (span->fmt & IR_FMT_CODE)) continue;
         found += MdEscapeCountDollars(IrText(document, span->textAt), span->textBytes);
      }
   }
   return found >= 2u;
}

// Strips whatever the end of a cell has no next line for: its trailing padding, and the break element
// that padding hid. A break with nothing after it is dropped everywhere else in this emitter, and a
// cell is no exception -- a trailing "<br>" is a line ending in a cell that has no next line. Both are
// stripped in a loop, because a break may be followed by padding and padding by another break.
//
// It takes the buffer rather than the emitter because the two table forms hold a cell in different
// places -- the pipe form assembles one in the line buffer, the raw-HTML form writes it straight to
// the output -- and the rule is the same for both. floorAt is where this cell's own content began, so
// the walk can never eat the tag that opened it, and MdIsPad is the ASCII pair only, so it stops at
// the newline a nested table closes with.
// @return Where the content ends once the trailing padding and breaks are off it.
static cui64 MdTrimBreakEnd(cchptr text, cui64 used, cui64 floorAt) {
   ui64 length = 0;
   ui64 at     = used;

   while(MD_CELL_BREAK[length]) ++length;

   for(;;) {
      while(at > floorAt && MdIsPad(text[at - 1u])) --at;
      if(at < floorAt + length) return at;

      cchptr tail = text + at - length;
      ui64   step = 0;

      while(step < length && tail[step] == MD_CELL_BREAK[step]) ++step;
      if(step < length) return at;
      at -= length;
   }
}

// Assembles one cell's whole content into the line buffer, its blocks joined by "<br>".
//
// A block that puts nothing on the page is skipped rather than joined, which is what keeps an empty
// paragraph -- the one every w:tc carries even when the cell is blank -- from becoming a "<br>" a
// reader sees. A cell that is a vertical merge's continuation is empty whatever it holds, because that
// is what Word draws and what CONVERSION_REFERENCE row 19 rules.
static cbool MdAssembleCell(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_CELLptr cell) {
   cbool dollars = MdCellDollars(document, cell);
   bool  started = false;

   emitter->lineUsed = 0;
   if(cell->flags & IR_CELL_VMERGED) return true;
   for(ui32 index = 0; index < cell->blockCount; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, cell->blockAt + index);

      if(!block) continue;
      // A nested table cannot appear here: a table holding one is written as raw HTML instead. The
      // skip is what keeps that a fact about the emitter rather than a promise about the walk.
      if(block->kind == IR_BLOCK_TABLE) {
         cIR_TABLEptr nested = IrTableAt(document, block->tableAt);

         if(nested && nested->blockEnd > cell->blockAt + index) index = nested->blockEnd - cell->blockAt - 1u;
         continue;
      }
      if(!MdCellBlockHasContent(document, block)) continue;
      if(started && !MdLineText(emitter, MD_CELL_BREAK)) return false;
      if(!MdCellBlock(emitter, document, block, dollars)) return false;
      started = true;
   }
   emitter->lineUsed = MdTrimBreakEnd(emitter->line, emitter->lineUsed, 0);
   return true;
}

// Writes one cell of a pipe row: a space, the cell's content, a space and the closing bar.
static cbool MdEmitPipeCell(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_CELLptr cell) {
   if(!MdAppendByte(emitter, ' ')) return false;
   if(cell) {
      if(!MdAssembleCell(emitter, document, cell)) return false;
      if(!MdAppend(emitter, emitter->line, emitter->lineUsed)) return false;
      emitter->lineUsed = 0;
   }
   return MdAppendText(emitter, " |");
}

// Writes one row of a pipe table, padded to the table's own width.
//
// The cells are walked once and the padding falls out of their own columns: a cell covering two of them
// puts its content in the first and leaves the second empty, which is row 19's policy A, and a row that
// stops short of the grid is filled to it. Nothing is looked up per column, so a wide table costs one
// pass over its cells rather than one scan per column.
static cbool MdEmitPipeRow(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_TABLEptr table, cIR_ROWptr row, cMD_PREFIXptrc prefix) {
   ui32 column = 0;

   if(!MdWritePrefix(emitter, prefix, false)) return false;
   if(!MdAppendByte(emitter, '|')) return false;
   for(ui32 index = row->firstCell; index != IR_NO_INDEX;) {
      cIR_CELLptr cell = IrCellAt(document, index);

      if(!cell) break;
      // Defensive, and reachable by nothing this build reads: a cell's column is derived from the one
      // before it in IrBeginCell rather than taken from the document, so a row's cells are contiguous
      // and this loop cannot run. What would make it live is w:gridBefore, the one place OOXML lets a
      // row start part-way across the grid -- see the note in CLAUDE.md's Known gaps. Kept because
      // filling a gap is what that element will need, and because closing one up slides a row left.
      while(column < cell->column && column < table->columns) {
         if(!MdEmitPipeCell(emitter, document, nullptr)) return false;
         ++column;
      }
      for(ui32 at = 0; at < cell->span && column < table->columns; ++at, ++column) {
         if(!MdEmitPipeCell(emitter, document, (at ? nullptr : cell))) return false;
      }
      index = cell->nextCell;
   }
   while(column < table->columns) {
      if(!MdEmitPipeCell(emitter, document, nullptr)) return false;
      ++column;
   }
   return MdAppendByte(emitter, '\n');
}

// Writes the delimiter row, which is what makes the lines above and below it a table at all: GFM reads
// a pipe table only where this row holds exactly as many cells as the header.
static cbool MdEmitDelimiterRow(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_TABLEptr table, cMD_PREFIXptrc prefix) {
   if(!MdWritePrefix(emitter, prefix, false)) return false;
   if(!MdAppendByte(emitter, '|')) return false;
   for(ui32 column = 0; column < table->columns; ++column) {
      cIR_ALIGN align = IrAlignOf(document, table, column);
      cbool     left  = (align == IR_ALIGN_LEFT || align == IR_ALIGN_CENTRE);
      cbool     right = (align == IR_ALIGN_RIGHT || align == IR_ALIGN_CENTRE);

      if(!MdAppendByte(emitter, ' ')) return false;
      if(left && !MdAppendByte(emitter, ':')) return false;
      if(!MdAppendRun(emitter, '-', MD_DELIMITER_DASHES)) return false;
      if(right && !MdAppendByte(emitter, ':')) return false;
      if(!MdAppendText(emitter, " |")) return false;
   }
   return MdAppendByte(emitter, '\n');
}

// Emits one table as a GFM pipe table.
static cbool MdEmitTablePipes(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_TABLEptr table, cMD_PREFIXptrc prefix) {
   ui32 index = table->firstRow;
   bool first = true;

   while(index != IR_NO_INDEX) {
      cIR_ROWptr row = IrRowAt(document, index);

      if(!row) break;
      if(!MdEmitPipeRow(emitter, document, table, row, prefix)) return false;
      // The first row is the header, always. w:trPr/w:tblHeader marks a row that repeats at a page
      // break and a table may mark several, but GFM has exactly one header row and it is the one at
      // the top -- so promoting a later one would reorder the document, which every pass above the
      // walk reads in order. See CLAUDE.md's mapping row, which rules the header to be the first row.
      if(first && !MdEmitDelimiterRow(emitter, document, table, prefix)) return false;
      first = false;
      index = row->nextRow;
   }
   return true;
}

//-- The raw-HTML table

// How many rows one vertical merge covers, counted forward from the row its restart cell stands in.
//
// A row below extends the merge only where it continues **every** column the restart covers, which is
// what keeps the rectangle a rowspan promises true. Counted at the restart's first column alone, a
// restart wider than the continuation under it claimed columns nothing continued: held[] was stamped
// across the whole span, the next ordinary cell of that row was pushed past it by the browser's own
// grid algorithm, and the table gained a column the pipe form of the same document does not have.
//
// Three separate things bound the cost, and all three are needed. The scan stops at the first row that
// does not continue the whole span, so the runs two restarts in one column cover are disjoint. The
// inner walk stops at the first column no continuation claims, which a row's increasing columns make
// sound, and it never looks past the restart's own end. And
// the caller does not ask at all for a cell outside the grid, which is what caps the number of calls
// per row at the grid's own width. Without the last two this was quadratic in the document's own cell
// count: 64,000 restarts over 64,000 continuations is a 21 KB .docx that took sixteen seconds, and the
// archive's caps leave room for a file that would take hours.
static cui32 MdRowSpanOf(cIR_DOCUMENTptr document, cIR_ROWptr row, cui32 column, cui32 span) {
   cui32 end  = column + span;
   ui32  rows = 1u;
   ui32  next = row->nextRow;

   while(next != IR_NO_INDEX) {
      cIR_ROWptr below = IrRowAt(document, next);
      ui32       need  = column;

      if(!below) break;
      // Every column the restart covers has to be continued, not just the one it starts at. A row's
      // cells carry strictly increasing columns, so this walks forward filling need and stops at the
      // first column no continuation claims -- which also keeps it bounded: it skips at most column
      // cells to reach the run and covers at most span more.
      for(ui32 index = below->firstCell; index != IR_NO_INDEX && need < end;) {
         cIR_CELLptr cell = IrCellAt(document, index);

         if(!cell) break;
         if(cell->column + cell->span <= need) {
            index = cell->nextCell;
            continue;
         }
         if(cell->column > need || !(cell->flags & IR_CELL_VMERGED)) break;
         need  = cell->column + cell->span;
         index = cell->nextCell;
      }
      if(need < end) break;
      ++rows;
      next = below->nextRow;
   }
   return rows;
}

// Writes one number as an HTML attribute value.
static cbool MdAppendNumber(MD_EMITTERptrc emitter, cui32 value) {
   char digits[12];
   ui64 count = 0;
   ui32 left  = value;

   do {
      digits[count++] = char('0' + (left % 10u));
      left /= 10u;
   } while(left && count < sizeof(digits));
   while(count) {
      if(!MdAppendByte(emitter, digits[--count])) return false;
   }
   return true;
}

// Writes one span in the raw-HTML fallback's own spelling. Nothing Markdown says is true inside an HTML
// block -- it runs to the next blank line and every byte of it is passed through unparsed -- so every
// delimiter is an element and every escape is an entity. The nesting is MdWriteSpan's, outermost first.
static cbool MdHtmlSpan(MD_EMITTERptrc emitter, cchptr bytes, cui64 byteCount, cui32 fmt) {
   cbool bold   = (fmt & IR_FMT_BOLD) != 0;
   cbool italic = (fmt & IR_FMT_ITALIC) != 0;
   cbool code   = (fmt & IR_FMT_CODE) != 0;

   if(!byteCount) return true;
   if((fmt & IR_FMT_SUPER) && !MdAppendText(emitter, "<sup>")) return false;
   if((fmt & IR_FMT_SUB) && !MdAppendText(emitter, "<sub>")) return false;
   if((fmt & IR_FMT_STRIKE) && !MdAppendText(emitter, "<del>")) return false;
   // Code drops bold and italic here for the reason mapping row 11 gives everywhere else, so that two
   // runs that come out as one code span really are one.
   if(!code && bold && !MdAppendText(emitter, "<strong>")) return false;
   if(!code && italic && !MdAppendText(emitter, "<em>")) return false;
   if(code && !MdAppendText(emitter, "<code>")) return false;
   if(!MdAppendEscaped(emitter, bytes, byteCount, MD_CONTEXT_HTML_BLOCK, false)) return false;
   if(code && !MdAppendText(emitter, "</code>")) return false;
   if(!code && italic && !MdAppendText(emitter, "</em>")) return false;
   if(!code && bold && !MdAppendText(emitter, "</strong>")) return false;
   if((fmt & IR_FMT_STRIKE) && !MdAppendText(emitter, "</del>")) return false;
   if((fmt & IR_FMT_SUB) && !MdAppendText(emitter, "</sub>")) return false;
   if((fmt & IR_FMT_SUPER) && !MdAppendText(emitter, "</sup>")) return false;
   return true;
}

// Writes one of M7's marker spans in the fallback's spelling: an anchor and a link are both an <a>, an
// image an <img>, and a link's two halves are the element's own two halves.
static cbool MdHtmlMarker(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_SPANptr span) {
   if(span->kind == IR_SPAN_LINK_START) {
      if(!MdAppendText(emitter, "<a href=\"")) return false;
      if(!MdAppendEscaped(emitter, IrDest(document, span->destAt), span->destBytes, MD_CONTEXT_HTML_BLOCK, false)) return false;
      return MdAppendText(emitter, "\">");
   }
   if(span->kind == IR_SPAN_LINK_END) return MdAppendText(emitter, "</a>");
   if(span->kind == IR_SPAN_ANCHOR) {
      if(!MdAppendText(emitter, "<a id=\"")) return false;
      if(!MdAppend(emitter, IrDest(document, span->destAt), span->destBytes)) return false;
      return MdAppendText(emitter, "\"></a>");
   }
   if(!MdAppendText(emitter, "<img src=\"")) return false;
   if(!MdAppendEscaped(emitter, IrDest(document, span->destAt), span->destBytes, MD_CONTEXT_HTML_BLOCK, false)) return false;
   if(!MdAppendText(emitter, "\" alt=\"")) return false;
   if(!MdAppendEscaped(emitter, IrText(document, span->textAt), span->textBytes, MD_CONTEXT_HTML_BLOCK, false)) return false;
   return MdAppendText(emitter, "\">");
}

// Writes one block of a cell in the fallback's spelling. The flattening is the pipe form's -- a list
// item keeps its marker, a code paragraph becomes a code span -- because what the fallback exists to
// preserve is the table's *shape*, and a cell that held blocks in one form and not the other would make
// the same document read differently for the sake of a merge somewhere else in it.
static cbool MdHtmlBlock(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_BLOCKptr block) {
   cbool code    = (block->kind == IR_BLOCK_CODE);
   bool  started = false;
   bool  pending = false; // A break seen but not yet written, in case nothing follows it

   if((block->listFlags & IR_LIST_ITEM) && !(block->listFlags & IR_LIST_PLAIN)) {
      char  marker[MD_MAX_MARKER];
      cui64 used = MdListMarker(block, marker);

      if(!MdAppendEscaped(emitter, marker, used, MD_CONTEXT_HTML_BLOCK, false)) return false;
      started = true;
   }
   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || MdSpanIsSilent(span)) continue;
      if(span->kind == IR_SPAN_BREAK) {
         if(started) pending = true;
         continue;
      }
      if(span->kind != IR_SPAN_TEXT) {
         if(pending && !MdAppendText(emitter, MD_CELL_BREAK)) return false;
         pending = false;
         if(!MdHtmlMarker(emitter, document, span)) return false;
         started = true;
         continue;
      }

      cchptr bytes = IrText(document, span->textAt);
      ui64   start = 0;

      if(!started) {
         while(start < span->textBytes && MdIsPad(bytes[start])) ++start;
      }
      if(start >= span->textBytes) continue;
      if(pending && !MdAppendText(emitter, MD_CELL_BREAK)) return false;
      pending = false;
      if(!MdHtmlSpan(emitter, bytes + start, span->textBytes - start, (code ? span->fmt | IR_FMT_CODE : span->fmt))) return false;
      started = true;
   }
   return true;
}

static cbool MdEmitTableHtml(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_TABLEptr table, cMD_PREFIXptrc prefix);

// Writes one cell's content between its own tags: its blocks joined by "<br>", and a nested table as a
// <table> of its own, which is the whole reason this form exists.
static cbool MdHtmlCell(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_CELLptr cell, cMD_PREFIXptrc prefix) {
   bool started = false;

   if(cell->flags & IR_CELL_VMERGED) return true;
   for(ui32 index = 0; index < cell->blockCount; ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, cell->blockAt + index);

      if(!block) continue;
      if(block->kind == IR_BLOCK_TABLE) {
         cIR_TABLEptr nested = IrTableAt(document, block->tableAt);

         if(!nested) continue;
         if(!MdEmitTableHtml(emitter, document, nested, prefix)) return false;
         index   = (nested->blockEnd > cell->blockAt + index ? nested->blockEnd - cell->blockAt - 1u : index);
         started = false; // A table is not a line a "<br>" continues
         continue;
      }
      if(!MdCellBlockHasContent(document, block)) continue;
      if(started && !MdAppendText(emitter, MD_CELL_BREAK)) return false;
      if(!MdHtmlBlock(emitter, document, block)) return false;
      started = true;
   }
   return true;
}

// Writes one empty cell of the raw-HTML table. It takes the whole opening tag rather than the tag
// name, because a pad carries no attribute and a cell that does writes its own closing bracket.
static cbool MdEmitHtmlPad(MD_EMITTERptrc emitter, cchptr open, cchptr close) { return MdAppendText(emitter, open) && MdAppendText(emitter, close); }

// Emits one table as a raw <table>. Every row is one line and no line is blank, because a CommonMark
// HTML block ends at the first blank line and whatever followed would be read as Markdown again.
//
// The open-merge count per column is what makes the grid exact. A cell a rowspan above already covers
// writes nothing, and a w:vMerge continuation that *nothing* covers -- which a producer writes when an
// intervening row spans across the column the merge was opened in -- is an ordinary empty cell rather
// than nothing at all. Dropped, it would leave the row a column short, which is a silently narrower
// table: the one failure CONVERSION_REFERENCE row 19 names by saying a row must never lose a column.
static cbool MdEmitTableHtml(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_TABLEptr table, cMD_PREFIXptrc prefix) {
   ui32 held[IR_MAX_COLUMNS];
   ui32 index  = table->firstRow;
   bool header = true;

   for(ui32 column = 0; column < IR_MAX_COLUMNS; ++column) held[column] = 0;
   if(!MdWritePrefix(emitter, prefix, false)) return false;
   if(!MdAppendText(emitter, "<table>\n")) return false;
   while(index != IR_NO_INDEX) {
      cIR_ROWptr row    = IrRowAt(document, index);
      cchptr     open   = (header ? "<th" : "<td");
      cchptr     bare   = (header ? "<th>" : "<td>");
      cchptr     close  = (header ? "</th>" : "</td>");
      ui32       column = 0;

      if(!row) break;
      if(!MdWritePrefix(emitter, prefix, false)) return false;
      if(!MdAppendText(emitter, "<tr>")) return false;
      for(ui32 at = row->firstCell; at != IR_NO_INDEX;) {
         cIR_CELLptr cell = IrCellAt(document, at);

         if(!cell) break;
         if(cell->flags & IR_CELL_VMERGED) {
            // A continuation the merge above it covers writes nothing at all; one that nothing covers
            // is an ordinary empty cell, because dropping it leaves the row a column short.
            for(ui32 span = 0; span < cell->span && column < IR_MAX_COLUMNS; ++span, ++column) {
               if(!held[column] && !MdEmitHtmlPad(emitter, bare, close)) return false;
            }
            at = cell->nextCell;
            continue;
         }
         // An ordinary cell can never land where a merge above it is still open, and the two halves of
         // that are both here: a rowspan is exactly the run of continuation cells below it, so held
         // expires on the row after the last of them -- and a row whose cell at that column is
         // ordinary rather than a continuation is the row the run stopped at. So no skip is written
         // for it, and none is needed.
         // The loop below is the pipe form's gap loop and is dead for the same reason that one is --
         // w:gridBefore is what would make it live. The held[] test is what it would then need.
         while(column < cell->column && column < IR_MAX_COLUMNS) {
            if(!held[column] && !MdEmitHtmlPad(emitter, bare, close)) return false;
            ++column;
         }

         // A cell past the grid writes no held[] entry and decorates a column the table does not have,
         // so its row span is never asked for: that guard is what bounds the walk below at 256 calls,
         // and nothing caps how many cells a row may hold.
         cbool restart = (cell->flags & IR_CELL_VRESTART) && cell->column < IR_MAX_COLUMNS;
         cui32 rows    = (restart ? MdRowSpanOf(document, row, cell->column, cell->span) : 1u);

         if(!MdAppendText(emitter, open)) return false;
         if(cell->span > 1u) {
            if(!MdAppendText(emitter, " colspan=\"") || !MdAppendNumber(emitter, cell->span) || !MdAppendByte(emitter, '"')) return false;
         }
         if(rows > 1u) {
            if(!MdAppendText(emitter, " rowspan=\"") || !MdAppendNumber(emitter, rows) || !MdAppendByte(emitter, '"')) return false;
         }
         if(!MdAppendByte(emitter, '>')) return false;

         cui64 contentAt = emitter->used;

         if(!MdHtmlCell(emitter, document, cell, prefix)) return false;
         emitter->used = MdTrimBreakEnd(emitter->out, emitter->used, contentAt);
         if(!MdAppendText(emitter, close)) return false;
         for(ui32 span = 0; span < cell->span && column < IR_MAX_COLUMNS; ++span, ++column) held[column] = rows;
         at = cell->nextCell;
      }
      while(column < table->columns && column < IR_MAX_COLUMNS) {
         if(!held[column] && !MdEmitHtmlPad(emitter, bare, close)) return false;
         ++column;
      }
      // Only the grid's own columns can hold a merge, and IrEndTable has already clamped that count,
      // so a one-column table does not pay for 256 of them on every row it has.
      for(ui32 span = 0; span < table->columns && span < IR_MAX_COLUMNS; ++span) {
         if(held[span]) held[span] -= 1u;
      }
      if(!MdAppendText(emitter, "</tr>\n")) return false;
      header = false;
      index  = row->nextRow;
   }
   if(!MdWritePrefix(emitter, prefix, false)) return false;
   return MdAppendText(emitter, "</table>\n");
}

// Emits one table, in whichever of its two forms its own shape and --tables call for.
static cbool MdEmitTable(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document, cIR_TABLEptr table, cMD_PREFIXptrc prefix) {
   if(MdTableAsHtml(emitter, table)) return MdEmitTableHtml(emitter, document, table, prefix);
   return MdEmitTablePipes(emitter, document, table, prefix);
}

//-- Separation

// Writes what stands between two blocks: exactly one blank line, with one exception. Two consecutive
// quote paragraphs are one quotation that a producer happened to break in two, and a blank line between
// them would close the blockquote and open a second; a bare ">" keeps them inside one, which is what a
// reader of the .docx sees. That is the only place the one-blank-line rule bends, and it bends towards
// the same shape: the separator is still exactly one line.
// It takes the blocks and not their kinds, because since M8 a kind no longer says everything a
// separator needs: a quotation that is also a list item is still IR_BLOCK_QUOTE, and joining it to the
// quotation before it with a bare ">" would put that marker outside the item it belongs to.
static cbool MdSeparate(MD_EMITTERptrc emitter, cIR_BLOCKptr previous, cIR_BLOCKptr next) {
   cbool listed = (previous && (previous->listFlags & IR_LIST_ITEM)) || (next && (next->listFlags & IR_LIST_ITEM));

   if(!listed && previous && next && previous->kind == IR_BLOCK_QUOTE && next->kind == IR_BLOCK_QUOTE) {
      if(!MdAppendText(emitter, MD_QUOTE_JOIN)) return false;
   }
   return MdAppendByte(emitter, '\n');
}

//== Entry points

void MdOpen(MD_EMITTERptrc emitter, cHARD_BREAK hardBreak, cTABLE_MODE tables) {
   mzero(emitter, sizeof(MD_EMITTER));
   emitter->hardBreak = hardBreak;
   emitter->tables    = tables;
}

void MdClose(MD_EMITTERptrc emitter) {
   cHARD_BREAK hardBreak = emitter->hardBreak;
   cTABLE_MODE tables    = emitter->tables;

   mdealloc(emitter->out);
   mdealloc(emitter->line);
   MdOpen(emitter, hardBreak, tables);
}

cMD_RESULT MdEmitDocument(MD_EMITTERptrc emitter, cIR_DOCUMENTptr document) {
   cui32        blocks   = IrBlockCount(document);
   ui32         index    = 0;
   cIR_BLOCKptr previous = nullptr;
   bool         wrote    = (emitter->used != 0);
   MD_LIST      list;
   MD_PREFIX    prefix;

   list.depth = 0;
   MdPrefixClear(&prefix);
   while(index < blocks) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) {
         ++index;
         continue;
      }
      // A run of items is grouped the way a run of code paragraphs is, and for the same reason: what
      // separates two items of one list is nothing at all, which no block separator can write.
      if(block->listFlags & IR_LIST_ITEM) {
         ui32 last = index;

         while(last < blocks) {
            cIR_BLOCKptr next = IrBlockAt(document, last);

            if(!next || !(next->listFlags & IR_LIST_ITEM)) break;
            ++last;
         }

         ui32 from = index;
         ui32 to   = last;

         // A content-free item is a marker on a line of its own in the middle of a list, and nothing at
         // all at either end of one: an empty last item is the paragraph a user leaves behind on
         // pressing Enter to get out of a list, and an empty first one is the same artefact at the top.
         //
         // Unless the item after it is deeper. Then it is not an artefact but the *parent* those items
         // hang from, and dropping it promotes them to the outer list -- where a later shallower item
         // becomes their sibling and a renderer counts it on from their numbers rather than from its
         // own. The trailing edge needs no such test: an item at the end of a run has nothing after it
         // to be the parent of.
         while(from < to) {
            cIR_BLOCKptr head = IrBlockAt(document, from);
            cIR_BLOCKptr next = (from + 1u < to ? IrBlockAt(document, from + 1u) : nullptr);

            if(!head || MdItemHasContent(document, head)) break;
            if(next && next->listLevel > head->listLevel) break;
            ++from;
         }
         while(to > from && !MdItemHasContent(document, IrBlockAt(document, to - 1u))) --to;
         if(from < to) {
            if(wrote && !MdSeparate(emitter, previous, IrBlockAt(document, from))) return MD_ERROR_MEMORY;
            if(!MdEmitList(emitter, document, from, to, &list, &prefix)) return MD_ERROR_MEMORY;
            previous = IrBlockAt(document, to - 1u);
            wrote    = true;
         }
         index = last;
         continue;
      }
      // A table is emitted whole and the loop then steps over every block it owns, because a cell's
      // blocks sit in this same array in document order -- which is what lets every pass between the
      // walk and here read one flat array and know nothing about tables at all.
      if(block->kind == IR_BLOCK_TABLE) {
         cIR_TABLEptr table = IrTableAt(document, block->tableAt);

         MdPrefixClear(&prefix);
         if(table && table->firstRow != IR_NO_INDEX) {
            if(wrote && !MdSeparate(emitter, previous, block)) return MD_ERROR_MEMORY;
            if(!MdEmitTable(emitter, document, table, &prefix)) return MD_ERROR_MEMORY;
            previous = block;
            wrote    = true;
         }
         index = (table && table->blockEnd > index ? table->blockEnd : index + 1u);
         continue;
      }
      if(block->kind == IR_BLOCK_CODE) {
         ui32 last = index;

         while(last < blocks) {
            cIR_BLOCKptr next = IrBlockAt(document, last);

            // A code paragraph that is also a list item is its own fence inside its own item, so the
            // run stops before one rather than swallowing it and emitting it at column zero.
            if(!next || next->kind != IR_BLOCK_CODE || (next->listFlags & IR_LIST_ITEM)) break;
            ++last;
         }

         ui32 from = index;
         ui32 to   = last;

         // A blank code paragraph is a blank line of code, which is worth keeping inside the fence and
         // is nothing at all at either end of it. Trimming here rather than after the separator is
         // written is what keeps a run of blank ones from leaving a stray blank line behind.
         while(from < to && !MdBlockHasContent(document, IrBlockAt(document, from))) ++from;
         while(to > from && !MdBlockHasContent(document, IrBlockAt(document, to - 1u))) --to;
         if(from < to) {
            MdPrefixClear(&prefix);
            if(wrote && !MdSeparate(emitter, previous, IrBlockAt(document, from))) return MD_ERROR_MEMORY;
            if(!MdEmitFence(emitter, document, from, to, &prefix)) return MD_ERROR_MEMORY;
            previous = IrBlockAt(document, from);
            wrote    = true;
         }
         index = last;
         continue;
      }
      // Each block writes its own closing newline, so the separator is one more of them and the last
      // block leaves the file ending in a single newline. No block but a trimmed-away run of code can
      // come to nothing here -- IrEndBlock drops one that holds no printable byte -- so the separator
      // can be written before the block rather than unwound again afterwards.
      MdPrefixClear(&prefix);
      if(wrote && !MdSeparate(emitter, previous, block)) return MD_ERROR_MEMORY;
      if(block->kind == IR_BLOCK_HEADING) {
         if(!MdEmitHeading(emitter, document, block)) return MD_ERROR_MEMORY;
      } else if(block->kind == IR_BLOCK_RULE) {
         if(!MdEmitRule(emitter)) return MD_ERROR_MEMORY;
      } else {
         if(block->kind == IR_BLOCK_QUOTE) MdPrefixSame(&prefix, MD_QUOTE_PREFIX, 2u);
         if(!MdEmitLines(emitter, document, block, &prefix)) return MD_ERROR_MEMORY;
      }
      previous = block;
      wrote    = true;
      ++index;
   }
   return (emitter->failed ? MD_ERROR_MEMORY : MD_OK);
}

cchptr MdBytes(cMD_EMITTERptr emitter) { return (emitter->out ? emitter->out : ""); }

cui64 MdByteCount(cMD_EMITTERptr emitter) { return emitter->used; }
