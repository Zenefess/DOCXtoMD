/*
 * File: TestStyleModel.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-08-25
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

// The role and level a literal name resolves to.
static cbool RoleIs(cchptr normalized, cSTYLE_ROLE wantedRole, cui8 wantedLevel) {
   ui8 level = 0;

   return StyleRoleOfName(normalized, &level) == wantedRole && level == wantedLevel;
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
}
