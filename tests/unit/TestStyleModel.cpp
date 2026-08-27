/*
 * File: TestStyleModel.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-26
 * Description: Unit tests for name normalization, role detection, basedOn folding and the toggle XOR.
 * To Do: 1) Drive a numbering-bearing style chain once M8 gives w:numPr somewhere to be read into.
 *        2) Add a case per producer from CONVERSION_REFERENCE 5.10 as real exports are collected at M11.
 * Dependencies: BuildGuards.h, Check.h, StyleModel.h, typedefs.h, stdio.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include <stdio.h>
#include "typedefs.h"
#include "Check.h"
#include "StyleModel.h"

//-- Helpers

// The opening of every styles part below, so a case is only the styles it is actually about.
static constexpr cchptr STYLE_HEAD = "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">";
static constexpr cchptr STYLE_TAIL = "</w:styles>";

// Bytes before the terminator.
static cui64 TestLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// Loads a styles part built from a literal body, wrapped in the root element every part needs.
static cSTYLE_RESULT LoadStyles(STYLE_MODELptrc model, cchptr body) {
   char   part[4096];
   ui64   used      = 0;
   cchptr pieces[3] = {STYLE_HEAD, body, STYLE_TAIL};

   for(ui32 index = 0; index < 3u; ++index) {
      cui64 length = TestLength(pieces[index]);

      for(ui64 at = 0; at < length && used + 1u < sizeof(part); ++at) part[used++] = pieces[index][at];
   }
   part[used] = 0;
   StyleOpen(model);
   return StyleLoadBytes(model, (cui8ptr)part, used);
}

// Whether a NUL-terminated string is exactly a literal.
static cbool StyleTextIs(cchptr text, cchptr wanted) {
   ui64 index = 0;

   while(text[index] && text[index] == wanted[index]) ++index;
   return text[index] == wanted[index];
}

// Whether one toggle is on in a resolved run.
static cbool ToggleOn(cSTYLE_RUN_PROPS props, cSTYLE_TOGGLE toggle) { return (props.toggles & StyleToggleBit(toggle)) != 0; }

// Resolves a run with no direct formatting under one paragraph style.
static cSTYLE_RUN_PROPS ResolveUnder(cSTYLE_MODELptr model, cchptr paragraphStyle) {
   STYLE_DIRECT_RUN direct;

   StyleClearDirect(&direct);
   return StyleResolveRun(model, StyleFind(model, paragraphStyle), &direct);
}

// The normalized form of a literal, for the name tests.
static cbool NormalizesTo(cchptr text, cchptr wanted) {
   char produced[128];
   ui64 index = 0;

   StyleNormalizeName(text, TestLength(text), produced, sizeof(produced));
   while(produced[index] && produced[index] == wanted[index]) ++index;
   return produced[index] == wanted[index];
}

// The role and level a literal name resolves to when a paragraph style declares it.
static cbool RoleIs(cchptr normalized, cSTYLE_ROLE wantedRole, cui8 wantedLevel) {
   ui8 level = 0;

   return StyleRoleOfName(normalized, STYLE_TYPE_PARAGRAPH, &level) == wantedRole && level == wantedLevel;
}

// The role a literal name resolves to when a *character* style declares it, which reads a different
// table: only CONVERSION_REFERENCE row 11's inline-code names mean anything there.
static cbool CharRoleIs(cchptr normalized, cSTYLE_ROLE wantedRole) {
   ui8 level = 0;

   return StyleRoleOfName(normalized, STYLE_TYPE_CHARACTER, &level) == wantedRole && level == 0;
}

//== The suite

void TestStyleModel(void);

void TestStyleModel(void) {
   CheckGroup("StyleModel: name normalization");
   CHECK(NormalizesTo("Heading 1", "heading 1"));
   CHECK(NormalizesTo("HEADING   1", "heading 1"));
   CHECK(NormalizesTo("  Heading\t1  ", "heading 1"));
   CHECK(NormalizesTo("Heading1", "heading1"));
   CHECK(NormalizesTo("Source_20_Text", "source text"));
   CHECK(NormalizesTo("Heading_20_4", "heading 4"));
   CHECK(NormalizesTo("_20_leading", "leading"));
   CHECK(NormalizesTo("trailing_20_", "trailing"));
   CHECK(NormalizesTo("", ""));
   CHECK(NormalizesTo("a_21_b", "a_21_b"));

   CheckGroup("StyleModel: role detection");
   CHECK(RoleIs("heading 1", STYLE_ROLE_HEADING, 1u));
   CHECK(RoleIs("heading 9", STYLE_ROLE_HEADING, 9u));
   CHECK(RoleIs("heading1", STYLE_ROLE_HEADING, 1u));
   CHECK(RoleIs("heading4", STYLE_ROLE_HEADING, 4u));
   CHECK(RoleIs("heading 0", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("heading 10", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("heading", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("headingx1", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("subheading 1", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("title", STYLE_ROLE_TITLE, 0u));
   CHECK(RoleIs("subtitle", STYLE_ROLE_SUBTITLE, 0u));
   CHECK(RoleIs("normal", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("", STYLE_ROLE_NORMAL, 0u));
   // The quote names of CONVERSION_REFERENCE row 13, and the code-block names of row 12.
   CHECK(RoleIs("quote", STYLE_ROLE_QUOTE, 0u));
   CHECK(RoleIs("intense quote", STYLE_ROLE_QUOTE, 0u));
   CHECK(RoleIs("block text", STYLE_ROLE_QUOTE, 0u));
   CHECK(RoleIs("quotations", STYLE_ROLE_QUOTE, 0u));
   CHECK(RoleIs("source code", STYLE_ROLE_CODE, 0u));
   CHECK(RoleIs("preformatted text", STYLE_ROLE_CODE, 0u));
   CHECK(RoleIs("html preformatted", STYLE_ROLE_CODE, 0u));
   CHECK(RoleIs("code", STYLE_ROLE_CODE, 0u));
   // A character style reads a different table, and the two overlap on exactly one name. "Source Text"
   // is LibreOffice's character style for inline code and an ordinary paragraph style name otherwise,
   // which is what tests/fixtures/headings depends on: it carries a paragraph style called that.
   CHECK(CharRoleIs("code", STYLE_ROLE_CODE));
   CHECK(CharRoleIs("html code", STYLE_ROLE_CODE));
   CHECK(CharRoleIs("verbatim char", STYLE_ROLE_CODE));
   CHECK(CharRoleIs("source text", STYLE_ROLE_CODE));
   CHECK(CharRoleIs("macro text", STYLE_ROLE_CODE));
   CHECK(RoleIs("source text", STYLE_ROLE_NORMAL, 0u));
   CHECK(RoleIs("macro text", STYLE_ROLE_NORMAL, 0u));
   CHECK(CharRoleIs("source code", STYLE_ROLE_NORMAL));
   CHECK(CharRoleIs("quote", STYLE_ROLE_NORMAL));
   // Word's linked character styles are named "<paragraph style> Char", so none of them is a heading.
   CHECK(CharRoleIs("heading 1", STYLE_ROLE_NORMAL));
   CHECK(CharRoleIs("heading 1 char", STYLE_ROLE_NORMAL));
   CHECK(CharRoleIs("title", STYLE_ROLE_NORMAL));

   CheckGroup("StyleModel: the monospace font table of row 11");
   CHECK(StyleFontIsMonospace("consolas"));
   CHECK(StyleFontIsMonospace("courier new"));
   CHECK(StyleFontIsMonospace("dejavu sans mono"));
   CHECK(StyleFontIsMonospace("jetbrains mono"));
   CHECK(StyleFontIsMonospace("ibm plex mono"));
   CHECK(!StyleFontIsMonospace("calibri"));
   CHECK(!StyleFontIsMonospace("courier newer"));
   CHECK(!StyleFontIsMonospace(""));

   CheckGroup("StyleModel: OnOff values");
   XML_TEXT absent = {nullptr, 0};

   CHECK(StyleOnOff(absent));
   CHECK(StyleOnOff({"1", 1u}));
   CHECK(StyleOnOff({"true", 4u}));
   CHECK(StyleOnOff({"on", 2u}));
   CHECK(!StyleOnOff({"0", 1u}));
   CHECK(!StyleOnOff({"false", 5u}));
   CHECK(!StyleOnOff({"off", 3u}));
   CHECK(!StyleOnOff({"", 0u}));
   CHECK(!StyleOnOff({"wobble", 6u}));
   CHECK(!StyleOnOff({"TRUE", 4u}));

   CheckGroup("StyleModel: an absent or refused part");
   STYLE_MODEL model;

   StyleOpen(&model);
   CHECK(StyleLoad(&model, nullptr, -1) == STYLE_OK);
   CHECK(StyleCount(&model) == 0);
   CHECK(StyleDefaultParagraph(&model) == -1);
   CHECK(StyleFind(&model, "Heading1") == -1);
   CHECK(StyleResolveParagraph(&model, -1, -1).headingLevel == 0);
   CHECK(StyleResolveParagraph(&model, -1, 0).headingLevel == 1u);
   StyleClose(&model);

   CHECK(LoadStyles(&model, "") == STYLE_OK);
   CHECK(StyleCount(&model) == 0);
   StyleClose(&model);

   // A well-formed part whose root is something else is refused as not a style part at all.
   cchptr notStyles = "<w:other xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>";

   StyleOpen(&model);
   CHECK(StyleLoadBytes(&model, (cui8ptr)notStyles, TestLength(notStyles)) == STYLE_ERROR_ROOT);
   StyleClose(&model);

   // A part with no root element at all is not well-formed XML, and the tokenizer says so first.
   StyleOpen(&model);
   CHECK(StyleLoadBytes(&model, (cui8ptr) "", 0u) == STYLE_ERROR_XML);
   StyleClose(&model);

   StyleOpen(&model);
   CHECK(StyleLoadBytes(&model, (cui8ptr) "<w:styles><w:style>", 19u) == STYLE_ERROR_XML);
   StyleClose(&model);

   CheckGroup("StyleModel: identifiers, defaults and types");
   CHECK(LoadStyles(&model, "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
                            "<w:style w:styleId=\"Heading1\"><w:name w:val=\"heading 1\"/></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"Emph\"><w:name w:val=\"Emphasis\"/></w:style>") == STYLE_OK);
   CHECK(StyleCount(&model) == 3u);
   CHECK(StyleDefaultParagraph(&model) == 0);
   CHECK(StyleFind(&model, "Heading1") == 1);
   CHECK(StyleFind(&model, "heading1") == -1); // Identifiers compare exactly; only names fold case
   CHECK(StyleFind(&model, "Nope") == -1);
   CHECK(StyleFind(&model, "") == -1);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Heading1"), -1).headingLevel == 1u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Normal"), -1).headingLevel == 0);
   StyleClose(&model);

   CheckGroup("StyleModel: heading levels and the outline fallback");
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"H7\"><w:name w:val=\"heading 7\"/></w:style>"
                            "<w:style w:styleId=\"H9\"><w:name w:val=\"heading 9\"/></w:style>"
                            "<w:style w:styleId=\"Outlined\"><w:name w:val=\"Body\"/>"
                            "<w:pPr><w:outlineLvl w:val=\"3\"/></w:pPr></w:style>"
                            "<w:style w:styleId=\"Deep\"><w:name w:val=\"Deep Body\"/>"
                            "<w:pPr><w:outlineLvl w:val=\"8\"/></w:pPr></w:style>"
                            "<w:style w:styleId=\"Body\"><w:name w:val=\"Body Text\"/>"
                            "<w:pPr><w:outlineLvl w:val=\"9\"/></w:pPr></w:style>"
                            "<w:style w:styleId=\"Titled\"><w:name w:val=\"Title\"/></w:style>"
                            "<w:style w:styleId=\"Subtitled\"><w:name w:val=\"Subtitle\"/></w:style>") == STYLE_OK);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "H7"), -1).headingLevel == 6u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "H9"), -1).headingLevel == 6u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Outlined"), -1).headingLevel == 4u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Deep"), -1).headingLevel == 6u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Body"), -1).headingLevel == 0);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Titled"), -1).role == STYLE_ROLE_TITLE);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Titled"), -1).headingLevel == 1u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Subtitled"), -1).headingLevel == 2u);
   // A name that says heading wins over any outline level, and a direct level beats the style's own.
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "H7"), 9).headingLevel == 6u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Outlined"), 0).headingLevel == 1u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Body"), 1).headingLevel == 2u);
   StyleClose(&model);

   CheckGroup("StyleModel: basedOn chains and cycles");
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"Base\"><w:name w:val=\"heading 2\"/></w:style>"
                            "<w:style w:styleId=\"Mid\"><w:name w:val=\"Mid Body\"/><w:basedOn w:val=\"Base\"/></w:style>"
                            "<w:style w:styleId=\"Leaf\"><w:name w:val=\"Leaf Body\"/><w:basedOn w:val=\"Mid\"/></w:style>"
                            "<w:style w:styleId=\"Over\"><w:name w:val=\"heading 5\"/><w:basedOn w:val=\"Base\"/></w:style>"
                            "<w:style w:styleId=\"Dangling\"><w:name w:val=\"D\"/><w:basedOn w:val=\"Nope\"/></w:style>"
                            "<w:style w:styleId=\"Self\"><w:name w:val=\"S\"/><w:basedOn w:val=\"Self\"/></w:style>"
                            "<w:style w:styleId=\"LoopA\"><w:name w:val=\"A\"/><w:basedOn w:val=\"LoopB\"/></w:style>"
                            "<w:style w:styleId=\"LoopB\"><w:name w:val=\"B\"/><w:basedOn w:val=\"LoopA\"/></w:style>") == STYLE_OK);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Leaf"), -1).headingLevel == 2u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Over"), -1).headingLevel == 5u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Dangling"), -1).headingLevel == 0);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Self"), -1).headingLevel == 0);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "LoopA"), -1).headingLevel == 0);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "LoopB"), -1).headingLevel == 0);
   StyleClose(&model);

   CheckGroup("StyleModel: toggle XOR across the style chains");
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"One\"><w:name w:val=\"One\"/><w:rPr><w:b/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Two\"><w:name w:val=\"Two\"/><w:basedOn w:val=\"One\"/>"
                            "<w:rPr><w:b/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Three\"><w:name w:val=\"Three\"/><w:basedOn w:val=\"Two\"/>"
                            "<w:rPr><w:b/></w:rPr></w:style>"
                            "<w:style w:styleId=\"OffAgain\"><w:name w:val=\"Off\"/><w:basedOn w:val=\"One\"/>"
                            "<w:rPr><w:b w:val=\"0\"/></w:rPr></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"CharBold\"><w:name w:val=\"CB\"/>"
                            "<w:rPr><w:b/></w:rPr></w:style>") == STYLE_OK);
   CHECK(ToggleOn(ResolveUnder(&model, "One"), STYLE_TOGGLE_BOLD));
   CHECK(!ToggleOn(ResolveUnder(&model, "Two"), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(ResolveUnder(&model, "Three"), STYLE_TOGGLE_BOLD));
   // An explicit false is a specification, but it is not a specification of true, so it does not flip.
   CHECK(ToggleOn(ResolveUnder(&model, "OffAgain"), STYLE_TOGGLE_BOLD));

   STYLE_DIRECT_RUN direct;

   // A character style joins the same XOR, so bold-on-bold cancels across the two chains as well.
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "CharBold");
   CHECK(!ToggleOn(StyleResolveRun(&model, StyleFind(&model, "One"), &direct), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(StyleResolveRun(&model, StyleFind(&model, "Two"), &direct), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(StyleResolveRun(&model, -1, &direct), STYLE_TOGGLE_BOLD));

   // Direct formatting is final: it does not join the XOR, it replaces its answer.
   StyleClearDirect(&direct);
   direct.toggleSpecified = StyleToggleBit(STYLE_TOGGLE_BOLD);
   direct.toggleTrue      = 0;
   CHECK(!ToggleOn(StyleResolveRun(&model, StyleFind(&model, "One"), &direct), STYLE_TOGGLE_BOLD));
   direct.toggleTrue = StyleToggleBit(STYLE_TOGGLE_BOLD);
   CHECK(ToggleOn(StyleResolveRun(&model, StyleFind(&model, "Two"), &direct), STYLE_TOGGLE_BOLD));
   StyleClose(&model);

   CheckGroup("StyleModel: docDefaults never joins the XOR");
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:b/><w:i/></w:rPr></w:rPrDefault></w:docDefaults>"
                            "<w:style w:styleId=\"Bolder\"><w:name w:val=\"Bolder\"/><w:rPr><w:b/></w:rPr></w:style>") == STYLE_OK);
   // A docDefaults true beats every style: one more specification of bold does not cancel it.
   CHECK(ToggleOn(ResolveUnder(&model, "Bolder"), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(ResolveUnder(&model, "Nope"), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(ResolveUnder(&model, "Bolder"), STYLE_TOGGLE_ITALIC));

   // Only the run's own formatting can put a docDefaults true back down again.
   StyleClearDirect(&direct);
   direct.toggleSpecified = StyleToggleBit(STYLE_TOGGLE_BOLD);
   direct.toggleTrue      = 0;
   CHECK(!ToggleOn(StyleResolveRun(&model, StyleFind(&model, "Bolder"), &direct), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(StyleResolveRun(&model, StyleFind(&model, "Bolder"), &direct), STYLE_TOGGLE_ITALIC));
   StyleClose(&model);

   // A docDefaults false is not a specification of true, so it neither short-circuits nor joins the
   // XOR: one style saying true still comes out on.
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:b w:val=\"0\"/></w:rPr></w:rPrDefault>"
                            "</w:docDefaults>"
                            "<w:style w:styleId=\"One\"><w:name w:val=\"One\"/><w:rPr><w:b/></w:rPr></w:style>") == STYLE_OK);
   CHECK(ToggleOn(ResolveUnder(&model, "One"), STYLE_TOGGLE_BOLD));
   CHECK(!ToggleOn(ResolveUnder(&model, "Nope"), STYLE_TOGGLE_BOLD));
   StyleClose(&model);

   CheckGroup("StyleModel: the plain properties are nearest-wins");
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:dstrike/><w:vertAlign w:val=\"subscript\"/></w:rPr>"
                            "</w:rPrDefault></w:docDefaults>"
                            "<w:style w:styleId=\"Base\"><w:name w:val=\"Base\"/>"
                            "<w:rPr><w:vertAlign w:val=\"superscript\"/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Leaf\"><w:name w:val=\"Leaf\"/><w:basedOn w:val=\"Base\"/>"
                            "<w:rPr><w:dstrike w:val=\"0\"/></w:rPr></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"Down\"><w:name w:val=\"Down\"/>"
                            "<w:rPr><w:vertAlign w:val=\"baseline\"/></w:rPr></w:style>") == STYLE_OK);
   CHECK(ResolveUnder(&model, "Base").vertAlign == STYLE_VERT_SUPERSCRIPT);
   CHECK(ResolveUnder(&model, "Base").doubleStrike);
   CHECK(ResolveUnder(&model, "Leaf").vertAlign == STYLE_VERT_SUPERSCRIPT);
   CHECK(!ResolveUnder(&model, "Leaf").doubleStrike);
   CHECK(ResolveUnder(&model, "Nope").vertAlign == STYLE_VERT_SUBSCRIPT);

   // A character style is nearer than a paragraph style, and baseline is a specification that cancels.
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "Down");
   CHECK(StyleResolveRun(&model, StyleFind(&model, "Base"), &direct).vertAlign == STYLE_VERT_BASELINE);
   direct.vertAlign = STYLE_VERT_SUPERSCRIPT;
   CHECK(StyleResolveRun(&model, StyleFind(&model, "Base"), &direct).vertAlign == STYLE_VERT_SUPERSCRIPT);
   StyleClose(&model);

   CheckGroup("StyleModel: the monospace verdict layers like every other plain property");
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\"/></w:rPr>"
                            "</w:rPrDefault></w:docDefaults>"
                            "<w:style w:styleId=\"Fixed\"><w:name w:val=\"Fixed\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Back\"><w:name w:val=\"Back\"/><w:basedOn w:val=\"Fixed\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Georgia\"/></w:rPr></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"Mono\"><w:name w:val=\"Mono\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Menlo\"/></w:rPr></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"Themed\"><w:name w:val=\"Themed\"/>"
                            "<w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\"/></w:rPr></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"CodeChar\"><w:name w:val=\"Code\"/></w:style>") == STYLE_OK);
   CHECK(ResolveUnder(&model, "Fixed").monospace);
   CHECK(!ResolveUnder(&model, "Back").monospace);  // The nearer specification wins, as w:dstrike does
   CHECK(!ResolveUnder(&model, "Nope").monospace);  // docDefaults named a proportional family
   CHECK(!ResolveUnder(&model, "Fixed").codeStyle); // A paragraph style is never a code *span*

   // A character style is nearer than the paragraph style, either way round.
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "Mono");
   CHECK(StyleResolveRun(&model, StyleFind(&model, "Back"), &direct).monospace);
   direct.characterStyle = StyleFind(&model, "Themed");
   // A w:rFonts naming only a theme slot specifies nothing, so the paragraph style still decides.
   CHECK(StyleResolveRun(&model, StyleFind(&model, "Fixed"), &direct).monospace);
   direct.monospace = 0;
   CHECK(!StyleResolveRun(&model, StyleFind(&model, "Fixed"), &direct).monospace);
   // The code role rides on the character style chain alone, and is a separate answer from the font.
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "CodeChar");
   CHECK(StyleResolveRun(&model, -1, &direct).codeStyle);
   CHECK(!StyleResolveRun(&model, -1, &direct).monospace);
   StyleClose(&model);

   CheckGroup("StyleModel: a monospace docDefaults switches the font heuristic off");
   // A document whose own default font is monospace says nothing about any particular run. Without the
   // guard every run in a Courier-set filing is code, every paragraph satisfies row 12's "every run is
   // monospace", and the whole document converts to one fence with every delimiter dead inside it.
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Courier New\"/></w:rPr>"
                            "</w:rPrDefault></w:docDefaults>"
                            "<w:style w:styleId=\"Body\"><w:name w:val=\"Body\"/></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"CodeChar\"><w:name w:val=\"Code\"/></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"Fixed\"><w:name w:val=\"Fixed\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr></w:style>") == STYLE_OK);
   CHECK(!ResolveUnder(&model, "Body").monospace);
   CHECK(!ResolveUnder(&model, "Nope").monospace);

   // What says something about the *run* still says it: a code character style, and a run or a style
   // that names its own monospace family.
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "CodeChar");
   CHECK(StyleResolveRun(&model, -1, &direct).codeStyle);
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "Fixed");
   CHECK(StyleResolveRun(&model, -1, &direct).monospace);
   StyleClearDirect(&direct);
   direct.monospace = 1;
   CHECK(StyleResolveRun(&model, -1, &direct).monospace);
   StyleClose(&model);

   CheckGroup("StyleModel: the baseline is the default paragraph style before docDefaults");
   // The other half of the same guard, and the likelier one: Word's w:docDefaults normally names a
   // *theme* slot, which specifies no family, and Modify Style on Normal is what carries the font. A
   // guard that reads only w:docDefaults leaves this document converting to one fence.
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:asciiTheme=\"minorHAnsi\"/>"
                            "</w:rPr></w:rPrDefault></w:docDefaults>"
                            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                            "<w:name w:val=\"Normal\"/><w:rPr><w:rFonts w:ascii=\"Courier New\"/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Body\"><w:name w:val=\"Body\"/>"
                            "<w:basedOn w:val=\"Normal\"/></w:style>") == STYLE_OK);
   CHECK(!ResolveUnder(&model, "Normal").monospace);
   CHECK(!ResolveUnder(&model, "Body").monospace);
   StyleClearDirect(&direct);
   direct.monospace = 1;
   CHECK(StyleResolveRun(&model, StyleFind(&model, "Body"), &direct).monospace);
   StyleClose(&model);

   // The baseline is read off the default style's *folded* record, so a Normal that inherits the family
   // rather than naming it is still a monospace baseline. Reading the unfolded property instead leaves
   // every suite green and restores the whole-document fence, so this is the case that pins the fold.
   CHECK(LoadStyles(&model, "<w:style w:type=\"paragraph\" w:styleId=\"Base\"><w:name w:val=\"Base\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Courier New\"/></w:rPr></w:style>"
                            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                            "<w:name w:val=\"Normal\"/><w:basedOn w:val=\"Base\"/></w:style>") == STYLE_OK);
   CHECK(!ResolveUnder(&model, "Normal").monospace);
   CHECK(!ResolveUnder(&model, "Base").monospace);
   StyleClose(&model);

   // And the reverse, where the fold is what turns the heuristic back *on*: a default style that
   // inherits Courier and then names a proportional family of its own is a proportional baseline.
   CHECK(LoadStyles(&model, "<w:style w:type=\"paragraph\" w:styleId=\"Base\"><w:name w:val=\"Base\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Courier New\"/></w:rPr></w:style>"
                            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                            "<w:name w:val=\"Normal\"/><w:basedOn w:val=\"Base\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Calibri\"/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Fixed\"><w:name w:val=\"Fixed\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr></w:style>") == STYLE_OK);
   CHECK(ResolveUnder(&model, "Fixed").monospace);
   StyleClose(&model);

   CheckGroup("StyleModel: a basedOn across two style types is ignored");
   // ISO/IEC 29500-1 17.7.4.3: a character style's parent shall be a character style. Beyond
   // conformance it is a hole through the monospace guard -- a character style based on a monospace
   // default *paragraph* style inherits its family, and the character layer is taken before the guard
   // is read, so italic prose converts to a fenced code block.
   CHECK(LoadStyles(&model, "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                            "<w:name w:val=\"Normal\"/><w:rPr><w:rFonts w:ascii=\"Courier New\"/></w:rPr></w:style>"
                            "<w:style w:type=\"character\" w:styleId=\"Emph\"><w:name w:val=\"Emphasis Char\"/>"
                            "<w:basedOn w:val=\"Normal\"/><w:rPr><w:i/></w:rPr></w:style>") == STYLE_OK);
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "Emph");
   CHECK(!StyleResolveRun(&model, -1, &direct).monospace);
   StyleClose(&model);

   // The role leaks the same way in the other direction, and needs no monospace font at all.
   CHECK(LoadStyles(&model, "<w:style w:type=\"character\" w:styleId=\"CodeChar\">"
                            "<w:name w:val=\"HTML Code\"/></w:style>"
                            "<w:style w:type=\"paragraph\" w:styleId=\"Body\"><w:name w:val=\"Body\"/>"
                            "<w:basedOn w:val=\"CodeChar\"/></w:style>") == STYLE_OK);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Body"), -1).role != STYLE_ROLE_CODE);

   // But w:type is optional, and an absent one is a *default* rather than a statement -- so a typeless
   // style keeps its link to a real character style. Comparing the stored types alone would drop it and
   // silently lose the code span it carries.
   StyleClose(&model);
   CHECK(LoadStyles(&model, "<w:style w:type=\"character\" w:styleId=\"MonoBase\"><w:name w:val=\"Mono Base\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr></w:style>"
                            "<w:style w:styleId=\"VerbatimChar\"><w:name w:val=\"Verbatim Char\"/>"
                            "<w:basedOn w:val=\"MonoBase\"/></w:style>") == STYLE_OK);
   StyleClearDirect(&direct);
   direct.characterStyle = StyleFind(&model, "VerbatimChar");
   CHECK(StyleResolveRun(&model, -1, &direct).monospace);
   StyleClose(&model);

   CheckGroup("StyleModel: a proportional default style beats a monospace docDefaults");
   // Nearest-wins runs the other way too, and the answer is the same rule rather than an exception: an
   // unstyled paragraph is proportional here, so a monospace run really does stand out and the
   // heuristic belongs back on.
   CHECK(LoadStyles(&model, "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Courier New\"/></w:rPr>"
                            "</w:rPrDefault></w:docDefaults>"
                            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\">"
                            "<w:name w:val=\"Normal\"/><w:rPr><w:rFonts w:ascii=\"Calibri\"/></w:rPr></w:style>"
                            "<w:style w:styleId=\"Fixed\"><w:name w:val=\"Fixed\"/>"
                            "<w:rPr><w:rFonts w:ascii=\"Consolas\"/></w:rPr></w:style>") == STYLE_OK);
   CHECK(!ResolveUnder(&model, "Normal").monospace);
   CHECK(ResolveUnder(&model, "Fixed").monospace);
   StyleClose(&model);

   CheckGroup("StyleModel: a value that had to be decoded outlives the token that carried it");
   // A value holding a reference is built in the reader's scratch, which the next token rewinds -- so
   // this is the case that catches a view kept a moment too long, and no ASCII-only literal can.
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"A&amp;B\"><w:name w:val=\"heading &amp; 3\"/></w:style>"
                            "<w:style w:styleId=\"C\"><w:name w:val=\"heading 3\"/><w:basedOn w:val=\"A&amp;B\"/>"
                            "</w:style>") == STYLE_OK);
   CHECK(StyleFind(&model, "A&B") == 0);
   CHECK(StyleFind(&model, "A&amp;B") == -1);
   CHECK(NormalizesTo(StyleName(&model, 0), "heading & 3"));
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "C"), -1).headingLevel == 3u);
   StyleClose(&model);

   CheckGroup("StyleModel: what a run's formatting is not read from");
   // A w:rPr inside a w:pPr is the paragraph *mark's* formatting and never reaches text, and a
   // w:rPrChange holds the properties from *before* an accepted revision. Both nest a w:rPr, so a
   // handler that matched on the local name alone would read either as the run's own.
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"Mark\"><w:name w:val=\"Mark\"/>"
                            "<w:pPr><w:rPr><w:b/></w:rPr></w:pPr></w:style>"
                            "<w:style w:styleId=\"Changed\"><w:name w:val=\"Changed\"/>"
                            "<w:rPr><w:i/><w:rPrChange><w:rPr><w:b/></w:rPr></w:rPrChange></w:rPr>"
                            "</w:style>") == STYLE_OK);
   CHECK(!ToggleOn(ResolveUnder(&model, "Mark"), STYLE_TOGGLE_BOLD));
   CHECK(ToggleOn(ResolveUnder(&model, "Changed"), STYLE_TOGGLE_ITALIC));
   CHECK(!ToggleOn(ResolveUnder(&model, "Changed"), STYLE_TOGGLE_BOLD));
   StyleClose(&model);

   CheckGroup("StyleModel: a chain longer than the walk will follow");
   {
      char deep[4096];
      ui64 used = 0;

      // Twenty links, so the walk stops at STYLE_MAX_CHAIN and the last four never contribute. The
      // point of the case is that it terminates and answers, not which answer it gives.
      for(ui32 index = 0; index < 20u; ++index) {
         char  one[160];
         csi32 written = snprintf(one, sizeof(one),
                                  "<w:style w:styleId=\"S%u\"><w:name w:val=\"S%u\"/><w:basedOn w:val=\"S%u\"/>"
                                  "<w:rPr><w:b/></w:rPr></w:style>",
                                  index, index, index + 1u);

         for(si32 at = 0; at < written && used + 1u < sizeof(deep); ++at) deep[used++] = one[at];
      }
      deep[used] = 0;
      CHECK(LoadStyles(&model, deep) == STYLE_OK);
      CHECK(StyleCount(&model) == 20u);
      // Both of these chains are longer than the cap, so both fold exactly sixteen explicit trues --
      // an even count, which the XOR reports as off.
      CHECK(!ToggleOn(ResolveUnder(&model, "S0"), STYLE_TOGGLE_BOLD));
      CHECK(!ToggleOn(ResolveUnder(&model, "S1"), STYLE_TOGGLE_BOLD));
      // From S5 the whole chain is fifteen links, which is inside the cap and is an odd count.
      CHECK(ToggleOn(ResolveUnder(&model, "S5"), STYLE_TOGGLE_BOLD));
      StyleClose(&model);
   }

   CheckGroup("StyleModel: the result sentences track their enumeration");
   CHECK(StyleResultText(nullptr, nullptr, STYLE_OK)[0] != 0);
   CHECK(StyleTextIs(StyleResultText(nullptr, nullptr, STYLE_ERROR_ROOT), "the style part's root element is not w:styles"));
   CHECK(StyleTextIs(StyleResultText(nullptr, nullptr, STYLE_ERROR_LIMIT), "the style part declares more styles than this reader accepts"));
   CHECK(StyleTextIs(StyleResultText(nullptr, nullptr, STYLE_RESULT_COUNT), "the style part could not be read"));

   CheckGroup("StyleModel: an unknown w:type and an absent name");
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"Heading3\"/>"
                            "<w:style w:type=\"nonsense\" w:styleId=\"Odd\"><w:name w:val=\"heading 2\"/></w:style>"
                            "<w:style w:styleId=\"NoId\"><w:name w:val=\"heading 4\"/></w:style>") == STYLE_OK);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Heading3"), -1).headingLevel == 3u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Odd"), -1).headingLevel == 2u);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "NoId"), -1).headingLevel == 4u);
   StyleClose(&model);

   CheckGroup("StyleModel: identifiers found through the index, not by scanning");
   {
      // Identifiers sharing a long prefix are what made the old linear scan quadratic: every
      // comparison walked the whole prefix before it could differ. They must still resolve exactly.
      char   many[4096];
      ui64   used = 0;
      cchptr HEAD = "<w:style w:type=\"paragraph\" w:styleId=\"PREFIXPREFIXPREFIX";
      cchptr MID  = "\"><w:name w:val=\"heading ";

      for(ui32 which = 1u; which <= 6u; ++which) {
         cchptr pieces[5] = {HEAD, "00000", MID, "0", "\"/></w:style>"};
         char   digit[2]  = {char('0' + which), 0};

         pieces[1] = digit;
         pieces[3] = digit;
         for(ui32 piece = 0; piece < 5u; ++piece) {
            cui64 length = TestLength(pieces[piece]);

            for(ui64 at = 0; at < length && used + 1u < sizeof(many); ++at) many[used++] = pieces[piece][at];
         }
      }
      many[used] = 0;
      CHECK(LoadStyles(&model, many) == STYLE_OK);
      CHECK(StyleCount(&model) == 6u);
      CHECK(StyleFind(&model, "PREFIXPREFIXPREFIX1") == 0);
      CHECK(StyleFind(&model, "PREFIXPREFIXPREFIX6") == 5);
      CHECK(StyleFind(&model, "PREFIXPREFIXPREFIX7") == -1);
      CHECK(StyleFind(&model, "PREFIXPREFIXPREFIX") == -1);
      CHECK(StyleFind(&model, "") == -1);
      CHECK(StyleFind(&model, nullptr) == -1);
      CHECK(StyleResolveParagraph(&model, StyleFind(&model, "PREFIXPREFIXPREFIX4"), -1).headingLevel == 4u);
      StyleClose(&model);
   }
   // A duplicated identifier still resolves to the first record, which is what the scan did.
   CHECK(LoadStyles(&model, "<w:style w:styleId=\"Twice\"><w:name w:val=\"heading 1\"/></w:style>"
                            "<w:style w:styleId=\"Twice\"><w:name w:val=\"heading 5\"/></w:style>") == STYLE_OK);
   CHECK(StyleFind(&model, "Twice") == 0);
   CHECK(StyleResolveParagraph(&model, StyleFind(&model, "Twice"), -1).headingLevel == 1u);
   StyleClose(&model);

   CheckGroup("StyleModel: an identifier longer than the walker's lookup key");
   {
      // ST_String stops at 255 bytes, so this is out of spec either way. What must not happen is the
      // model storing an id in full that DocFindStyle, which truncates at the same ceiling, can never
      // match -- the style would silently resolve to nothing and its heading would be lost.
      char   part[1024];
      ui64   used = 0;
      char   key[STYLE_MAX_NAME_BYTES];
      cchptr OPEN  = "<w:style w:type=\"paragraph\" w:styleId=\"";
      cchptr CLOSE = "\"><w:name w:val=\"heading 1\"/></w:style>";

      for(ui64 at = 0; OPEN[at]; ++at) part[used++] = OPEN[at];
      for(ui64 at = 0; at < 300u; ++at) part[used++] = 'L';
      for(ui64 at = 0; CLOSE[at]; ++at) part[used++] = CLOSE[at];
      part[used] = 0;
      for(ui64 at = 0; at + 1u < sizeof(key); ++at) key[at] = 'L';
      key[STYLE_MAX_NAME_BYTES - 1u] = 0;
      CHECK(LoadStyles(&model, part) == STYLE_OK);
      // The lookup key DocFindStyle would build is 255 Ls, and it has to find the style.
      CHECK(StyleFind(&model, key) == 0);
      CHECK(StyleResolveParagraph(&model, StyleFind(&model, key), -1).headingLevel == 1u);
      StyleClose(&model);
   }
}
