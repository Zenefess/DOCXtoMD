/*
 * File: TestOpcPackage.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-25
 * Description: Unit tests for OpcPackage's pure core: relationship target resolution and rels naming.
 * To Do: 1) Drive OpcOpen from an in-memory archive, once a fixture can be built without touching disk.
 *        2) Add the producer-shaped packages (Google Docs, LibreOffice, Pandoc) when M11 collects them.
 * Dependencies: BuildGuards.h, Check.h, OpcPackage.h, typedefs.h, memory management.h, windows.h, stdio.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include <stdio.h>
#include <windows.h>
#include "typedefs.h"
#include "memory management.h"
#include "OpcPackage.h"
#include "Check.h"

//-- Helpers

// Three rows of OPC_RESULT_SENTENCE, spelled here so the table and the enum indexing it cannot drift
// apart in silence. The last is the out-of-range default, which two enum values below must both reach.
static constexpr cchptr NO_TYPES_SAID   = "not a valid DOCX; the package has no [Content_Types].xml";
static constexpr cchptr LIMIT_SAID      = "not a valid DOCX; the package declares more structure than the converter will read";
static constexpr cchptr UNREADABLE_SAID = "not a valid DOCX; the package could not be read";

// Compares two NUL-terminated strings.
static cbool OpcSame(cchptr a, cchptr b) {
   ui64 index = 0;

   while(a[index] && a[index] == b[index]) ++index;
   return a[index] == b[index];
}

// One resolution that must succeed, and must produce exactly this part name.
static cbool Resolves(cchptr source, cchptr target, cchptr want) {
   char resolved[OPC_MAX_PART_BYTES + 1u] = {};

   if(OpcResolveTarget(source, target, false, resolved, sizeof(resolved)) != OPC_OK) return false;
   if(OpcSame(resolved, want)) return true;
   printf("      resolved %s\n      want     %s\n", resolved, want);
   return false;
}

// One target that must be refused. The resolved buffer must be left empty either way.
static cbool Refuses(cchptr source, cchptr target) {
   char resolved[OPC_MAX_PART_BYTES + 1u] = {'x', 0};

   if(OpcResolveTarget(source, target, false, resolved, sizeof(resolved)) == OPC_OK) {
      printf("      accepted %s -> %s\n", target, resolved);
      return false;
   }
   return !resolved[0];
}

//== Entry point

void TestOpcPackage(void) {
   constexpr cchptr MAIN     = "/word/document.xml";
   constexpr cchptr GLOSSARY = "/word/glossary/document.xml";
   constexpr cchptr PACKAGE  = "/";

   CheckGroup("OpcPackage: targets that resolve");
   CHECK(Resolves(MAIN, "styles.xml", "/word/styles.xml"));
   CHECK(Resolves(MAIN, "media/image1.png", "/word/media/image1.png"));
   CHECK(Resolves(MAIN, "/word/media/image1.png", "/word/media/image1.png"));
   CHECK(Resolves(MAIN, "../customXml/item1.xml", "/customXml/item1.xml"));
   CHECK(Resolves(MAIN, "sub/../styles.xml", "/word/styles.xml"));
   CHECK(Resolves(MAIN, "./styles.xml", "/word/styles.xml"));
   CHECK(Resolves(GLOSSARY, "../media/x.png", "/word/media/x.png"));
   CHECK(Resolves(PACKAGE, "word/document.xml", "/word/document.xml")); // The _rels/.rels case
   CHECK(Resolves(PACKAGE, "/word/document.xml", "/word/document.xml"));
   CHECK(Resolves("/x.xml", "y.xml", "/y.xml")); // A source part in the root
   CHECK(Resolves(MAIN, "my%20picture.png", "/word/my picture.png"));
   CHECK(Resolves(MAIN, "my picture.png", "/word/my picture.png")); // A raw space is tolerated
   CHECK(Resolves(MAIN, "x%2e.png", "/word/x..png"));               // Decodes to a dot mid-name
   CHECK(Resolves(MAIN, "%77ord.xml", "/word/word.xml"));

   CheckGroup("OpcPackage: targets that escape or are not part names");
   CHECK(Refuses(MAIN, ""));
   CHECK(Refuses(MAIN, "../../etc/passwd"));
   CHECK(Refuses(MAIN, "/../word/document.xml"));
   CHECK(Refuses(MAIN, "..%2F..%2Fx"));     // %2F would forge a boundary
   CHECK(Refuses(MAIN, "%2e%2e/%2e%2e/x")); // and %2e would forge a climb
   CHECK(Refuses(MAIN, "C:\\Windows\\x.png"));
   CHECK(Refuses(MAIN, "\\\\server\\share\\x.png"));
   CHECK(Refuses(MAIN, "document.xml:stream")); // An NTFS alternate stream
   CHECK(Refuses(MAIN, "1:stream"));            // A colon that does not follow a scheme is still a colon
   CHECK(Refuses(MAIN, ":stream"));
   CHECK(Refuses(MAIN, "x*y.png")); // The rest of the bytes a part name may not hold,
   CHECK(Refuses(MAIN, "a<b.png")); // literal here rather than spelled as an escape
   CHECK(Refuses(MAIN, "a>b.png"));
   CHECK(Refuses(MAIN, "a|b.png"));
   CHECK(Refuses(MAIN, "a\"b.png"));
   CHECK(Refuses(MAIN, "%2Astar.png")); // and the same bytes spelled as escapes
   CHECK(Refuses(MAIN, "%3Astream"));
   CHECK(Refuses(MAIN, "%A0.png"));                           // an escape that is not well-formed UTF-8
   CHECK(Refuses(MAIN, "%C3%28.png"));                        // and one that is a broken sequence
   CHECK(Resolves(MAIN, "%C3%A9.png", "/word/\xC3\xA9.png")); // but a well-formed one is a name
   CHECK(Refuses(MAIN, "word//document.xml"));
   CHECK(Refuses(MAIN, "word/"));
   CHECK(Refuses(MAIN, "word/document.xml."));
   CHECK(Refuses(MAIN, "#anchor"));
   CHECK(Refuses(MAIN, "styles.xml?v=2"));
   CHECK(Refuses(MAIN, "http://example.com/x")); // A scheme, declared Internal
   CHECK(Refuses(MAIN, "file:///c:/x"));
   CHECK(Refuses(MAIN, "//example.com/x"));
   CHECK(Refuses(MAIN, "x%GG.png"));
   CHECK(Refuses(MAIN, "x%0.png"));
   CHECK(Refuses(MAIN, "x%00.png"));
   CHECK(Refuses(MAIN, "x\x01y.png"));
   CHECK(Refuses(MAIN, "."));
   CHECK(Refuses(MAIN, ".."));
   CHECK(Refuses(MAIN, "sub/.")); // Lands on a folder, and a folder is never a part
   CHECK(Refuses(MAIN, "sub/.."));

   CheckGroup("OpcPackage: targets that are too long");

   char giant[OPC_MAX_TARGET_BYTES + 8u];

   for(ui64 index = 0; index < sizeof(giant) - 1u; ++index) giant[index] = 'a';
   giant[sizeof(giant) - 1u] = 0;
   CHECK(Refuses(MAIN, giant));

   char deep[OPC_MAX_SEGMENTS * 2u + 8u];
   ui64 used = 0;

   for(ui32 index = 0; index <= OPC_MAX_SEGMENTS; ++index) {
      deep[used++] = 'a';
      deep[used++] = '/';
   }
   deep[used++] = 'x';
   deep[used]   = 0;
   CHECK(Refuses(MAIN, deep)); // More path segments than a part name may have

   CheckGroup("OpcPackage: external targets are copied through");

   char resolved[OPC_MAX_PART_BYTES + 1u] = {};

   CHECK(OpcResolveTarget(MAIN, "http://example.com/x?y=1#z", true, resolved, sizeof(resolved)) == OPC_OK);
   CHECK(OpcSame(resolved, "http://example.com/x?y=1#z"));
   CHECK(OpcResolveTarget(MAIN, "mailto:someone@example.com", true, resolved, sizeof(resolved)) == OPC_OK);
   CHECK(OpcSame(resolved, "mailto:someone@example.com"));
   CHECK(OpcResolveTarget(MAIN, "#heading", true, resolved, sizeof(resolved)) == OPC_OK);
   CHECK(OpcSame(resolved, "#heading"));
   CHECK(OpcResolveTarget(MAIN, "", true, resolved, sizeof(resolved)) != OPC_OK);

   CheckGroup("OpcPackage: a destination that is too small");

   char cramped[8] = {};

   CHECK(OpcResolveTarget(MAIN, "media/image1.png", false, cramped, sizeof(cramped)) != OPC_OK);
   CHECK(OpcResolveTarget(MAIN, "media/image1.png", false, cramped, 0) != OPC_OK);

   CheckGroup("OpcPackage: relationship part names");

   char rels[OPC_MAX_PART_BYTES + 16u] = {};

   CHECK(OpcRelsPartName("/word/document.xml", rels, sizeof(rels)) == OPC_OK);
   CHECK(OpcSame(rels, "/word/_rels/document.xml.rels"));
   CHECK(OpcRelsPartName("/", rels, sizeof(rels)) == OPC_OK);
   CHECK(OpcSame(rels, "/_rels/.rels")); // The package's own, spelled by the rule
   CHECK(OpcRelsPartName("/word/footnotes.xml", rels, sizeof(rels)) == OPC_OK);
   CHECK(OpcSame(rels, "/word/_rels/footnotes.xml.rels"));
   CHECK(OpcRelsPartName("/x.xml", rels, sizeof(rels)) == OPC_OK);
   CHECK(OpcSame(rels, "/_rels/x.xml.rels"));                                 // A part in the root, not in a folder
   CHECK(OpcRelsPartName("word/document.xml", rels, sizeof(rels)) != OPC_OK); // A part name has a leading /
   CHECK(OpcRelsPartName("", rels, sizeof(rels)) != OPC_OK);
   CHECK(OpcRelsPartName("/word/document.xml", rels, 8u) != OPC_OK);

   CheckGroup("OpcPackage: sentences and exit codes");
   // Pinned by content, not by non-nullness: OpcResultText cannot return null, so a truth test asserts
   // nothing and would survive the whole table being shifted by a row. That is the drift M4 caught.
   CHECK(OpcSame(OpcResultText(nullptr, OPC_ERROR_NO_CONTENT_TYPES), NO_TYPES_SAID));
   CHECK(OpcSame(OpcResultText(nullptr, OPC_ERROR_LIMIT), LIMIT_SAID));
   CHECK(OpcSame(OpcResultText(nullptr, OPC_RESULT(-1)), UNREADABLE_SAID));
   CHECK(OpcSame(OpcResultText(nullptr, OPC_RESULT_COUNT), UNREADABLE_SAID));
   CHECK(OpcExitCode(nullptr, OPC_OK) == EXIT_ALL_CONVERTED);
   CHECK(OpcExitCode(nullptr, OPC_ERROR_MEMORY) == EXIT_INTERNAL);
   CHECK(OpcExitCode(nullptr, OPC_ERROR_RANGE) == EXIT_INTERNAL);
   CHECK(OpcExitCode(nullptr, OPC_ERROR_NO_MAIN_REL) == EXIT_NOT_DOCX);
   CHECK(OpcExitCode(nullptr, OPC_ERROR_NOT_UTF8) == EXIT_NOT_DOCX);
   CHECK(OpcExitCode(nullptr, OPC_ERROR_ZIP) == EXIT_NOT_DOCX);
}
