/*
 * File: MdEscape.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
 * Description: The escaping rules themselves: one measuring and writing core, and the line-start pass.
 * To Do: 1) Add the pipe rule to the line-start pass when a table can put one at the head of a line.
 *        2) Fold U+00A0 into the line-start whitespace tests if a producer is found starting a line with one.
 * Dependencies: BuildGuards.h, MdEscape.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include "typedefs.h"
#include "MdEscape.h"

//-- Character classes

// The longest entity name CommonMark recognises is thirty-one bytes, so an ampersand that has not found
// its semicolon by then is not opening one and the scan can stop.
constexpr cui64 MD_MAX_ENTITY_BYTES = 33u;

// The hexadecimal digits a percent escape is written with.
static constexpr cchptr MD_HEX_DIGITS = "0123456789ABCDEF";

// The bytes the inline rules backslash-escape wherever they stand. Pitfall 4 is why the list is
// unconditional: a literal one of these landing beside a delimiter the emitter wrote merges with it.
static constexpr cchptr MD_INLINE_ESCAPED = "\\*_`[]~";

// The bytes a link destination percent-encodes. The percent sign itself is deliberately absent: a
// target arrives already encoded far more often than it arrives holding a literal percent, and
// re-encoding one would turn a working %20 into a broken %2520.
static constexpr cchptr MD_DEST_RESERVED = "<>()\"`\\[]{}|^";

// Whether a byte is ASCII whitespace of the kind that separates Markdown's block markers from their
// content. A line end never reaches here: the emitter splits lines before anything is escaped.
static cbool MdIsSpace(cchar byte) { return byte == ' ' || byte == '\t'; }

// Whether a byte is an ASCII letter.
static cbool MdIsAlpha(cchar byte) { return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z'); }

// Whether a byte is an ASCII digit.
static cbool MdIsDigit(cchar byte) { return byte >= '0' && byte <= '9'; }

// Whether a byte is an ASCII hexadecimal digit.
static cbool MdIsHex(cchar byte) { return MdIsDigit(byte) || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F'); }

// Whether the inline rules backslash-escape a byte wherever it stands.
static cbool MdAlwaysEscaped(cchar byte) {
   for(cchptr walk = MD_INLINE_ESCAPED; *walk; ++walk) {
      if(*walk == byte) return true;
   }
   return false;
}

// Whether an ampersand at this position would be read as an entity reference, which is the only case
// CONVERSION_REFERENCE 4.1 asks to be turned into &amp;.
static cbool MdOpensEntity(cchptr text, cui64 byteCount, cui64 at) {
   ui64  index = at + 1u;
   cui64 stop  = (byteCount - at > MD_MAX_ENTITY_BYTES ? at + MD_MAX_ENTITY_BYTES : byteCount);

   if(index < stop && text[index] == '#') {
      ++index;
      if(index < stop && (text[index] == 'x' || text[index] == 'X')) {
         ++index;
         cui64 first = index;

         while(index < stop && MdIsHex(text[index])) ++index;
         return index > first && index < stop && text[index] == ';';
      }

      cui64 first = index;

      while(index < stop && MdIsDigit(text[index])) ++index;
      return index > first && index < stop && text[index] == ';';
   }

   cui64 first = index;

   while(index < stop && (MdIsAlpha(text[index]) || MdIsDigit(text[index]))) ++index;
   return index > first && index < stop && text[index] == ';';
}

// Whether a less-than sign at this position could open an HTML tag or an autolink. Anywhere else it is
// an ordinary character and escaping it would only add noise.
static cbool MdOpensMarkup(cchptr text, cui64 byteCount, cui64 at) {
   if(at + 1u >= byteCount) return false;

   cchar next = text[at + 1u];

   return MdIsAlpha(next) || next == '/' || next == '!' || next == '?';
}

// Whether a link destination has to percent-encode a byte.
static cbool MdNeedsPercent(cchar byte) {
   if(ui8(byte) <= 0x20u || ui8(byte) == 0x7Fu) return true;
   for(cchptr walk = MD_DEST_RESERVED; *walk; ++walk) {
      if(*walk == byte) return true;
   }
   return false;
}

//-- The core

// Writes one byte, or counts it when there is nowhere to write. Every rule below funnels through this,
// so measuring and writing can never disagree about a length.
static void MdPut(chptr dest, cui64 destBytes, ui64ptrc used, cchar byte) {
   if(dest && *used < destBytes) dest[*used] = byte;
   *used += 1u;
}

// Escapes one run of text into dest, or measures it when dest is null.
static cui64 MdEscapeCore(chptr dest, cui64 destBytes, cchptr text, cui64 byteCount, cMD_CONTEXT context) {
   ui64 used = 0;

   if(!text) return 0;
   if(context == MD_CONTEXT_CODE_SPAN || context == MD_CONTEXT_CODE_BLOCK) {
      for(ui64 index = 0; index < byteCount; ++index) MdPut(dest, destBytes, &used, text[index]);
      return used;
   }
   if(context == MD_CONTEXT_LINK_DEST) {
      for(ui64 index = 0; index < byteCount; ++index) {
         cchar byte = text[index];

         if(!MdNeedsPercent(byte)) {
            MdPut(dest, destBytes, &used, byte);
            continue;
         }
         MdPut(dest, destBytes, &used, '%');
         MdPut(dest, destBytes, &used, MD_HEX_DIGITS[(ui8(byte) >> 4) & 0x0Fu]);
         MdPut(dest, destBytes, &used, MD_HEX_DIGITS[ui8(byte) & 0x0Fu]);
      }
      return used;
   }
   // A raw-HTML fallback still has its inner text parsed as inline content, so it takes the whole inline
   // set; what it adds is that the two bytes that could open markup become entities without a guess.
   cbool html = (context == MD_CONTEXT_HTML);

   for(ui64 index = 0; index < byteCount; ++index) {
      cchar byte = text[index];

      if(MdAlwaysEscaped(byte) || (context == MD_CONTEXT_TABLE_CELL && byte == '|')) {
         MdPut(dest, destBytes, &used, '\\');
         MdPut(dest, destBytes, &used, byte);
         continue;
      }
      if(byte == '<' && html) {
         for(cchptr walk = "&lt;"; *walk; ++walk) MdPut(dest, destBytes, &used, *walk);
         continue;
      }
      if(byte == '<' && MdOpensMarkup(text, byteCount, index)) {
         MdPut(dest, destBytes, &used, '\\');
         MdPut(dest, destBytes, &used, byte);
         continue;
      }
      if(byte == '&' && (html || MdOpensEntity(text, byteCount, index))) {
         for(cchptr walk = "&amp;"; *walk; ++walk) MdPut(dest, destBytes, &used, *walk);
         continue;
      }
      MdPut(dest, destBytes, &used, byte);
   }
   return used;
}

//== Entry points

cui64 MdEscapeMeasure(cchptr text, cui64 byteCount, cMD_CONTEXT context) { return MdEscapeCore(nullptr, 0, text, byteCount, context); }

cui64 MdEscapeWrite(chptrc dest, cui64 destBytes, cchptr text, cui64 byteCount, cMD_CONTEXT context) {
   cui64 wanted = MdEscapeCore(dest, destBytes, text, byteCount, context);

   return (wanted > destBytes ? destBytes : wanted);
}

csi64 MdEscapeLineStartAt(cchptr line, cui64 byteCount, cbool continuation) {
   if(!line || !byteCount) return -1;

   cchar first = line[0];

   // A blockquote marker needs nothing after it, so a leading angle bracket always opens one.
   if(first == '>') return 0;

   // An ATX heading is one to six hashes followed by a space, a tab or the end of the line.
   if(first == '#') {
      ui64 run = 0;

      while(run < byteCount && line[run] == '#') ++run;
      if(run <= 6u && (run == byteCount || MdIsSpace(line[run]))) return 0;
   }

   // A bullet is a hyphen, a plus or an asterisk followed by a space, a tab or the end of the line. The
   // asterisk never reaches here: the inline rules escape it wherever it stands. This also catches the
   // spaced spelling of a thematic break, "- - -", whose second byte is a space.
   if(first == '-' || first == '+') {
      if(byteCount == 1u || MdIsSpace(line[1])) return 0;
   }

   // A line of nothing but hyphens or nothing but equals signs, trailing padding aside. Three or more
   // hyphens are a thematic break wherever they stand; anything else here only underlines the line
   // above it into a heading, so it matters only when there is a line above it to underline.
   if(first == '-' || first == '=') {
      ui64 run  = 0;
      ui64 tail = byteCount;

      while(run < byteCount && line[run] == first) ++run;
      while(tail > run && MdIsSpace(line[tail - 1u])) --tail;
      if(tail == run) {
         if(first == '-' && run >= 3u) return 0;
         if(continuation) return 0;
      }
   }

   // An ordered-list marker is one to nine digits, then a dot or a closing parenthesis, then a space, a
   // tab or the end of the line. The punctuation is what gets escaped, so that the number survives.
   if(MdIsDigit(first)) {
      ui64 digits = 0;

      while(digits < byteCount && MdIsDigit(line[digits])) ++digits;
      if(digits <= 9u && digits < byteCount && (line[digits] == '.' || line[digits] == ')')) {
         if(digits + 1u == byteCount || MdIsSpace(line[digits + 1u])) return si64(digits);
      }
   }
   return -1;
}

csi64 MdEscapeHeadingTailAt(cchptr content, cui64 byteCount) {
   if(!content || !byteCount) return -1;

   ui64 run = byteCount;

   while(run > 0 && content[run - 1u] == '#') --run;
   if(run == byteCount) return -1;                     // The content does not end in hashes at all
   if(run && !MdIsSpace(content[run - 1u])) return -1; // Not a closing sequence: the hashes touch a word
   return si64(run);
}
