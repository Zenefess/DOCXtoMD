/*
 * File: TestZipReader.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-09-23
 * Last Modified: 2026-09-23
 * Description: Unit tests for ZipReader's pure core: decision D10's entry-name rules and the result sentences.
 * To Do: 1) Drive ZipOpen from an in-memory archive, once the reader can take bytes rather than a path.
 *        2) Pin the remaining result sentences, which the container fixtures reach one substring at a time.
 * Dependencies: BuildGuards.h, Check.h, Inflate.h, ZipReader.h, typedefs.h, stdio.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include <stdio.h>
#include "typedefs.h"
#include "Inflate.h"
#include "ZipReader.h"
#include "Check.h"

//-- Helpers

// Rows of ZipReader's two sentence tables, spelled here so a table and the enum indexing it cannot drift
// apart in silence -- which is the drift M4 caught in another module's table, and the reason every table
// in src/ is pinned by content rather than by non-nullness.
static constexpr cchptr LIMIT_SAID     = "refusing this file; it exceeds a decompression limit, so it may be a ZIP bomb";
static constexpr cchptr OLE_SAID       = "not a valid DOCX; this is an OLE compound file, so an encrypted .docx or a legacy .doc";
static constexpr cchptr DISTANCE_SAID  = "not a valid DOCX; a deflate stream matches data from before the start of the entry";
static constexpr cchptr CORRUPT_SAID   = "not a valid DOCX; a deflate stream is corrupt";
static constexpr cchptr NAME_SAID      = "not a valid DOCX; an entry name is not a relative path this reader will hold";
static constexpr cchptr DRIVE_SAID     = "not a valid DOCX; an entry name begins with a drive letter, which ZIP forbids";
static constexpr cchptr ABSOLUTE_SAID  = "not a valid DOCX; an entry name is an absolute path, which ZIP forbids";
static constexpr cchptr BACKSLASH_SAID = "not a valid DOCX; an entry name uses a backslash as a path separator, which ZIP forbids";
static constexpr cchptr SEGMENT_SAID   = "not a valid DOCX; an entry name holds a . or .. segment, the shape a path traversal takes";

// Compares two NUL-terminated strings.
static cbool ZipSame(cchptr a, cchptr b) {
   ui64 index = 0;

   while(a[index] && a[index] == b[index]) ++index;
   return a[index] == b[index];
}

// Length of a NUL-terminated string.
static cui64 ZipLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// The rule one name breaks, measured the way ZipParseCentral hands it over.
static cZIP_NAME_RULE Rule(cchptr name) { return ZipCheckEntryName(name, ZipLength(name)); }

// A reader holding nothing but the record of one refusal, which is all ZipResultText reads for a name.
static void Refused(ZIP_READERptrc reader, cZIP_NAME_RULE rule, cchptr name) {
   reader->lastInflate = INFLATE_OK;
   reader->lastName    = rule;
   reader->badName     = name;
   reader->message[0]  = 0;
}

// One composed sentence that must be exactly this.
static cbool Says(ZIP_READERptrc reader, cZIP_RESULT result, cchptr want) {
   cchptr said = ZipResultText(reader, result);

   if(ZipSame(said, want)) return true;
   printf("      said %s\n      want %s\n", said, want);
   return false;
}

//== Entry point

void TestZipReader(void) {
   CheckGroup("ZipReader: entry names a package is made of");
   CHECK(Rule("[Content_Types].xml") == ZIP_NAME_OK);
   CHECK(Rule("_rels/.rels") == ZIP_NAME_OK); // A leaf that starts with a dot is not a dot segment
   CHECK(Rule("word/document.xml") == ZIP_NAME_OK);
   CHECK(Rule("word/_rels/document.xml.rels") == ZIP_NAME_OK);
   CHECK(Rule("word/media/image1.png") == ZIP_NAME_OK);
   CHECK(Rule("word/") == ZIP_NAME_OK);            // A directory entry
   CHECK(Rule("word//unused.xml") == ZIP_NAME_OK); // An empty segment is inert
   CHECK(Rule("word/extra.") == ZIP_NAME_OK);      // A trailing dot is part of a name
   CHECK(Rule("word/...xml") == ZIP_NAME_OK);      // and three dots are a name, not a climb
   CHECK(Rule("word/..x") == ZIP_NAME_OK);
   CHECK(Rule("__MACOSX/word/._document.xml") == ZIP_NAME_OK);
   CHECK(Rule("word/r\xC3\xA9sum\xC3\xA9.xml") == ZIP_NAME_OK);
   CHECK(Rule("word/a b%20c.xml") == ZIP_NAME_OK);
   CHECK(ZipCheckEntryName("word/../x", 4u) == ZIP_NAME_OK); // Only the bytes it is told about are read
   CHECK(ZipCheckEntryName(nullptr, 0u) == ZIP_NAME_OK);

   CheckGroup("ZipReader: entry names decision D10 refuses");
   CHECK(Rule("C:/Windows/win.ini") == ZIP_NAME_DRIVE);
   CHECK(Rule("c:evil.xml") == ZIP_NAME_DRIVE);           // Drive-relative, which is still a drive
   CHECK(Rule("C:\\Windows\\win.ini") == ZIP_NAME_DRIVE); // A drive before it is a backslash
   CHECK(Rule("/word/document.xml") == ZIP_NAME_ABSOLUTE);
   CHECK(Rule("/") == ZIP_NAME_ABSOLUTE);
   CHECK(Rule("/../x") == ZIP_NAME_ABSOLUTE); // Absolute before it is a climb
   CHECK(Rule("word\\document.xml") == ZIP_NAME_BACKSLASH);
   CHECK(Rule("_rels\\.rels") == ZIP_NAME_BACKSLASH);
   CHECK(Rule("\\\\server\\share\\x") == ZIP_NAME_BACKSLASH);
   CHECK(Rule("..\\..\\x") == ZIP_NAME_BACKSLASH); // A backslash before it is a climb
   CHECK(Rule("word/document.xml:Zone.Identifier") == ZIP_NAME_STREAM);
   CHECK(Rule("customXml/item:1.xml") == ZIP_NAME_STREAM);
   CHECK(Rule("1:x") == ZIP_NAME_STREAM); // A digit is not a drive letter
   CHECK(Rule(":x") == ZIP_NAME_STREAM);
   CHECK(Rule("../../evil.xml") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule("word/../../evil.xml") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule("word/..") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule("word/../") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule("./word/document.xml") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule("word/./document.xml") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule(".") == ZIP_NAME_DOT_SEGMENT);
   CHECK(Rule("..") == ZIP_NAME_DOT_SEGMENT);

   CheckGroup("ZipReader: sentences");
   // Pinned by content, not by non-nullness: every entry below returns a sentence whatever happens, so a
   // truth test would survive the whole table being shifted by a row.
   CHECK(Says(nullptr, ZIP_ERROR_LIMIT, LIMIT_SAID));
   CHECK(Says(nullptr, ZIP_ERROR_OLE, OLE_SAID));
   CHECK(Says(nullptr, ZIP_ERROR_INFLATE, CORRUPT_SAID));
   CHECK(Says(nullptr, ZIP_ERROR_NAME, NAME_SAID));

   ZIP_READER reader = {};

   Refused(&reader, ZIP_NAME_OK, nullptr);
   reader.lastInflate = INFLATE_ERROR_DISTANCE;
   CHECK(Says(&reader, ZIP_ERROR_INFLATE, DISTANCE_SAID)); // The last row of the inflate table
   reader.lastInflate = INFLATE_RESULT(-1);
   CHECK(Says(&reader, ZIP_ERROR_INFLATE, CORRUPT_SAID)); // and a value outside it

   CheckGroup("ZipReader: a refused entry is named");
   Refused(&reader, ZIP_NAME_BACKSLASH, "_rels\\.rels");
   CHECK(Says(&reader, ZIP_ERROR_NAME, "not a valid DOCX; an entry name uses a backslash as a path separator, which ZIP forbids, in _rels\\.rels"));
   Refused(&reader, ZIP_NAME_DOT_SEGMENT, "../../evil.xml");
   cchptr climbed = "not a valid DOCX; an entry name holds a . or .. segment, the shape a path traversal takes, in ../../evil.xml";

   CHECK(Says(&reader, ZIP_ERROR_NAME, climbed));
   // An entry name is attacker-controlled bytes, and a carriage return or an escape in one would rewrite
   // the console line it is printed on.
   Refused(&reader, ZIP_NAME_STREAM, "a:\r\x1B[2Jb\x7F");
   CHECK(Says(&reader, ZIP_ERROR_NAME, "not a valid DOCX; an entry name holds a colon, which names an NTFS alternate data stream, in a:??[2Jb?"));
   Refused(&reader, ZIP_NAME_BACKSLASH, nullptr);
   CHECK(Says(&reader, ZIP_ERROR_NAME, BACKSLASH_SAID)); // No name recorded: the rule alone
   Refused(&reader, ZIP_NAME_DRIVE, nullptr);
   CHECK(Says(&reader, ZIP_ERROR_NAME, DRIVE_SAID));
   Refused(&reader, ZIP_NAME_ABSOLUTE, nullptr);
   CHECK(Says(&reader, ZIP_ERROR_NAME, ABSOLUTE_SAID));
   Refused(&reader, ZIP_NAME_RULE(99), nullptr);
   CHECK(Says(&reader, ZIP_ERROR_NAME, NAME_SAID)); // A rule outside the table: the general sentence

   char longName[1024];

   for(ui64 index = 0; index < sizeof(longName) - 1u; ++index) longName[index] = 'x';
   longName[0]                     = '/';
   longName[sizeof(longName) - 1u] = 0;
   Refused(&reader, ZIP_NAME_ABSOLUTE, longName);

   cchptr said = ZipResultText(&reader, ZIP_ERROR_NAME);

   CHECK(ZipLength(said) == sizeof(reader.message) - 1u); // Cut at the buffer, never past it
   CHECK(said[ZipLength(said) - 1u] == 'x');
   Refused(&reader, ZIP_NAME_DOT_SEGMENT, nullptr);
   CHECK(Says(&reader, ZIP_ERROR_NAME, SEGMENT_SAID)); // The last row of the name table
}
