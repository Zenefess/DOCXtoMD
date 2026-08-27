/*
 * File: LinkResolver.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-27
 * Last Modified: 2026-08-27
 * Description: The reference lookup, the GFM slugger and the two Unicode tables it is generated from.
 * To Do: 1) Reuse the name index between documents once M13 gives one worker several.
 *        2) Widen the fold table to the multi-character mappings, if a heading is ever found needing one.
 *        3) Benchmark an AVX2 scan for the next byte a slug drops before adopting one (bd1/bd2).
 * Dependencies: BuildGuards.h, Ir.h, LinkResolver.h, OpcPackage.h, Utf.h, typedefs.h,
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
#include "Ir.h"
#include "OpcPackage.h"
#include "Utf.h"
#include "LinkResolver.h"

//-- Limits

// The longest relationship id this module will look up. An r:id is an XML name and every producer writes
// "rId" and a decimal; the ceiling is what keeps the lookup buffer off the heap.
constexpr cui64 LINK_MAX_REF_BYTES = 128u;

// The longest destination a resolved reference may produce. A URI longer than this is not a link anyone
// clicked, and the alternative is an allocation per link.
constexpr cui64 LINK_MAX_DEST_BYTES = 2048u;

//-- Unicode tables

// One range of code points, first and last inclusive.
struct LINK_RANGE {
   ui32 first; ///< First code point of the range
   ui32 last;  ///< Last code point of the range
};

// One run of code points that fold to lower case by the same amount. The step is 1 where every code
// point in the run folds and 2 where every other one does, which is the shape most of Latin Extended-A
// takes: capital and small letters alternate, so a run of pairs compresses four times better than a run
// of singletons. 181 runs cover every simple one-to-one lower-case mapping in the character database.
struct LINK_FOLD {
   ui32 first; ///< First code point of the run
   ui32 last;  ///< Last code point of it
   ui32 step;  ///< 1 when every code point folds, 2 when every other one does
   si32 delta; ///< What to add to a code point of the run to fold it
};

// Constant forms of the two table records, spelled per GCS r2.
typedef const LINK_RANGE cLINK_RANGE;
typedef const LINK_FOLD  cLINK_FOLD;

// Every range of code points a GFM slug keeps: the Unicode general categories L, M and N, plus connector
// punctuation, which is what github-slugger's removal regex leaves behind. The table is generated from
// the Unicode character database and sorted, so a reader who doubts a row can regenerate it and diff --
// and the reason it is not a hand-picked subset is M6's, one milestone on: a letter called punctuation
// and a punctuation character called a letter are both wrong, and here either writes a link that scrolls
// nowhere. Everything outside these ranges is dropped, except the space, which becomes a hyphen, and the
// hyphen itself, which is kept. A code point the database does not assign is kept rather than dropped,
// because the renderer's own table is generated from whichever Unicode version it was built against and
// no answer here can be right for all of them.
static constexpr LINK_RANGE LINK_SLUG_KEEP[] = {
    {0x0030u, 0x0039u},   {0x0041u, 0x005Au},   {0x005Fu, 0x005Fu},   {0x0061u, 0x007Au},   // Digit Zero
    {0x00AAu, 0x00AAu},   {0x00B2u, 0x00B3u},   {0x00B5u, 0x00B5u},   {0x00B9u, 0x00BAu},   // Feminine Ordinal Indicator
    {0x00BCu, 0x00BEu},   {0x00C0u, 0x00D6u},   {0x00D8u, 0x00F6u},   {0x00F8u, 0x02C1u},   // Vulgar Fraction One Quarter
    {0x02C6u, 0x02D1u},   {0x02E0u, 0x02E4u},   {0x02ECu, 0x02ECu},   {0x02EEu, 0x02EEu},   // Modifier Letter Circumflex Accent
    {0x0300u, 0x0374u},   {0x0376u, 0x0377u},   {0x037Au, 0x037Du},   {0x037Fu, 0x037Fu},   // Combining Grave Accent
    {0x0386u, 0x0386u},   {0x0388u, 0x038Au},   {0x038Cu, 0x038Cu},   {0x038Eu, 0x03A1u},   // Greek Capital Letter Alpha With Tonos
    {0x03A3u, 0x03F5u},   {0x03F7u, 0x0481u},   {0x0483u, 0x052Fu},   {0x0531u, 0x0556u},   // Greek Capital Letter Sigma
    {0x0559u, 0x0559u},   {0x0560u, 0x0588u},   {0x0591u, 0x05BDu},   {0x05BFu, 0x05BFu},   // Armenian Modifier Letter Left Half Ring
    {0x05C1u, 0x05C2u},   {0x05C4u, 0x05C5u},   {0x05C7u, 0x05C7u},   {0x05D0u, 0x05EAu},   // Hebrew Point Shin Dot
    {0x05EFu, 0x05F2u},   {0x0610u, 0x061Au},   {0x0620u, 0x0669u},   {0x066Eu, 0x06D3u},   // Hebrew Yod Triangle
    {0x06D5u, 0x06DCu},   {0x06DFu, 0x06E8u},   {0x06EAu, 0x06FCu},   {0x06FFu, 0x06FFu},   // Arabic Letter Ae
    {0x0710u, 0x074Au},   {0x074Du, 0x07B1u},   {0x07C0u, 0x07F5u},   {0x07FAu, 0x07FAu},   // Syriac Letter Alaph
    {0x07FDu, 0x07FDu},   {0x0800u, 0x082Du},   {0x0840u, 0x085Bu},   {0x0860u, 0x086Au},   // Nko Dantayalan
    {0x0870u, 0x0887u},   {0x0889u, 0x088Eu},   {0x0898u, 0x08E1u},   {0x08E3u, 0x0963u},   // Arabic Letter Alef With Attached Fatha
    {0x0966u, 0x096Fu},   {0x0971u, 0x0983u},   {0x0985u, 0x098Cu},   {0x098Fu, 0x0990u},   // Devanagari Digit Zero
    {0x0993u, 0x09A8u},   {0x09AAu, 0x09B0u},   {0x09B2u, 0x09B2u},   {0x09B6u, 0x09B9u},   // Bengali Letter O
    {0x09BCu, 0x09C4u},   {0x09C7u, 0x09C8u},   {0x09CBu, 0x09CEu},   {0x09D7u, 0x09D7u},   // Bengali Sign Nukta
    {0x09DCu, 0x09DDu},   {0x09DFu, 0x09E3u},   {0x09E6u, 0x09F1u},   {0x09F4u, 0x09F9u},   // Bengali Letter Rra
    {0x09FCu, 0x09FCu},   {0x09FEu, 0x09FEu},   {0x0A01u, 0x0A03u},   {0x0A05u, 0x0A0Au},   // Bengali Letter Vedic Anusvara
    {0x0A0Fu, 0x0A10u},   {0x0A13u, 0x0A28u},   {0x0A2Au, 0x0A30u},   {0x0A32u, 0x0A33u},   // Gurmukhi Letter Ee
    {0x0A35u, 0x0A36u},   {0x0A38u, 0x0A39u},   {0x0A3Cu, 0x0A3Cu},   {0x0A3Eu, 0x0A42u},   // Gurmukhi Letter Va
    {0x0A47u, 0x0A48u},   {0x0A4Bu, 0x0A4Du},   {0x0A51u, 0x0A51u},   {0x0A59u, 0x0A5Cu},   // Gurmukhi Vowel Sign Ee
    {0x0A5Eu, 0x0A5Eu},   {0x0A66u, 0x0A75u},   {0x0A81u, 0x0A83u},   {0x0A85u, 0x0A8Du},   // Gurmukhi Letter Fa
    {0x0A8Fu, 0x0A91u},   {0x0A93u, 0x0AA8u},   {0x0AAAu, 0x0AB0u},   {0x0AB2u, 0x0AB3u},   // Gujarati Letter E
    {0x0AB5u, 0x0AB9u},   {0x0ABCu, 0x0AC5u},   {0x0AC7u, 0x0AC9u},   {0x0ACBu, 0x0ACDu},   // Gujarati Letter Va
    {0x0AD0u, 0x0AD0u},   {0x0AE0u, 0x0AE3u},   {0x0AE6u, 0x0AEFu},   {0x0AF9u, 0x0AFFu},   // Gujarati Om
    {0x0B01u, 0x0B03u},   {0x0B05u, 0x0B0Cu},   {0x0B0Fu, 0x0B10u},   {0x0B13u, 0x0B28u},   // Oriya Sign Candrabindu
    {0x0B2Au, 0x0B30u},   {0x0B32u, 0x0B33u},   {0x0B35u, 0x0B39u},   {0x0B3Cu, 0x0B44u},   // Oriya Letter Pa
    {0x0B47u, 0x0B48u},   {0x0B4Bu, 0x0B4Du},   {0x0B55u, 0x0B57u},   {0x0B5Cu, 0x0B5Du},   // Oriya Vowel Sign E
    {0x0B5Fu, 0x0B63u},   {0x0B66u, 0x0B6Fu},   {0x0B71u, 0x0B77u},   {0x0B82u, 0x0B83u},   // Oriya Letter Yya
    {0x0B85u, 0x0B8Au},   {0x0B8Eu, 0x0B90u},   {0x0B92u, 0x0B95u},   {0x0B99u, 0x0B9Au},   // Tamil Letter A
    {0x0B9Cu, 0x0B9Cu},   {0x0B9Eu, 0x0B9Fu},   {0x0BA3u, 0x0BA4u},   {0x0BA8u, 0x0BAAu},   // Tamil Letter Ja
    {0x0BAEu, 0x0BB9u},   {0x0BBEu, 0x0BC2u},   {0x0BC6u, 0x0BC8u},   {0x0BCAu, 0x0BCDu},   // Tamil Letter Ma
    {0x0BD0u, 0x0BD0u},   {0x0BD7u, 0x0BD7u},   {0x0BE6u, 0x0BF2u},   {0x0C00u, 0x0C0Cu},   // Tamil Om
    {0x0C0Eu, 0x0C10u},   {0x0C12u, 0x0C28u},   {0x0C2Au, 0x0C39u},   {0x0C3Cu, 0x0C44u},   // Telugu Letter E
    {0x0C46u, 0x0C48u},   {0x0C4Au, 0x0C4Du},   {0x0C55u, 0x0C56u},   {0x0C58u, 0x0C5Au},   // Telugu Vowel Sign E
    {0x0C5Du, 0x0C5Du},   {0x0C60u, 0x0C63u},   {0x0C66u, 0x0C6Fu},   {0x0C78u, 0x0C7Eu},   // Telugu Letter Nakaara Pollu
    {0x0C80u, 0x0C83u},   {0x0C85u, 0x0C8Cu},   {0x0C8Eu, 0x0C90u},   {0x0C92u, 0x0CA8u},   // Kannada Sign Spacing Candrabindu
    {0x0CAAu, 0x0CB3u},   {0x0CB5u, 0x0CB9u},   {0x0CBCu, 0x0CC4u},   {0x0CC6u, 0x0CC8u},   // Kannada Letter Pa
    {0x0CCAu, 0x0CCDu},   {0x0CD5u, 0x0CD6u},   {0x0CDDu, 0x0CDEu},   {0x0CE0u, 0x0CE3u},   // Kannada Vowel Sign O
    {0x0CE6u, 0x0CEFu},   {0x0CF1u, 0x0CF2u},   {0x0D00u, 0x0D0Cu},   {0x0D0Eu, 0x0D10u},   // Kannada Digit Zero
    {0x0D12u, 0x0D44u},   {0x0D46u, 0x0D48u},   {0x0D4Au, 0x0D4Eu},   {0x0D54u, 0x0D63u},   // Malayalam Letter O
    {0x0D66u, 0x0D78u},   {0x0D7Au, 0x0D7Fu},   {0x0D81u, 0x0D83u},   {0x0D85u, 0x0D96u},   // Malayalam Digit Zero
    {0x0D9Au, 0x0DB1u},   {0x0DB3u, 0x0DBBu},   {0x0DBDu, 0x0DBDu},   {0x0DC0u, 0x0DC6u},   // Sinhala Letter Alpapraana Kayanna
    {0x0DCAu, 0x0DCAu},   {0x0DCFu, 0x0DD4u},   {0x0DD6u, 0x0DD6u},   {0x0DD8u, 0x0DDFu},   // Sinhala Sign Al-Lakuna
    {0x0DE6u, 0x0DEFu},   {0x0DF2u, 0x0DF3u},   {0x0E01u, 0x0E3Au},   {0x0E40u, 0x0E4Eu},   // Sinhala Lith Digit Zero
    {0x0E50u, 0x0E59u},   {0x0E81u, 0x0E82u},   {0x0E84u, 0x0E84u},   {0x0E86u, 0x0E8Au},   // Thai Digit Zero
    {0x0E8Cu, 0x0EA3u},   {0x0EA5u, 0x0EA5u},   {0x0EA7u, 0x0EBDu},   {0x0EC0u, 0x0EC4u},   // Lao Letter Pali Jha
    {0x0EC6u, 0x0EC6u},   {0x0EC8u, 0x0ECDu},   {0x0ED0u, 0x0ED9u},   {0x0EDCu, 0x0EDFu},   // Lao Ko La
    {0x0F00u, 0x0F00u},   {0x0F18u, 0x0F19u},   {0x0F20u, 0x0F33u},   {0x0F35u, 0x0F35u},   // Tibetan Syllable Om
    {0x0F37u, 0x0F37u},   {0x0F39u, 0x0F39u},   {0x0F3Eu, 0x0F47u},   {0x0F49u, 0x0F6Cu},   // Tibetan Mark Ngas Bzung Sgor Rtags
    {0x0F71u, 0x0F84u},   {0x0F86u, 0x0F97u},   {0x0F99u, 0x0FBCu},   {0x0FC6u, 0x0FC6u},   // Tibetan Vowel Sign Aa
    {0x1000u, 0x1049u},   {0x1050u, 0x109Du},   {0x10A0u, 0x10C5u},   {0x10C7u, 0x10C7u},   // Myanmar Letter Ka
    {0x10CDu, 0x10CDu},   {0x10D0u, 0x10FAu},   {0x10FCu, 0x1248u},   {0x124Au, 0x124Du},   // Georgian Capital Letter Aen
    {0x1250u, 0x1256u},   {0x1258u, 0x1258u},   {0x125Au, 0x125Du},   {0x1260u, 0x1288u},   // Ethiopic Syllable Qha
    {0x128Au, 0x128Du},   {0x1290u, 0x12B0u},   {0x12B2u, 0x12B5u},   {0x12B8u, 0x12BEu},   // Ethiopic Syllable Xwi
    {0x12C0u, 0x12C0u},   {0x12C2u, 0x12C5u},   {0x12C8u, 0x12D6u},   {0x12D8u, 0x1310u},   // Ethiopic Syllable Kxwa
    {0x1312u, 0x1315u},   {0x1318u, 0x135Au},   {0x135Du, 0x135Fu},   {0x1369u, 0x137Cu},   // Ethiopic Syllable Gwi
    {0x1380u, 0x138Fu},   {0x13A0u, 0x13F5u},   {0x13F8u, 0x13FDu},   {0x1401u, 0x166Cu},   // Ethiopic Syllable Sebatbeit Mwa
    {0x166Fu, 0x167Fu},   {0x1681u, 0x169Au},   {0x16A0u, 0x16EAu},   {0x16EEu, 0x16F8u},   // Canadian Syllabics Qai
    {0x1700u, 0x1715u},   {0x171Fu, 0x1734u},   {0x1740u, 0x1753u},   {0x1760u, 0x176Cu},   // Tagalog Letter A
    {0x176Eu, 0x1770u},   {0x1772u, 0x1773u},   {0x1780u, 0x17D3u},   {0x17D7u, 0x17D7u},   // Tagbanwa Letter La
    {0x17DCu, 0x17DDu},   {0x17E0u, 0x17E9u},   {0x17F0u, 0x17F9u},   {0x180Bu, 0x180Du},   // Khmer Sign Avakrahasanya
    {0x180Fu, 0x1819u},   {0x1820u, 0x1878u},   {0x1880u, 0x18AAu},   {0x18B0u, 0x18F5u},   // Mongolian Free Variation Selector Four
    {0x1900u, 0x191Eu},   {0x1920u, 0x192Bu},   {0x1930u, 0x193Bu},   {0x1946u, 0x196Du},   // Limbu Vowel-Carrier Letter
    {0x1970u, 0x1974u},   {0x1980u, 0x19ABu},   {0x19B0u, 0x19C9u},   {0x19D0u, 0x19DAu},   // Tai Le Letter Tone-2
    {0x1A00u, 0x1A1Bu},   {0x1A20u, 0x1A5Eu},   {0x1A60u, 0x1A7Cu},   {0x1A7Fu, 0x1A89u},   // Buginese Letter Ka
    {0x1A90u, 0x1A99u},   {0x1AA7u, 0x1AA7u},   {0x1AB0u, 0x1ACEu},   {0x1B00u, 0x1B4Cu},   // Tai Tham Tham Digit Zero
    {0x1B50u, 0x1B59u},   {0x1B6Bu, 0x1B73u},   {0x1B80u, 0x1BF3u},   {0x1C00u, 0x1C37u},   // Balinese Digit Zero
    {0x1C40u, 0x1C49u},   {0x1C4Du, 0x1C7Du},   {0x1C80u, 0x1C88u},   {0x1C90u, 0x1CBAu},   // Lepcha Digit Zero
    {0x1CBDu, 0x1CBFu},   {0x1CD0u, 0x1CD2u},   {0x1CD4u, 0x1CFAu},   {0x1D00u, 0x1F15u},   // Georgian Mtavruli Capital Letter Aen
    {0x1F18u, 0x1F1Du},   {0x1F20u, 0x1F45u},   {0x1F48u, 0x1F4Du},   {0x1F50u, 0x1F57u},   // Greek Capital Letter Epsilon With Psili
    {0x1F59u, 0x1F59u},   {0x1F5Bu, 0x1F5Bu},   {0x1F5Du, 0x1F5Du},   {0x1F5Fu, 0x1F7Du},   // Greek Capital Letter Upsilon With Dasia
    {0x1F80u, 0x1FB4u},   {0x1FB6u, 0x1FBCu},   {0x1FBEu, 0x1FBEu},   {0x1FC2u, 0x1FC4u},   // Greek Small Letter Alpha With Psili And Ypogegramme
    {0x1FC6u, 0x1FCCu},   {0x1FD0u, 0x1FD3u},   {0x1FD6u, 0x1FDBu},   {0x1FE0u, 0x1FECu},   // Greek Small Letter Eta With Perispomeni
    {0x1FF2u, 0x1FF4u},   {0x1FF6u, 0x1FFCu},   {0x203Fu, 0x2040u},   {0x2054u, 0x2054u},   // Greek Small Letter Omega With Varia And Ypogegramme
    {0x2070u, 0x2071u},   {0x2074u, 0x2079u},   {0x207Fu, 0x2089u},   {0x2090u, 0x209Cu},   // Superscript Zero
    {0x20D0u, 0x20F0u},   {0x2102u, 0x2102u},   {0x2107u, 0x2107u},   {0x210Au, 0x2113u},   // Combining Left Harpoon Above
    {0x2115u, 0x2115u},   {0x2119u, 0x211Du},   {0x2124u, 0x2124u},   {0x2126u, 0x2126u},   // Double-Struck Capital N
    {0x2128u, 0x2128u},   {0x212Au, 0x212Du},   {0x212Fu, 0x2139u},   {0x213Cu, 0x213Fu},   // Black-Letter Capital Z
    {0x2145u, 0x2149u},   {0x214Eu, 0x214Eu},   {0x2150u, 0x2189u},   {0x2460u, 0x249Bu},   // Double-Struck Italic Capital D
    {0x24EAu, 0x24FFu},   {0x2776u, 0x2793u},   {0x2C00u, 0x2CE4u},   {0x2CEBu, 0x2CF3u},   // Circled Digit Zero
    {0x2CFDu, 0x2CFDu},   {0x2D00u, 0x2D25u},   {0x2D27u, 0x2D27u},   {0x2D2Du, 0x2D2Du},   // Coptic Fraction One Half
    {0x2D30u, 0x2D67u},   {0x2D6Fu, 0x2D6Fu},   {0x2D7Fu, 0x2D96u},   {0x2DA0u, 0x2DA6u},   // Tifinagh Letter Ya
    {0x2DA8u, 0x2DAEu},   {0x2DB0u, 0x2DB6u},   {0x2DB8u, 0x2DBEu},   {0x2DC0u, 0x2DC6u},   // Ethiopic Syllable Cca
    {0x2DC8u, 0x2DCEu},   {0x2DD0u, 0x2DD6u},   {0x2DD8u, 0x2DDEu},   {0x2DE0u, 0x2DFFu},   // Ethiopic Syllable Kya
    {0x2E2Fu, 0x2E2Fu},   {0x3005u, 0x3007u},   {0x3021u, 0x302Fu},   {0x3031u, 0x3035u},   // Vertical Tilde
    {0x3038u, 0x303Cu},   {0x3041u, 0x3096u},   {0x3099u, 0x309Au},   {0x309Du, 0x309Fu},   // Hangzhou Numeral Ten
    {0x30A1u, 0x30FAu},   {0x30FCu, 0x30FFu},   {0x3105u, 0x312Fu},   {0x3131u, 0x318Eu},   // Katakana Letter Small A
    {0x3192u, 0x3195u},   {0x31A0u, 0x31BFu},   {0x31F0u, 0x31FFu},   {0x3220u, 0x3229u},   // Ideographic Annotation One Mark
    {0x3248u, 0x324Fu},   {0x3251u, 0x325Fu},   {0x3280u, 0x3289u},   {0x32B1u, 0x32BFu},   // Circled Number Ten On Black Square
    {0x3400u, 0x4DBFu},   {0x4E00u, 0xA48Cu},   {0xA4D0u, 0xA4FDu},   {0xA500u, 0xA60Cu},   // Cjk Unified Ideograph-3400
    {0xA610u, 0xA62Bu},   {0xA640u, 0xA672u},   {0xA674u, 0xA67Du},   {0xA67Fu, 0xA6F1u},   // Vai Syllable Ndole Fa
    {0xA717u, 0xA71Fu},   {0xA722u, 0xA788u},   {0xA78Bu, 0xA7CAu},   {0xA7D0u, 0xA7D1u},   // Modifier Letter Dot Vertical Bar
    {0xA7D3u, 0xA7D3u},   {0xA7D5u, 0xA7D9u},   {0xA7F2u, 0xA827u},   {0xA82Cu, 0xA82Cu},   // Latin Small Letter Double Thorn
    {0xA830u, 0xA835u},   {0xA840u, 0xA873u},   {0xA880u, 0xA8C5u},   {0xA8D0u, 0xA8D9u},   // North Indic Fraction One Quarter
    {0xA8E0u, 0xA8F7u},   {0xA8FBu, 0xA8FBu},   {0xA8FDu, 0xA92Du},   {0xA930u, 0xA953u},   // Combining Devanagari Digit Zero
    {0xA960u, 0xA97Cu},   {0xA980u, 0xA9C0u},   {0xA9CFu, 0xA9D9u},   {0xA9E0u, 0xA9FEu},   // Hangul Choseong Tikeut-Mieum
    {0xAA00u, 0xAA36u},   {0xAA40u, 0xAA4Du},   {0xAA50u, 0xAA59u},   {0xAA60u, 0xAA76u},   // Cham Letter A
    {0xAA7Au, 0xAAC2u},   {0xAADBu, 0xAADDu},   {0xAAE0u, 0xAAEFu},   {0xAAF2u, 0xAAF6u},   // Myanmar Letter Aiton Ra
    {0xAB01u, 0xAB06u},   {0xAB09u, 0xAB0Eu},   {0xAB11u, 0xAB16u},   {0xAB20u, 0xAB26u},   // Ethiopic Syllable Tthu
    {0xAB28u, 0xAB2Eu},   {0xAB30u, 0xAB5Au},   {0xAB5Cu, 0xAB69u},   {0xAB70u, 0xABEAu},   // Ethiopic Syllable Bba
    {0xABECu, 0xABEDu},   {0xABF0u, 0xABF9u},   {0xAC00u, 0xD7A3u},   {0xD7B0u, 0xD7C6u},   // Meetei Mayek Lum Iyek
    {0xD7CBu, 0xD7FBu},   {0xF900u, 0xFA6Du},   {0xFA70u, 0xFAD9u},   {0xFB00u, 0xFB06u},   // Hangul Jongseong Nieun-Rieul
    {0xFB13u, 0xFB17u},   {0xFB1Du, 0xFB28u},   {0xFB2Au, 0xFB36u},   {0xFB38u, 0xFB3Cu},   // Armenian Small Ligature Men Now
    {0xFB3Eu, 0xFB3Eu},   {0xFB40u, 0xFB41u},   {0xFB43u, 0xFB44u},   {0xFB46u, 0xFBB1u},   // Hebrew Letter Mem With Dagesh
    {0xFBD3u, 0xFD3Du},   {0xFD50u, 0xFD8Fu},   {0xFD92u, 0xFDC7u},   {0xFDF0u, 0xFDFBu},   // Arabic Letter Ng Isolated Form
    {0xFE00u, 0xFE0Fu},   {0xFE20u, 0xFE2Fu},   {0xFE33u, 0xFE34u},   {0xFE4Du, 0xFE4Fu},   // Variation Selector-1
    {0xFE70u, 0xFE74u},   {0xFE76u, 0xFEFCu},   {0xFF10u, 0xFF19u},   {0xFF21u, 0xFF3Au},   // Arabic Fathatan Isolated Form
    {0xFF3Fu, 0xFF3Fu},   {0xFF41u, 0xFF5Au},   {0xFF66u, 0xFFBEu},   {0xFFC2u, 0xFFC7u},   // Fullwidth Low Line
    {0xFFCAu, 0xFFCFu},   {0xFFD2u, 0xFFD7u},   {0xFFDAu, 0xFFDCu},   {0x10000u, 0x1000Bu}, // Halfwidth Hangul Letter Yeo
    {0x1000Du, 0x10026u}, {0x10028u, 0x1003Au}, {0x1003Cu, 0x1003Du}, {0x1003Fu, 0x1004Du}, // Linear B Syllable B036 Jo
    {0x10050u, 0x1005Du}, {0x10080u, 0x100FAu}, {0x10107u, 0x10133u}, {0x10140u, 0x10178u}, // Linear B Symbol B018
    {0x1018Au, 0x1018Bu}, {0x101FDu, 0x101FDu}, {0x10280u, 0x1029Cu}, {0x102A0u, 0x102D0u}, // Greek Zero Sign
    {0x102E0u, 0x102FBu}, {0x10300u, 0x10323u}, {0x1032Du, 0x1034Au}, {0x10350u, 0x1037Au}, // Coptic Epact Thousands Mark
    {0x10380u, 0x1039Du}, {0x103A0u, 0x103C3u}, {0x103C8u, 0x103CFu}, {0x103D1u, 0x103D5u}, // Ugaritic Letter Alpa
    {0x10400u, 0x1049Du}, {0x104A0u, 0x104A9u}, {0x104B0u, 0x104D3u}, {0x104D8u, 0x104FBu}, // Deseret Capital Letter Long I
    {0x10500u, 0x10527u}, {0x10530u, 0x10563u}, {0x10570u, 0x1057Au}, {0x1057Cu, 0x1058Au}, // Elbasan Letter A
    {0x1058Cu, 0x10592u}, {0x10594u, 0x10595u}, {0x10597u, 0x105A1u}, {0x105A3u, 0x105B1u}, // Vithkuqi Capital Letter Se
    {0x105B3u, 0x105B9u}, {0x105BBu, 0x105BCu}, {0x10600u, 0x10736u}, {0x10740u, 0x10755u}, // Vithkuqi Small Letter Se
    {0x10760u, 0x10767u}, {0x10780u, 0x10785u}, {0x10787u, 0x107B0u}, {0x107B2u, 0x107BAu}, // Linear A Sign A800
    {0x10800u, 0x10805u}, {0x10808u, 0x10808u}, {0x1080Au, 0x10835u}, {0x10837u, 0x10838u}, // Cypriot Syllable A
    {0x1083Cu, 0x1083Cu}, {0x1083Fu, 0x10855u}, {0x10858u, 0x10876u}, {0x10879u, 0x1089Eu}, // Cypriot Syllable Za
    {0x108A7u, 0x108AFu}, {0x108E0u, 0x108F2u}, {0x108F4u, 0x108F5u}, {0x108FBu, 0x1091Bu}, // Nabataean Number One
    {0x10920u, 0x10939u}, {0x10980u, 0x109B7u}, {0x109BCu, 0x109CFu}, {0x109D2u, 0x10A03u}, // Lydian Letter A
    {0x10A05u, 0x10A06u}, {0x10A0Cu, 0x10A13u}, {0x10A15u, 0x10A17u}, {0x10A19u, 0x10A35u}, // Kharoshthi Vowel Sign E
    {0x10A38u, 0x10A3Au}, {0x10A3Fu, 0x10A48u}, {0x10A60u, 0x10A7Eu}, {0x10A80u, 0x10A9Fu}, // Kharoshthi Sign Bar Above
    {0x10AC0u, 0x10AC7u}, {0x10AC9u, 0x10AE6u}, {0x10AEBu, 0x10AEFu}, {0x10B00u, 0x10B35u}, // Manichaean Letter Aleph
    {0x10B40u, 0x10B55u}, {0x10B58u, 0x10B72u}, {0x10B78u, 0x10B91u}, {0x10BA9u, 0x10BAFu}, // Inscriptional Parthian Letter Aleph
    {0x10C00u, 0x10C48u}, {0x10C80u, 0x10CB2u}, {0x10CC0u, 0x10CF2u}, {0x10CFAu, 0x10D27u}, // Old Turkic Letter Orkhon A
    {0x10D30u, 0x10D39u}, {0x10E60u, 0x10E7Eu}, {0x10E80u, 0x10EA9u}, {0x10EABu, 0x10EACu}, // Hanifi Rohingya Digit Zero
    {0x10EB0u, 0x10EB1u}, {0x10F00u, 0x10F27u}, {0x10F30u, 0x10F54u}, {0x10F70u, 0x10F85u}, // Yezidi Letter Lam With Dot Above
    {0x10FB0u, 0x10FCBu}, {0x10FE0u, 0x10FF6u}, {0x11000u, 0x11046u}, {0x11052u, 0x11075u}, // Chorasmian Letter Aleph
    {0x1107Fu, 0x110BAu}, {0x110C2u, 0x110C2u}, {0x110D0u, 0x110E8u}, {0x110F0u, 0x110F9u}, // Brahmi Number Joiner
    {0x11100u, 0x11134u}, {0x11136u, 0x1113Fu}, {0x11144u, 0x11147u}, {0x11150u, 0x11173u}, // Chakma Sign Candrabindu
    {0x11176u, 0x11176u}, {0x11180u, 0x111C4u}, {0x111C9u, 0x111CCu}, {0x111CEu, 0x111DAu}, // Mahajani Ligature Shri
    {0x111DCu, 0x111DCu}, {0x111E1u, 0x111F4u}, {0x11200u, 0x11211u}, {0x11213u, 0x11237u}, // Sharada Headstroke
    {0x1123Eu, 0x1123Eu}, {0x11280u, 0x11286u}, {0x11288u, 0x11288u}, {0x1128Au, 0x1128Du}, // Khojki Sign Sukun
    {0x1128Fu, 0x1129Du}, {0x1129Fu, 0x112A8u}, {0x112B0u, 0x112EAu}, {0x112F0u, 0x112F9u}, // Multani Letter Nya
    {0x11300u, 0x11303u}, {0x11305u, 0x1130Cu}, {0x1130Fu, 0x11310u}, {0x11313u, 0x11328u}, // Grantha Sign Combining Anusvara Above
    {0x1132Au, 0x11330u}, {0x11332u, 0x11333u}, {0x11335u, 0x11339u}, {0x1133Bu, 0x11344u}, // Grantha Letter Pa
    {0x11347u, 0x11348u}, {0x1134Bu, 0x1134Du}, {0x11350u, 0x11350u}, {0x11357u, 0x11357u}, // Grantha Vowel Sign Ee
    {0x1135Du, 0x11363u}, {0x11366u, 0x1136Cu}, {0x11370u, 0x11374u}, {0x11400u, 0x1144Au}, // Grantha Sign Pluta
    {0x11450u, 0x11459u}, {0x1145Eu, 0x11461u}, {0x11480u, 0x114C5u}, {0x114C7u, 0x114C7u}, // Newa Digit Zero
    {0x114D0u, 0x114D9u}, {0x11580u, 0x115B5u}, {0x115B8u, 0x115C0u}, {0x115D8u, 0x115DDu}, // Tirhuta Digit Zero
    {0x11600u, 0x11640u}, {0x11644u, 0x11644u}, {0x11650u, 0x11659u}, {0x11680u, 0x116B8u}, // Modi Letter A
    {0x116C0u, 0x116C9u}, {0x11700u, 0x1171Au}, {0x1171Du, 0x1172Bu}, {0x11730u, 0x1173Bu}, // Takri Digit Zero
    {0x11740u, 0x11746u}, {0x11800u, 0x1183Au}, {0x118A0u, 0x118F2u}, {0x118FFu, 0x11906u}, // Ahom Letter Ca
    {0x11909u, 0x11909u}, {0x1190Cu, 0x11913u}, {0x11915u, 0x11916u}, {0x11918u, 0x11935u}, // Dives Akuru Letter O
    {0x11937u, 0x11938u}, {0x1193Bu, 0x11943u}, {0x11950u, 0x11959u}, {0x119A0u, 0x119A7u}, // Dives Akuru Vowel Sign Ai
    {0x119AAu, 0x119D7u}, {0x119DAu, 0x119E1u}, {0x119E3u, 0x119E4u}, {0x11A00u, 0x11A3Eu}, // Nandinagari Letter E
    {0x11A47u, 0x11A47u}, {0x11A50u, 0x11A99u}, {0x11A9Du, 0x11A9Du}, {0x11AB0u, 0x11AF8u}, // Zanabazar Square Subjoiner
    {0x11C00u, 0x11C08u}, {0x11C0Au, 0x11C36u}, {0x11C38u, 0x11C40u}, {0x11C50u, 0x11C6Cu}, // Bhaiksuki Letter A
    {0x11C72u, 0x11C8Fu}, {0x11C92u, 0x11CA7u}, {0x11CA9u, 0x11CB6u}, {0x11D00u, 0x11D06u}, // Marchen Letter Ka
    {0x11D08u, 0x11D09u}, {0x11D0Bu, 0x11D36u}, {0x11D3Au, 0x11D3Au}, {0x11D3Cu, 0x11D3Du}, // Masaram Gondi Letter Ai
    {0x11D3Fu, 0x11D47u}, {0x11D50u, 0x11D59u}, {0x11D60u, 0x11D65u}, {0x11D67u, 0x11D68u}, // Masaram Gondi Vowel Sign Au
    {0x11D6Au, 0x11D8Eu}, {0x11D90u, 0x11D91u}, {0x11D93u, 0x11D98u}, {0x11DA0u, 0x11DA9u}, // Gunjala Gondi Letter Oo
    {0x11EE0u, 0x11EF6u}, {0x11FB0u, 0x11FB0u}, {0x11FC0u, 0x11FD4u}, {0x12000u, 0x12399u}, // Makasar Letter Ka
    {0x12400u, 0x1246Eu}, {0x12480u, 0x12543u}, {0x12F90u, 0x12FF0u}, {0x13000u, 0x1342Eu}, // Cuneiform Numeric Sign Two Ash
    {0x14400u, 0x14646u}, {0x16800u, 0x16A38u}, {0x16A40u, 0x16A5Eu}, {0x16A60u, 0x16A69u}, // Anatolian Hieroglyph A001
    {0x16A70u, 0x16ABEu}, {0x16AC0u, 0x16AC9u}, {0x16AD0u, 0x16AEDu}, {0x16AF0u, 0x16AF4u}, // Tangsa Letter Oz
    {0x16B00u, 0x16B36u}, {0x16B40u, 0x16B43u}, {0x16B50u, 0x16B59u}, {0x16B5Bu, 0x16B61u}, // Pahawh Hmong Vowel Keeb
    {0x16B63u, 0x16B77u}, {0x16B7Du, 0x16B8Fu}, {0x16E40u, 0x16E96u}, {0x16F00u, 0x16F4Au}, // Pahawh Hmong Sign Vos Lub
    {0x16F4Fu, 0x16F87u}, {0x16F8Fu, 0x16F9Fu}, {0x16FE0u, 0x16FE1u}, {0x16FE3u, 0x16FE4u}, // Miao Sign Consonant Modifier Bar
    {0x16FF0u, 0x16FF1u}, {0x17000u, 0x187F7u}, {0x18800u, 0x18CD5u}, {0x18D00u, 0x18D08u}, // Vietnamese Alternate Reading Mark Ca
    {0x1AFF0u, 0x1AFF3u}, {0x1AFF5u, 0x1AFFBu}, {0x1AFFDu, 0x1AFFEu}, {0x1B000u, 0x1B122u}, // Katakana Letter Minnan Tone-2
    {0x1B150u, 0x1B152u}, {0x1B164u, 0x1B167u}, {0x1B170u, 0x1B2FBu}, {0x1BC00u, 0x1BC6Au}, // Hiragana Letter Small Wi
    {0x1BC70u, 0x1BC7Cu}, {0x1BC80u, 0x1BC88u}, {0x1BC90u, 0x1BC99u}, {0x1BC9Du, 0x1BC9Eu}, // Duployan Affix Left Horizontal Secant
    {0x1CF00u, 0x1CF2Du}, {0x1CF30u, 0x1CF46u}, {0x1D165u, 0x1D169u}, {0x1D16Du, 0x1D172u}, // Znamenny Combining Mark Gorazdo Nizko S Kryzhem O
    {0x1D17Bu, 0x1D182u}, {0x1D185u, 0x1D18Bu}, {0x1D1AAu, 0x1D1ADu}, {0x1D242u, 0x1D244u}, // Musical Symbol Combining Accent
    {0x1D2E0u, 0x1D2F3u}, {0x1D360u, 0x1D378u}, {0x1D400u, 0x1D454u}, {0x1D456u, 0x1D49Cu}, // Mayan Numeral Zero
    {0x1D49Eu, 0x1D49Fu}, {0x1D4A2u, 0x1D4A2u}, {0x1D4A5u, 0x1D4A6u}, {0x1D4A9u, 0x1D4ACu}, // Mathematical Script Capital C
    {0x1D4AEu, 0x1D4B9u}, {0x1D4BBu, 0x1D4BBu}, {0x1D4BDu, 0x1D4C3u}, {0x1D4C5u, 0x1D505u}, // Mathematical Script Capital S
    {0x1D507u, 0x1D50Au}, {0x1D50Du, 0x1D514u}, {0x1D516u, 0x1D51Cu}, {0x1D51Eu, 0x1D539u}, // Mathematical Fraktur Capital D
    {0x1D53Bu, 0x1D53Eu}, {0x1D540u, 0x1D544u}, {0x1D546u, 0x1D546u}, {0x1D54Au, 0x1D550u}, // Mathematical Double-Struck Capital D
    {0x1D552u, 0x1D6A5u}, {0x1D6A8u, 0x1D6C0u}, {0x1D6C2u, 0x1D6DAu}, {0x1D6DCu, 0x1D6FAu}, // Mathematical Double-Struck Small A
    {0x1D6FCu, 0x1D714u}, {0x1D716u, 0x1D734u}, {0x1D736u, 0x1D74Eu}, {0x1D750u, 0x1D76Eu}, // Mathematical Italic Small Alpha
    {0x1D770u, 0x1D788u}, {0x1D78Au, 0x1D7A8u}, {0x1D7AAu, 0x1D7C2u}, {0x1D7C4u, 0x1D7CBu}, // Mathematical Sans-Serif Bold Small Alpha
    {0x1D7CEu, 0x1D7FFu}, {0x1DA00u, 0x1DA36u}, {0x1DA3Bu, 0x1DA6Cu}, {0x1DA75u, 0x1DA75u}, // Mathematical Bold Digit Zero
    {0x1DA84u, 0x1DA84u}, {0x1DA9Bu, 0x1DA9Fu}, {0x1DAA1u, 0x1DAAFu}, {0x1DF00u, 0x1DF1Eu}, // Signwriting Location Head Neck
    {0x1E000u, 0x1E006u}, {0x1E008u, 0x1E018u}, {0x1E01Bu, 0x1E021u}, {0x1E023u, 0x1E024u}, // Combining Glagolitic Letter Azu
    {0x1E026u, 0x1E02Au}, {0x1E100u, 0x1E12Cu}, {0x1E130u, 0x1E13Du}, {0x1E140u, 0x1E149u}, // Combining Glagolitic Letter Yo
    {0x1E14Eu, 0x1E14Eu}, {0x1E290u, 0x1E2AEu}, {0x1E2C0u, 0x1E2F9u}, {0x1E7E0u, 0x1E7E6u}, // Nyiakeng Puachue Hmong Logogram Nyaj
    {0x1E7E8u, 0x1E7EBu}, {0x1E7EDu, 0x1E7EEu}, {0x1E7F0u, 0x1E7FEu}, {0x1E800u, 0x1E8C4u}, // Ethiopic Syllable Gurage Hhwa
    {0x1E8C7u, 0x1E8D6u}, {0x1E900u, 0x1E94Bu}, {0x1E950u, 0x1E959u}, {0x1EC71u, 0x1ECABu}, // Mende Kikakui Digit One
    {0x1ECADu, 0x1ECAFu}, {0x1ECB1u, 0x1ECB4u}, {0x1ED01u, 0x1ED2Du}, {0x1ED2Fu, 0x1ED3Du}, // Indic Siyaq Fraction One Quarter
    {0x1EE00u, 0x1EE03u}, {0x1EE05u, 0x1EE1Fu}, {0x1EE21u, 0x1EE22u}, {0x1EE24u, 0x1EE24u}, // Arabic Mathematical Alef
    {0x1EE27u, 0x1EE27u}, {0x1EE29u, 0x1EE32u}, {0x1EE34u, 0x1EE37u}, {0x1EE39u, 0x1EE39u}, // Arabic Mathematical Initial Hah
    {0x1EE3Bu, 0x1EE3Bu}, {0x1EE42u, 0x1EE42u}, {0x1EE47u, 0x1EE47u}, {0x1EE49u, 0x1EE49u}, // Arabic Mathematical Initial Ghain
    {0x1EE4Bu, 0x1EE4Bu}, {0x1EE4Du, 0x1EE4Fu}, {0x1EE51u, 0x1EE52u}, {0x1EE54u, 0x1EE54u}, // Arabic Mathematical Tailed Lam
    {0x1EE57u, 0x1EE57u}, {0x1EE59u, 0x1EE59u}, {0x1EE5Bu, 0x1EE5Bu}, {0x1EE5Du, 0x1EE5Du}, // Arabic Mathematical Tailed Khah
    {0x1EE5Fu, 0x1EE5Fu}, {0x1EE61u, 0x1EE62u}, {0x1EE64u, 0x1EE64u}, {0x1EE67u, 0x1EE6Au}, // Arabic Mathematical Tailed Dotless Qaf
    {0x1EE6Cu, 0x1EE72u}, {0x1EE74u, 0x1EE77u}, {0x1EE79u, 0x1EE7Cu}, {0x1EE7Eu, 0x1EE7Eu}, // Arabic Mathematical Stretched Meem
    {0x1EE80u, 0x1EE89u}, {0x1EE8Bu, 0x1EE9Bu}, {0x1EEA1u, 0x1EEA3u}, {0x1EEA5u, 0x1EEA9u}, // Arabic Mathematical Looped Alef
    {0x1EEABu, 0x1EEBBu}, {0x1F100u, 0x1F10Cu}, {0x1FBF0u, 0x1FBF9u}, {0x20000u, 0x2A6DFu}, // Arabic Mathematical Double-Struck Lam
    {0x2A700u, 0x2B738u}, {0x2B740u, 0x2B81Du}, {0x2B820u, 0x2CEA1u}, {0x2CEB0u, 0x2EBE0u}, // Cjk Unified Ideograph-2A700
    {0x2F800u, 0x2FA1Du}, {0x30000u, 0x3134Au}, {0xE0100u, 0xE01EFu}                        // Cjk Compatibility Ideograph-2F800
};

// Every simple one-to-one lower-case mapping in the character database, as runs. GitHub lower-cases with
// the whole of Unicode's case folding, so a Cyrillic or a Greek heading needs this and not only the
// ASCII pair; the few mappings that grow a character into two are left alone, which is To Do 2.
static constexpr LINK_FOLD LINK_SLUG_FOLD[] = {
    {0x0041u, 0x005Au, 1u, 32},     {0x00C0u, 0x00D6u, 1u, 32},     // Latin Capital Letter A
    {0x00D8u, 0x00DEu, 1u, 32},     {0x0100u, 0x012Eu, 2u, 1},      // Latin Capital Letter O With Stroke
    {0x0132u, 0x0136u, 2u, 1},      {0x0139u, 0x0147u, 2u, 1},      // Latin Capital Ligature Ij
    {0x014Au, 0x0176u, 2u, 1},      {0x0178u, 0x0178u, 1u, -121},   // Latin Capital Letter Eng
    {0x0179u, 0x017Du, 2u, 1},      {0x0181u, 0x0181u, 1u, 210},    // Latin Capital Letter Z With Acute
    {0x0182u, 0x0184u, 2u, 1},      {0x0186u, 0x0186u, 1u, 206},    // Latin Capital Letter B With Topbar
    {0x0187u, 0x0187u, 1u, 1},      {0x0189u, 0x018Au, 1u, 205},    // Latin Capital Letter C With Hook
    {0x018Bu, 0x018Bu, 1u, 1},      {0x018Eu, 0x018Eu, 1u, 79},     // Latin Capital Letter D With Topbar
    {0x018Fu, 0x018Fu, 1u, 202},    {0x0190u, 0x0190u, 1u, 203},    // Latin Capital Letter Schwa
    {0x0191u, 0x0191u, 1u, 1},      {0x0193u, 0x0193u, 1u, 205},    // Latin Capital Letter F With Hook
    {0x0194u, 0x0194u, 1u, 207},    {0x0196u, 0x0196u, 1u, 211},    // Latin Capital Letter Gamma
    {0x0197u, 0x0197u, 1u, 209},    {0x0198u, 0x0198u, 1u, 1},      // Latin Capital Letter I With Stroke
    {0x019Cu, 0x019Cu, 1u, 211},    {0x019Du, 0x019Du, 1u, 213},    // Latin Capital Letter Turned M
    {0x019Fu, 0x019Fu, 1u, 214},    {0x01A0u, 0x01A4u, 2u, 1},      // Latin Capital Letter O With Middle Tilde
    {0x01A6u, 0x01A6u, 1u, 218},    {0x01A7u, 0x01A7u, 1u, 1},      // Latin Letter Yr
    {0x01A9u, 0x01A9u, 1u, 218},    {0x01ACu, 0x01ACu, 1u, 1},      // Latin Capital Letter Esh
    {0x01AEu, 0x01AEu, 1u, 218},    {0x01AFu, 0x01AFu, 1u, 1},      // Latin Capital Letter T With Retroflex Hook
    {0x01B1u, 0x01B2u, 1u, 217},    {0x01B3u, 0x01B5u, 2u, 1},      // Latin Capital Letter Upsilon
    {0x01B7u, 0x01B7u, 1u, 219},    {0x01B8u, 0x01B8u, 1u, 1},      // Latin Capital Letter Ezh
    {0x01BCu, 0x01BCu, 1u, 1},      {0x01C4u, 0x01C4u, 1u, 2},      // Latin Capital Letter Tone Five
    {0x01C5u, 0x01C5u, 1u, 1},      {0x01C7u, 0x01C7u, 1u, 2},      // Latin Capital Letter D With Small Letter Z With Caron
    {0x01C8u, 0x01C8u, 1u, 1},      {0x01CAu, 0x01CAu, 1u, 2},      // Latin Capital Letter L With Small Letter J
    {0x01CBu, 0x01DBu, 2u, 1},      {0x01DEu, 0x01EEu, 2u, 1},      // Latin Capital Letter N With Small Letter J
    {0x01F1u, 0x01F1u, 1u, 2},      {0x01F2u, 0x01F4u, 2u, 1},      // Latin Capital Letter Dz
    {0x01F6u, 0x01F6u, 1u, -97},    {0x01F7u, 0x01F7u, 1u, -56},    // Latin Capital Letter Hwair
    {0x01F8u, 0x021Eu, 2u, 1},      {0x0220u, 0x0220u, 1u, -130},   // Latin Capital Letter N With Grave
    {0x0222u, 0x0232u, 2u, 1},      {0x023Au, 0x023Au, 1u, 10795},  // Latin Capital Letter Ou
    {0x023Bu, 0x023Bu, 1u, 1},      {0x023Du, 0x023Du, 1u, -163},   // Latin Capital Letter C With Stroke
    {0x023Eu, 0x023Eu, 1u, 10792},  {0x0241u, 0x0241u, 1u, 1},      // Latin Capital Letter T With Diagonal Stroke
    {0x0243u, 0x0243u, 1u, -195},   {0x0244u, 0x0244u, 1u, 69},     // Latin Capital Letter B With Stroke
    {0x0245u, 0x0245u, 1u, 71},     {0x0246u, 0x024Eu, 2u, 1},      // Latin Capital Letter Turned V
    {0x0370u, 0x0372u, 2u, 1},      {0x0376u, 0x0376u, 1u, 1},      // Greek Capital Letter Heta
    {0x037Fu, 0x037Fu, 1u, 116},    {0x0386u, 0x0386u, 1u, 38},     // Greek Capital Letter Yot
    {0x0388u, 0x038Au, 1u, 37},     {0x038Cu, 0x038Cu, 1u, 64},     // Greek Capital Letter Epsilon With Tonos
    {0x038Eu, 0x038Fu, 1u, 63},     {0x0391u, 0x03A1u, 1u, 32},     // Greek Capital Letter Upsilon With Tonos
    {0x03A3u, 0x03ABu, 1u, 32},     {0x03CFu, 0x03CFu, 1u, 8},      // Greek Capital Letter Sigma
    {0x03D8u, 0x03EEu, 2u, 1},      {0x03F4u, 0x03F4u, 1u, -60},    // Greek Letter Archaic Koppa
    {0x03F7u, 0x03F7u, 1u, 1},      {0x03F9u, 0x03F9u, 1u, -7},     // Greek Capital Letter Sho
    {0x03FAu, 0x03FAu, 1u, 1},      {0x03FDu, 0x03FFu, 1u, -130},   // Greek Capital Letter San
    {0x0400u, 0x040Fu, 1u, 80},     {0x0410u, 0x042Fu, 1u, 32},     // Cyrillic Capital Letter Ie With Grave
    {0x0460u, 0x0480u, 2u, 1},      {0x048Au, 0x04BEu, 2u, 1},      // Cyrillic Capital Letter Omega
    {0x04C0u, 0x04C0u, 1u, 15},     {0x04C1u, 0x04CDu, 2u, 1},      // Cyrillic Letter Palochka
    {0x04D0u, 0x052Eu, 2u, 1},      {0x0531u, 0x0556u, 1u, 48},     // Cyrillic Capital Letter A With Breve
    {0x10A0u, 0x10C5u, 1u, 7264},   {0x10C7u, 0x10C7u, 1u, 7264},   // Georgian Capital Letter An
    {0x10CDu, 0x10CDu, 1u, 7264},   {0x13A0u, 0x13EFu, 1u, 38864},  // Georgian Capital Letter Aen
    {0x13F0u, 0x13F5u, 1u, 8},      {0x1C90u, 0x1CBAu, 1u, -3008},  // Cherokee Letter Ye
    {0x1CBDu, 0x1CBFu, 1u, -3008},  {0x1E00u, 0x1E94u, 2u, 1},      // Georgian Mtavruli Capital Letter Aen
    {0x1E9Eu, 0x1E9Eu, 1u, -7615},  {0x1EA0u, 0x1EFEu, 2u, 1},      // Latin Capital Letter Sharp S
    {0x1F08u, 0x1F0Fu, 1u, -8},     {0x1F18u, 0x1F1Du, 1u, -8},     // Greek Capital Letter Alpha With Psili
    {0x1F28u, 0x1F2Fu, 1u, -8},     {0x1F38u, 0x1F3Fu, 1u, -8},     // Greek Capital Letter Eta With Psili
    {0x1F48u, 0x1F4Du, 1u, -8},     {0x1F59u, 0x1F5Fu, 2u, -8},     // Greek Capital Letter Omicron With Psili
    {0x1F68u, 0x1F6Fu, 1u, -8},     {0x1F88u, 0x1F8Fu, 1u, -8},     // Greek Capital Letter Omega With Psili
    {0x1F98u, 0x1F9Fu, 1u, -8},     {0x1FA8u, 0x1FAFu, 1u, -8},     // Greek Capital Letter Eta With Psili And Prosgegrammeni
    {0x1FB8u, 0x1FB9u, 1u, -8},     {0x1FBAu, 0x1FBBu, 1u, -74},    // Greek Capital Letter Alpha With Vrachy
    {0x1FBCu, 0x1FBCu, 1u, -9},     {0x1FC8u, 0x1FCBu, 1u, -86},    // Greek Capital Letter Alpha With Prosgegrammeni
    {0x1FCCu, 0x1FCCu, 1u, -9},     {0x1FD8u, 0x1FD9u, 1u, -8},     // Greek Capital Letter Eta With Prosgegrammeni
    {0x1FDAu, 0x1FDBu, 1u, -100},   {0x1FE8u, 0x1FE9u, 1u, -8},     // Greek Capital Letter Iota With Varia
    {0x1FEAu, 0x1FEBu, 1u, -112},   {0x1FECu, 0x1FECu, 1u, -7},     // Greek Capital Letter Upsilon With Varia
    {0x1FF8u, 0x1FF9u, 1u, -128},   {0x1FFAu, 0x1FFBu, 1u, -126},   // Greek Capital Letter Omicron With Varia
    {0x1FFCu, 0x1FFCu, 1u, -9},     {0x2126u, 0x2126u, 1u, -7517},  // Greek Capital Letter Omega With Prosgegrammeni
    {0x212Au, 0x212Au, 1u, -8383},  {0x212Bu, 0x212Bu, 1u, -8262},  // Kelvin Sign
    {0x2132u, 0x2132u, 1u, 28},     {0x2160u, 0x216Fu, 1u, 16},     // Turned Capital F
    {0x2183u, 0x2183u, 1u, 1},      {0x24B6u, 0x24CFu, 1u, 26},     // Roman Numeral Reversed One Hundred
    {0x2C00u, 0x2C2Fu, 1u, 48},     {0x2C60u, 0x2C60u, 1u, 1},      // Glagolitic Capital Letter Azu
    {0x2C62u, 0x2C62u, 1u, -10743}, {0x2C63u, 0x2C63u, 1u, -3814},  // Latin Capital Letter L With Middle Tilde
    {0x2C64u, 0x2C64u, 1u, -10727}, {0x2C67u, 0x2C6Bu, 2u, 1},      // Latin Capital Letter R With Tail
    {0x2C6Du, 0x2C6Du, 1u, -10780}, {0x2C6Eu, 0x2C6Eu, 1u, -10749}, // Latin Capital Letter Alpha
    {0x2C6Fu, 0x2C6Fu, 1u, -10783}, {0x2C70u, 0x2C70u, 1u, -10782}, // Latin Capital Letter Turned A
    {0x2C72u, 0x2C72u, 1u, 1},      {0x2C75u, 0x2C75u, 1u, 1},      // Latin Capital Letter W With Hook
    {0x2C7Eu, 0x2C7Fu, 1u, -10815}, {0x2C80u, 0x2CE2u, 2u, 1},      // Latin Capital Letter S With Swash Tail
    {0x2CEBu, 0x2CEDu, 2u, 1},      {0x2CF2u, 0x2CF2u, 1u, 1},      // Coptic Capital Letter Cryptogrammic Shei
    {0xA640u, 0xA66Cu, 2u, 1},      {0xA680u, 0xA69Au, 2u, 1},      // Cyrillic Capital Letter Zemlya
    {0xA722u, 0xA72Eu, 2u, 1},      {0xA732u, 0xA76Eu, 2u, 1},      // Latin Capital Letter Egyptological Alef
    {0xA779u, 0xA77Bu, 2u, 1},      {0xA77Du, 0xA77Du, 1u, -35332}, // Latin Capital Letter Insular D
    {0xA77Eu, 0xA786u, 2u, 1},      {0xA78Bu, 0xA78Bu, 1u, 1},      // Latin Capital Letter Turned Insular G
    {0xA78Du, 0xA78Du, 1u, -42280}, {0xA790u, 0xA792u, 2u, 1},      // Latin Capital Letter Turned H
    {0xA796u, 0xA7A8u, 2u, 1},      {0xA7AAu, 0xA7AAu, 1u, -42308}, // Latin Capital Letter B With Flourish
    {0xA7ABu, 0xA7ABu, 1u, -42319}, {0xA7ACu, 0xA7ACu, 1u, -42315}, // Latin Capital Letter Reversed Open E
    {0xA7ADu, 0xA7ADu, 1u, -42305}, {0xA7AEu, 0xA7AEu, 1u, -42308}, // Latin Capital Letter L With Belt
    {0xA7B0u, 0xA7B0u, 1u, -42258}, {0xA7B1u, 0xA7B1u, 1u, -42282}, // Latin Capital Letter Turned K
    {0xA7B2u, 0xA7B2u, 1u, -42261}, {0xA7B3u, 0xA7B3u, 1u, 928},    // Latin Capital Letter J With Crossed-Tail
    {0xA7B4u, 0xA7C2u, 2u, 1},      {0xA7C4u, 0xA7C4u, 1u, -48},    // Latin Capital Letter Beta
    {0xA7C5u, 0xA7C5u, 1u, -42307}, {0xA7C6u, 0xA7C6u, 1u, -35384}, // Latin Capital Letter S With Hook
    {0xA7C7u, 0xA7C9u, 2u, 1},      {0xA7D0u, 0xA7D0u, 1u, 1},      // Latin Capital Letter D With Short Stroke Overlay
    {0xA7D6u, 0xA7D8u, 2u, 1},      {0xA7F5u, 0xA7F5u, 1u, 1},      // Latin Capital Letter Middle Scots S
    {0xFF21u, 0xFF3Au, 1u, 32},     {0x10400u, 0x10427u, 1u, 40},   // Fullwidth Latin Capital Letter A
    {0x104B0u, 0x104D3u, 1u, 40},   {0x10570u, 0x1057Au, 1u, 39},   // Osage Capital Letter A
    {0x1057Cu, 0x1058Au, 1u, 39},   {0x1058Cu, 0x10592u, 1u, 39},   // Vithkuqi Capital Letter Ha
    {0x10594u, 0x10595u, 1u, 39},   {0x10C80u, 0x10CB2u, 1u, 64},   // Vithkuqi Capital Letter Y
    {0x118A0u, 0x118BFu, 1u, 32},   {0x16E40u, 0x16E5Fu, 1u, 32},   // Warang Citi Capital Letter Ngaa
    {0x1E900u, 0x1E921u, 1u, 34}                                    // Adlam Capital Letter Alif
};

// Two anchors from the character database, so a table rebuilt from a different Unicode version cannot
// quietly move the rows every ASCII slug depends on.
static_assert(LINK_SLUG_KEEP[0].first == 0x0030u && LINK_SLUG_KEEP[0].last == 0x0039u, // The ASCII digits
              "LinkResolver: the slug table must begin at the ASCII digits.");
static_assert(LINK_SLUG_FOLD[0].first == 0x0041u && LINK_SLUG_FOLD[0].delta == 32, "LinkResolver: the fold table must begin at the ASCII capitals.");

//-- The slugger

// Whether a code point survives into a slug.
static cbool LinkSlugKeeps(cui32 point) {
   ui64 low  = 0;
   ui64 high = sizeof(LINK_SLUG_KEEP) / sizeof(LINK_SLUG_KEEP[0]);

   while(low < high) {
      cui64 middle = low + (high - low) / 2u;

      if(point < LINK_SLUG_KEEP[middle].first) high = middle;
      else if(point > LINK_SLUG_KEEP[middle].last) low = middle + 1u;
      else return true;
   }
   return false;
}

// One code point folded to lower case, or itself where the database gives it no simple mapping.
static cui32 LinkSlugFold(cui32 point) {
   ui64 low  = 0;
   ui64 high = sizeof(LINK_SLUG_FOLD) / sizeof(LINK_SLUG_FOLD[0]);

   while(low < high) {
      cui64 middle = low + (high - low) / 2u;

      if(point < LINK_SLUG_FOLD[middle].first) high = middle;
      else if(point > LINK_SLUG_FOLD[middle].last) low = middle + 1u;
      else {
         cLINK_FOLD row = LINK_SLUG_FOLD[middle];

         // A run of step 2 holds only every other code point: the ones between are the lower-case forms
         // already, and folding those by the same delta would push them past the letter they belong to.
         if(row.step == 2u && ((point - row.first) & 1u)) return point;
         return ui32(si64(point) + row.delta);
      }
   }
   return point;
}

// Appends one code point to a buffer as UTF-8, and reports whether it fitted.
static cbool LinkPutPoint(chptrc dest, cui64 destBytes, ui64ptrc used, cui32 point) {
   ui8   encoded[4];
   cui64 width = UtfEncode(point, encoded);

   if(!width || *used + width + 1u > destBytes) return false;
   for(ui64 index = 0; index < width; ++index) dest[*used + index] = char(encoded[index]);
   *used += width;
   return true;
}

// Slugs one run of text onto the end of a buffer, and reports whether all of it fitted.
//
// The transformation is per character and carries no state at all, which is what lets a heading be
// slugged span by span rather than assembled first: lower-casing and the keep test each answer about one
// code point, and github-slugger's own rule is exactly those two in that order.
static cbool LinkSlugAppend(cchptr text, cui64 byteCount, chptrc dest, cui64 destBytes, ui64ptrc used) {
   ui64 at = 0;

   while(at < byteCount) {
      ui32  point = 0;
      cui64 width = UtfDecode((cui8ptr)text + at, byteCount - at, &point);

      if(!width) return false; // Not well-formed UTF-8, which OpcLoadXmlPart has already ruled out
      at += width;
      // A space becomes a hyphen and a hyphen stays one; everything else is folded and then kept or
      // dropped by its category, in that order, because that is the order github-slugger uses.
      if(point == ' ') {
         point = '-';
      } else {
         point = LinkSlugFold(point);
         if(point != '-' && !LinkSlugKeeps(point)) continue;
      }
      if(!LinkPutPoint(dest, destBytes, used, point)) return false;
   }
   return true;
}

//-- The name index

// One name the index holds, and what the document does with it. Two passes fill it: the links mark the
// names something points at, and the anchors record which of them the document actually defines.
struct LINK_ENTRY {
   ui32 nameAt;     ///< Destination-arena offset of the name
   ui32 nameBytes;  ///< How many bytes it is
   si32 value;      ///< The span of the first anchor with this name, or -1 while none has been seen
   bool referenced; ///< Whether a link names it
   bool used;       ///< Whether this slot holds an entry at all
};

typedef LINK_ENTRY       *LINK_ENTRYptr;
typedef const LINK_ENTRY *cLINK_ENTRYptr;

// One open-addressed index over names that live in a document's destination arena.
//
// It is an index rather than a scan for the reason M5's review found the hard way in StyleModel: a
// document may carry tens of thousands of bookmarks and as many references to them, and pairing the two
// by scanning is quadratic in a way no fixture notices and no large document survives. The document is
// held rather than the arena's address because IrSetDest may grow the arena between passes, which moves
// it; the offsets stay put, so re-reading the base each time is the whole of the fix.
struct LINK_INDEX {
   LINK_ENTRYptr   slots;    ///< Power-of-two table, zeroed on allocation
   cIR_DOCUMENTptr document; ///< Where the names live
   ui64            mask;     ///< One less than the slot count
};

typedef LINK_INDEX       *LINK_INDEXptr;
typedef const LINK_INDEX *cLINK_INDEXptr;
typedef LINK_INDEX *const LINK_INDEXptrc;

// The FNV-1a hash of a name, which is what spreads the slots.
static cui64 LinkHash(cchptr bytes, cui64 byteCount) {
   ui64 hash = 0xCBF29CE484222325ull;

   for(ui64 index = 0; index < byteCount; ++index) {
      hash ^= ui8(bytes[index]);
      hash *= 0x100000001B3ull;
   }
   return hash;
}

// Whether the name a slot holds is the one being looked for.
static cbool LinkSameName(cLINK_INDEXptr index, cLINK_ENTRYptr entry, cchptr name, cui32 nameBytes) {
   cchptr held = IrDest(index->document, entry->nameAt);

   if(entry->nameBytes != nameBytes) return false;
   for(ui32 at = 0; at < nameBytes; ++at) {
      if(held[at] != name[at]) return false;
   }
   return true;
}

// Finds a name in the index, or the free slot it would go in. Never null: the table is sized for every
// name the document can offer it and never grows, so an insertion always has somewhere to go.
static LINK_ENTRYptr LinkIndexSlot(LINK_INDEXptrc index, cui32 nameAt, cui32 nameBytes) {
   cchptr name = IrDest(index->document, nameAt);
   ui64   slot = LinkHash(name, nameBytes) & index->mask;

   for(;;) {
      LINK_ENTRYptr entry = index->slots + slot;

      if(!entry->used || LinkSameName(index, entry, name, nameBytes)) return entry;
      slot = (slot + 1u) & index->mask;
   }
}

// The same, filling a free slot in rather than leaving it empty.
static LINK_ENTRYptr LinkIndexFind(LINK_INDEXptrc index, cui32 nameAt, cui32 nameBytes) {
   LINK_ENTRYptr entry = LinkIndexSlot(index, nameAt, nameBytes);

   if(!entry->used) {
      entry->nameAt    = nameAt;
      entry->nameBytes = nameBytes;
      entry->value     = -1;
      entry->used      = true;
   }
   return entry;
}

// The power of two that holds a count with room to spare, so probing stays short.
static cui64 LinkIndexSlots(cui64 names) {
   ui64 slots = 16u;

   while(slots < names * 2u + 2u) slots *= 2u;
   return slots;
}

// Allocates a zeroed index for a given number of names, or reports that it could not.
static cbool LinkIndexOpen(LINK_INDEXptrc index, cIR_DOCUMENTptr document, cui64 names) {
   cui64 slots = LinkIndexSlots(names);
   cui64 bytes = slots * sizeof(LINK_ENTRY);

   index->slots    = (LINK_ENTRYptr)amalloc(bytes, 32u);
   index->document = document;
   index->mask     = slots - 1u;
   if(!index->slots) return false;
   mzero(index->slots, bytes);
   return true;
}

// Releases both indexes and reports the failure that brought the caller here.
static cbool LinkFail(LINK_INDEXptrc byName, LINK_INDEXptrc bySlug) {
   mdealloc(byName->slots);
   mdealloc(bySlug->slots);
   return false;
}

//-- Reference resolution

// The length of a NUL-terminated string, bounded so a malformed one cannot run away.
static cui64 LinkLength(cchptr text, cui64 limit) {
   ui64 length = 0;

   while(length < limit && text[length]) ++length;
   return length;
}

// Appends a range to a buffer that is being built, and reports whether it fitted.
static cbool LinkAppend(chptrc dest, cui64 destBytes, ui64ptrc used, cchptr bytes, cui64 byteCount) {
   if(*used + byteCount > destBytes) return false;
   for(ui64 index = 0; index < byteCount; ++index) dest[*used + index] = bytes[index];
   *used += byteCount;
   return true;
}

// Resolves one span's recorded reference into a destination, and reports whether the document survived.
//
// A reference that names nothing leaves an empty destination behind, which every later stage reads as
// "no link": the text stays and the brackets go, which is what CONVERSION_REFERENCE 5.4's "dangling refs
// degrade gracefully" means applied to a reference rather than to numbering.
static cbool LinkResolveOne(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, csi32 partIndex, cui32 spanIndex) {
   IR_SPANptr span = IrSpanMutable(document, spanIndex);

   if(!span) return true;

   cchptr recorded      = IrDest(document, span->destAt);
   cui32  recordedBytes = span->destBytes;
   ui32   idBytes       = 0;

   // The walk joins a hyperlink's r:id and its w:anchor with the '#' that will separate them in the
   // output. A relationship id is an XML name and cannot hold one, so the first '#' is the seam.
   while(idBytes < recordedBytes && recorded[idBytes] != '#') ++idBytes;

   char  reference[LINK_MAX_REF_BYTES];
   cui32 fragmentAt    = span->destAt + idBytes;
   cui32 fragmentBytes = recordedBytes - idBytes;

   span->flags = IR_SPAN_FLAG_NONE;
   if(!idBytes || idBytes >= LINK_MAX_REF_BYTES) return IrSetDest(document, spanIndex, "", 0);
   for(ui32 at = 0; at < idBytes; ++at) reference[at] = recorded[at];
   reference[idBytes] = 0;

   csi32 relation = OpcFindRelById(package, partIndex, reference);

   if(relation < 0) return IrSetDest(document, spanIndex, "", 0);

   cOPC_REL_VIEW record = OpcRel(package, relation);
   char          built[LINK_MAX_DEST_BYTES];
   ui64          used = 0;

   if(record.external) {
      if(!LinkAppend(built, sizeof(built), &used, record.target, LinkLength(record.target, LINK_MAX_DEST_BYTES))) {
         return IrSetDest(document, spanIndex, "", 0);
      }
      // The fragment is read back out of the arena rather than kept as a pointer: IrSetDest may have
      // grown it, and an offset survives that where an address does not.
      if(fragmentBytes) LinkAppend(built, sizeof(built), &used, IrDest(document, fragmentAt), fragmentBytes);
      return IrSetDest(document, spanIndex, built, used);
   }
   // An internal target is a part of the package. An image is drawn from one, and MediaPlan turns the
   // name into a file beside the .md; a hyperlink to one has nowhere to point, because Markdown can
   // address a file and not a part, so it keeps its text and loses its brackets.
   if(span->kind != IR_SPAN_IMAGE || !record.part || !record.part[0]) return IrSetDest(document, spanIndex, "", 0);
   if(!LinkAppend(built, sizeof(built), &used, record.part, LinkLength(record.part, LINK_MAX_DEST_BYTES))) {
      return IrSetDest(document, spanIndex, "", 0);
   }
   if(!IrSetDest(document, spanIndex, built, used)) return false;
   span = IrSpanMutable(document, spanIndex);
   if(span) span->flags = IR_SPAN_FLAG_PART;
   return true;
}

//-- Anchors and slugs

// Whether a span is a link whose destination is a fragment inside this document.
//
// A resolved external target never begins with '#', because a URI begins with its scheme; one that does
// is a reference into the document itself, whether the producer wrote it as a w:anchor or as a
// relationship target of "#name". Both mean the same thing and both resolve here.
static cbool LinkIsInternal(cIR_SPANptr span, cIR_DOCUMENTptr document) {
   if(span->kind != IR_SPAN_LINK_START || (span->flags & IR_SPAN_FLAG_MUTE) || span->destBytes < 2u) return false;
   return IrDest(document, span->destAt)[0] == '#';
}

// Mutes one span, so that it emits nothing at all.
static void LinkMute(IR_DOCUMENTptrc document, cui32 spanIndex) {
   IR_SPANptr span = IrSpanMutable(document, spanIndex);

   if(span) span->flags |= IR_SPAN_FLAG_MUTE;
}

// Mutes both halves of every link that would emit brackets around nothing.
//
// Two links come to nothing and they arrive here the same way. One whose destination resolved to
// nothing -- a dangling relationship, a target inside the package, a bookmark the document does not
// define -- keeps its text and loses its brackets, which is the graceful degradation. One whose
// *content* is empty is CONVERSION_REFERENCE 5.6's skipped hyperlink: "[](url)" is a link a reader
// cannot click. Deciding both here rather than in the emitter is what lets the emitter simply skip a
// muted span, and it is what lets its flanking lookahead know whether a bracket is coming.
static void LinkMuteEmptyLinks(IR_DOCUMENTptrc document) {
   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;

      si64 opened = -1;

      for(ui32 at = 0; at < block->spanCount; ++at) {
         cui32       spanIndex = block->spanAt + at;
         cIR_SPANptr span      = IrSpanAt(document, spanIndex);

         if(!span) continue;
         if(span->kind == IR_SPAN_LINK_START) {
            opened = si64(spanIndex);
            continue;
         }
         if(span->kind != IR_SPAN_LINK_END || opened < 0) continue;

         cIR_SPANptr start = IrSpanAt(document, ui32(opened));

         if(!start->destBytes || !IrHasInk(document, ui32(opened) + 1u, spanIndex)) {
            LinkMute(document, ui32(opened));
            LinkMute(document, spanIndex);
         }
         opened = -1;
      }
      // A start with no end is a walk that stopped early; it emits nothing rather than a stray bracket.
      if(opened >= 0) LinkMute(document, ui32(opened));
   }
}

// Builds the slug of one heading, deduplicated against every heading before it, and stores it in the
// document's destination arena.
//
// The numbering is github-slugger's own and it is a loop rather than a counter, because a heading may
// literally be called "Introduction 1" and collide with the "-1" a second "Introduction" would take.
static csi64 LinkHeadingSlug(IR_DOCUMENTptrc document, cIR_BLOCKptr block, LINK_INDEXptrc slugs, ui32ptrc slugBytes) {
   char text[LINK_MAX_NAME_BYTES];
   ui64 used = 0;

   for(ui32 index = 0; index < block->spanCount; ++index) {
      cIR_SPANptr span = IrSpanAt(document, block->spanAt + index);

      if(!span || span->kind != IR_SPAN_TEXT) continue;
      if(!LinkSlugAppend(IrText(document, span->textAt), span->textBytes, text, sizeof(text), &used)) break;
   }
   *slugBytes = 0;
   if(!used) return -1;

   csi64 stored = IrStoreDest(document, text, used);

   if(stored < 0) return -1;

   LINK_ENTRYptr entry = LinkIndexFind(slugs, ui32(stored), ui32(used));

   if(entry->value < 0) {
      entry->value = 0;
      *slugBytes   = ui32(used);
      return stored;
   }
   // The slug is taken, so the original's counter advances until "<slug>-<n>" is free. Every result is
   // recorded, which is what stops two different headings arriving at the same numbered form.
   for(;;) {
      char candidate[LINK_MAX_NAME_BYTES];
      char digits[16];
      ui64 length     = 0;
      ui64 digitCount = 0;
      ui32 number     = ui32(++entry->value);

      while(number) {
         digits[digitCount++] = char('0' + (number % 10u));
         number /= 10u;
      }
      if(!LinkAppend(candidate, sizeof(candidate), &length, text, used)) return -1;
      if(!LinkAppend(candidate, sizeof(candidate), &length, "-", 1u)) return -1;
      while(digitCount) candidate[length++] = digits[--digitCount];

      csi64 place = IrStoreDest(document, candidate, length);

      if(place < 0) return -1;

      LINK_ENTRYptr taken = LinkIndexFind(slugs, ui32(place), ui32(length));

      if(taken->value >= 0) continue;
      taken->value = 0;
      *slugBytes   = ui32(length);
      return place;
   }
}

//== Entry points

cbool LinkResolveRefs(IR_DOCUMENTptrc document, OPC_PACKAGEptrc package, csi32 partIndex) {
   for(ui32 index = 0; index < IrSpanCount(document); ++index) {
      cIR_SPANptr span = IrSpanAt(document, index);

      if(!span || !(span->flags & IR_SPAN_FLAG_REL)) continue;
      // No package is the unit suite's case, where a body is walked out of a string literal. There is
      // nothing to look an id up in, so every reference resolves to nothing -- the same degradation a
      // dangling one gets, which is what keeps the pipeline's shape the same with and without a package.
      if(!package || partIndex < 0) {
         IR_SPANptr blank = IrSpanMutable(document, index);

         if(blank) blank->flags = IR_SPAN_FLAG_NONE;
         if(!IrSetDest(document, index, "", 0)) return false;
         continue;
      }
      if(!LinkResolveOne(document, package, partIndex, index)) return false;
   }
   return true;
}

cbool LinkResolveAnchors(IR_DOCUMENTptrc document) {
   ui64 names    = 0;
   ui64 headings = 0;

   // Muted before anything is counted, so that a link with no content and one whose reference came to
   // nothing are both out of the way before an anchor is asked whether something points at it.
   LinkMuteEmptyLinks(document);
   for(ui32 index = 0; index < IrSpanCount(document); ++index) {
      cIR_SPANptr span = IrSpanAt(document, index);

      if(span && (span->kind == IR_SPAN_ANCHOR || LinkIsInternal(span, document))) ++names;
   }
   // No anchor and no internal link means nothing to resolve and no slug worth computing: a heading's
   // slug is the renderer's to generate, and this pass only ever needs one to point a link at.
   if(!names) return true;
   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(block && block->kind == IR_BLOCK_HEADING) ++headings;
   }

   LINK_INDEX byName;
   LINK_INDEX bySlug;

   if(!LinkIndexOpen(&byName, document, names)) return false;
   if(!LinkIndexOpen(&bySlug, document, headings)) {
      mdealloc(byName.slots);
      return false;
   }
   // First pass: every name the document defines, and every name something points at.
   for(ui32 index = 0; index < IrSpanCount(document); ++index) {
      cIR_SPANptr span = IrSpanAt(document, index);

      if(!span) continue;
      if(span->kind == IR_SPAN_ANCHOR) {
         LINK_ENTRYptr entry = LinkIndexFind(&byName, span->destAt, span->destBytes);

         // The first bookmark of a name wins, which is the same rule ZipReader applies to a duplicate
         // entry name and for the same reason: it is deterministic, and a later one cannot override it.
         if(entry->value < 0) entry->value = si32(index);
         continue;
      }
      if(LinkIsInternal(span, document)) LinkIndexFind(&byName, span->destAt + 1u, span->destBytes - 1u)->referenced = true;
   }
   // Second pass: what each anchor resolves to, in block order, because a heading's slug is numbered
   // against every heading before it and the numbering is what a renderer will reproduce.
   for(ui32 index = 0; index < IrBlockCount(document); ++index) {
      cIR_BLOCKptr block = IrBlockAt(document, index);

      if(!block) continue;

      ui32  slugBytes = 0;
      csi64 slugAt    = (block->kind == IR_BLOCK_HEADING ? LinkHeadingSlug(document, block, &bySlug, &slugBytes) : -1);

      for(ui32 at = 0; at < block->spanCount; ++at) {
         cui32       spanIndex = block->spanAt + at;
         cIR_SPANptr span      = IrSpanAt(document, spanIndex);

         if(!span || span->kind != IR_SPAN_ANCHOR) continue;

         LINK_ENTRYptr entry = LinkIndexFind(&byName, span->destAt, span->destBytes);

         if(!entry->referenced || entry->value != si32(spanIndex)) {
            LinkMute(document, spanIndex);
            continue;
         }
         // A bookmark inside a heading needs no markup of its own: the heading already carries the
         // anchor the renderer generates, so the span resolves to that slug and then emits nothing.
         if(slugAt >= 0 && slugBytes) {
            if(!IrSetDest(document, spanIndex, IrDest(document, ui32(slugAt)), slugBytes)) return LinkFail(&byName, &bySlug);
            LinkMute(document, spanIndex);
            continue;
         }

         char  safe[LINK_MAX_NAME_BYTES];
         cui64 length = LinkAnchorName(IrDest(document, span->destAt), span->destBytes, safe, sizeof(safe));

         if(!IrSetDest(document, spanIndex, safe, length)) return LinkFail(&byName, &bySlug);
         // A name that sanitises to nothing cannot be an anchor at all, so it emits nothing and the
         // links that named it lose their brackets rather than pointing at "#".
         if(!length) LinkMute(document, spanIndex);
      }
   }
   // Third pass: every internal link, rewritten from the name it was given to the anchor it reaches.
   for(ui32 index = 0; index < IrSpanCount(document); ++index) {
      cIR_SPANptr span = IrSpanAt(document, index);

      if(!span || !LinkIsInternal(span, document)) continue;

      LINK_ENTRYptr entry  = LinkIndexFind(&byName, span->destAt + 1u, span->destBytes - 1u);
      cIR_SPANptr   anchor = (entry->value < 0 ? nullptr : IrSpanAt(document, ui32(entry->value)));

      if(!anchor || !anchor->destBytes) {
         if(!IrSetDest(document, index, "", 0)) return LinkFail(&byName, &bySlug);
         continue;
      }

      char built[LINK_MAX_NAME_BYTES + 1u];
      ui64 used = 0;

      cchptr target = IrDest(document, anchor->destAt);

      if(!LinkAppend(built, sizeof(built), &used, "#", 1u) || !LinkAppend(built, sizeof(built), &used, target, anchor->destBytes)) {
         used = 0;
      }
      if(!IrSetDest(document, index, built, used)) return LinkFail(&byName, &bySlug);
   }
   // Muted a second time, because the third pass is itself a way for a link to lose its destination:
   // one naming a bookmark the document does not define resolves to nothing, and a link with nothing
   // to point at keeps its text and loses its brackets exactly as a dangling relationship does.
   LinkMuteEmptyLinks(document);
   mdealloc(byName.slots);
   mdealloc(bySlug.slots);
   return true;
}

cui64 LinkSlug(cchptr text, cui64 byteCount, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   if(!destBytes) return 0;
   dest[0] = 0;
   if(text) LinkSlugAppend(text, byteCount, dest, destBytes, &used);
   dest[used] = 0;
   return used;
}

cui64 LinkAnchorName(cchptr name, cui64 byteCount, chptrc dest, cui64 destBytes) {
   ui64 used = 0;

   if(!destBytes) return 0;
   dest[0] = 0;
   if(!name) return 0;
   for(ui64 index = 0; index < byteCount && used + 1u < destBytes; ++index) {
      cchar byte         = name[index];
      cbool alphanumeric = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
      cbool safe         = alphanumeric || byte == '-' || byte == '_' || byte == '.';

      dest[used++] = (safe ? byte : '-');
   }
   dest[used] = 0;
   return used;
}
