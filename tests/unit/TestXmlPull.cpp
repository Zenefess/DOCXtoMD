/*
 * File: TestXmlPull.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-24
 * Last Modified: 2026-08-24
 * Description: Unit tests for XmlPull: token streams driven from string literals, and every refusal.
 * To Do: 1) Add the producer-shaped fixtures (Google Docs, LibreOffice, Pandoc) when M11 collects them.
 *        2) Drive a fuzz corpus through XmlNext once a corpus exists to drive it from.
 * Dependencies: BuildGuards.h, Check.h, XmlPull.h, typedefs.h, memory management.h, windows.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#include "BuildGuards.h"

#include <windows.h>
#include "typedefs.h"
#include "memory management.h"
#include "XmlPull.h"
#include "Check.h"

//-- Namespace URIs the cases use

constexpr cchptr NS_W        = "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr cchptr NS_W_STRICT = "http://purl.oclc.org/ooxml/wordprocessingml/main";
constexpr cchptr NS_R        = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr cchptr NS_PR       = "http://schemas.openxmlformats.org/package/2006/relationships";

// One relationship record exactly as a .rels part spells it, kept out of the case below so the line fits.
constexpr cchptr RELATIONSHIP_ELEMENT = "<Relationship xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\""
                                        " Id=\"rId1\" Target=\"word/document.xml\"/>";

// A Word 2019 w:document root, cut down but with the shape that matters: many namespace declarations,
// one real attribute among them, and a prefix that is not the one the URI is conventionally spelled with.
constexpr cchptr WORD_ROOT = "<w:document"
                             " xmlns:wpc=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\""
                             " xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\""
                             " xmlns:o=\"urn:schemas-microsoft-com:office:office\""
                             " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
                             " xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\""
                             " xmlns:v=\"urn:schemas-microsoft-com:vml\""
                             " xmlns:wp14=\"http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing\""
                             " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\""
                             " xmlns:w10=\"urn:schemas-microsoft-com:office:word\""
                             " xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\""
                             " xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\""
                             " xmlns:w15=\"http://schemas.microsoft.com/office/word/2012/wordml\""
                             " xmlns:wpg=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\""
                             " xmlns:wpi=\"http://schemas.microsoft.com/office/word/2010/wordprocessingInk\""
                             " xmlns:wne=\"http://schemas.microsoft.com/office/word/2006/wordml\""
                             " xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\""
                             " mc:Ignorable=\"w14 w15 wp14\"><w:body/></w:document>";

// Writes an element carrying a given number of ordinary attributes, and reports how many bytes it took.
static cui64 BuildWideElement(chptrc out, cui32 count) {
   ui64 used = 0;

   out[used++] = '<';
   out[used++] = 'a';
   for(ui32 index = 0; index < count; ++index) {
      out[used++] = ' ';
      out[used++] = 'a';
      out[used++] = char('0' + (index / 100u) % 10u);
      out[used++] = char('0' + (index / 10u) % 10u);
      out[used++] = char('0' + index % 10u);
      out[used++] = '=';
      out[used++] = '"';
      out[used++] = 'x';
      out[used++] = '"';
   }
   out[used++] = '/';
   out[used++] = '>';
   out[used]   = 0;
   return used;
}

// Writes an element carrying a given number of namespace declarations, which are scoping and not
// attributes, so they are counted against a different ceiling.
static cui64 BuildBoundElement(chptrc out, cui32 count) {
   ui64 used = 0;

   out[used++] = '<';
   out[used++] = 'a';
   for(ui32 index = 0; index < count; ++index) {
      for(cchptr walk = " xmlns:p"; *walk; ++walk) out[used++] = *walk;
      out[used++] = char('0' + (index / 100u) % 10u);
      out[used++] = char('0' + (index / 10u) % 10u);
      out[used++] = char('0' + index % 10u);
      for(cchptr walk = "=\"urn:x\""; *walk; ++walk) out[used++] = *walk;
   }
   out[used++] = '/';
   out[used++] = '>';
   out[used]   = 0;
   return used;
}

//-- Tracing

// Appends bytes to a NUL-terminated trace, never past its room.
static void TraceAdd(chptrc out, ui64ptrc used, cui64 room, cchptr text, cui64 length) {
   for(ui64 index = 0; index < length && *used + 1u < room; ++index) out[(*used)++] = text[index];
   out[*used] = 0;
}

// Appends a small unsigned number.
static void TraceNumber(chptrc out, ui64ptrc used, cui64 room, cui32 value) {
   char digits[12];
   ui32 length = 0;
   ui32 left   = value;

   do {
      digits[length++] = char('0' + (left % 10u));
      left /= 10u;
   } while(left);
   while(length--) TraceAdd(out, used, room, digits + length, 1u);
}

// Tokenizes one literal and writes a compact trace: (name opens, )name closes, [text] is character data,
// $ is the end of the part, and !n is refusal n. One string per case reads better than ten assertions.
static void XmlTrace(cchptr xml, cui64 byteCount, chptrc out, cui64 room) {
   XML_READER reader;
   ui64       used = 0;

   out[0] = 0;
   XmlOpen(&reader, (cui8ptr)xml, byteCount);
   for(;;) {
      cXML_TOKEN token = XmlNext(&reader);

      if(token == XML_TOKEN_START_ELEMENT) {
         TraceAdd(out, &used, room, "(", 1u);
         TraceAdd(out, &used, room, reader.name.bytes, reader.name.length);
         continue;
      }
      if(token == XML_TOKEN_END_ELEMENT) {
         TraceAdd(out, &used, room, ")", 1u);
         TraceAdd(out, &used, room, reader.name.bytes, reader.name.length);
         continue;
      }
      if(token == XML_TOKEN_TEXT) {
         TraceAdd(out, &used, room, "[", 1u);
         TraceAdd(out, &used, room, reader.text.bytes, reader.text.length);
         TraceAdd(out, &used, room, "]", 1u);
         continue;
      }
      if(token == XML_TOKEN_END_OF_INPUT) TraceAdd(out, &used, room, "$", 1u);
      else {
         TraceAdd(out, &used, room, "!", 1u);
         TraceNumber(out, &used, room, ui32(reader.result));
      }
      break;
   }
   XmlClose(&reader);
}

// Whether a NUL-terminated string ends with another.
static cbool TraceEndsWith(cchptr text, cchptr tail) {
   ui64 textLength = 0;
   ui64 tailLength = 0;

   while(text[textLength]) ++textLength;
   while(tail[tailLength]) ++tailLength;
   if(tailLength > textLength) return false;
   for(ui64 index = 0; index < tailLength; ++index) {
      if(text[textLength - tailLength + index] != tail[index]) return false;
   }
   return true;
}

// Compares two NUL-terminated strings.
static cbool TraceEqual(cchptr a, cchptr b) {
   ui64 index = 0;

   while(a[index] && a[index] == b[index]) ++index;
   return a[index] == b[index];
}

// Measures a NUL-terminated literal, so a case may be written as one string.
static cui64 TextLength(cchptr text) {
   ui64 length = 0;

   while(text[length]) ++length;
   return length;
}

// One case: tokenize the literal and compare its trace against the expected one.
static cbool XmlCase(cchptr xml, cchptr want) {
   char trace[1024];

   XmlTrace(xml, TextLength(xml), trace, sizeof(trace));
   if(TraceEqual(trace, want)) return true;
   printf("      trace %s\n      want  %s\n", trace, want);
   return false;
}

// One case whose literal holds NUL bytes, so its length cannot be measured from the literal itself.
static cbool XmlCaseBytes(cchptr xml, cui64 byteCount, cchptr want) {
   char trace[1024];

   XmlTrace(xml, byteCount, trace, sizeof(trace));
   if(TraceEqual(trace, want)) return true;
   printf("      trace %s\n      want  %s\n", trace, want);
   return false;
}

// Opens a reader and advances to the first token, for the cases that inspect the reader rather than a trace.
static cXML_TOKEN XmlFirst(XML_READERptrc reader, cchptr xml) {
   XmlOpen(reader, (cui8ptr)xml, TextLength(xml));
   return XmlNext(reader);
}

//== Entry point

void TestXmlPull(void) {
   XML_READER reader;

   CheckGroup("XmlPull: structure");
   CHECK(XmlCase("<a/>", "(a)a$"));
   CHECK(XmlCase("<a></a>", "(a)a$"));
   CHECK(XmlCase("<a><b/><c></c></a>", "(a(b)b(c)c)a$"));
   CHECK(XmlCase("<a  />", "(a)a$"));
   CHECK(XmlCase("<a></a  >", "(a)a$"));
   CHECK(XmlCase("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?><a/>", "(a)a$"));
   CHECK(XmlCase("<!-- a comment --><a/><!-- and another -->", "(a)a$"));
   CHECK(XmlCase("   <a/>  \r\n ", "(a)a$"));
   CHECK(XmlCase("\xEF\xBB\xBF<a/>", "(a)a$"));

   CheckGroup("XmlPull: character data");
   CHECK(XmlCase("<a>hi</a>", "(a[hi])a$"));
   CHECK(XmlCase("<a> </a>", "(a[ ])a$"));
   CHECK(XmlCase("<a><b/>  <c/></a>", "(a(b)b[  ](c)c)a$"));
   CHECK(XmlCase("<a>x<!--c-->y</a>", "(a[x][y])a$"));
   CHECK(XmlCase("<a><![CDATA[<b>&]]></a>", "(a[<b>&])a$"));
   CHECK(XmlCase("<a>x<![CDATA[&]]>y</a>", "(a[x&y])a$"));
   CHECK(XmlCase("<a><![CDATA[]]></a>", "(a)a$")); // A section holding nothing is not a token
   CHECK(XmlCase("<a>&amp;&lt;&gt;&quot;&apos;</a>", "(a[&<>\"'])a$"));
   CHECK(XmlCase("<a>&#65;&#x42;&#x62;</a>", "(a[ABb])a$"));
   CHECK(XmlCase("<a>&#x1F600;</a>", "(a[\xF0\x9F\x98\x80])a$"));
   CHECK(XmlCase("<a>&#38;#38;</a>", "(a[&#38;])a$")); // Decoded output is never scanned again
   CHECK(XmlCase("<a>x\r\ny</a>", "(a[x\ny])a$"));
   CHECK(XmlCase("<a>x\ry</a>", "(a[x\ny])a$"));
   CHECK(XmlCase("<a><![CDATA[x\r\ny]]></a>", "(a[x\ny])a$"));
   CHECK(XmlCase("<a>&#xD;</a>", "(a[\r])a$")); // A reference to a carriage return is not normalised

   CheckGroup("XmlPull: refusals");
   CHECK(XmlCase("<!DOCTYPE a SYSTEM \"x\"><a/>", "!1"));
   CHECK(XmlCase("<a><!DOCTYPE b></a>", "(a!1"));
   CHECK(XmlCase("<a></b>", "(a!4"));
   CHECK(XmlCase("</a>", "!4"));
   CHECK(XmlCase("<a>", "(a!3"));
   CHECK(XmlCase("<a><b></a>", "(a(b!4"));
   CHECK(XmlCase("", "!11"));
   CHECK(XmlCase("   ", "!11"));
   CHECK(XmlCase("<a/><b/>", "(a)a!11"));
   CHECK(XmlCase("junk<a/>", "!11"));
   CHECK(XmlCase("<a/>junk", "(a)a!11"));
   CHECK(XmlCase("<a><![CDATA[x", "(a!3"));
   CHECK(XmlCase("<a><!-- x", "(a!3"));
   CHECK(XmlCase("<a><?p x", "(a!3"));
   CHECK(XmlCase("<a b=\"x", "!3"));
   CHECK(XmlCase("<a>&foo;</a>", "(a!9"));
   CHECK(XmlCase("<a>&amp</a>", "(a!9"));
   CHECK(XmlCase("<a>&#;</a>", "(a!9"));
   CHECK(XmlCase("<a>&#0;</a>", "(a!10"));
   CHECK(XmlCase("<a>&#xD800;</a>", "(a!10"));
   CHECK(XmlCase("<a>&#x110000;</a>", "(a!10"));
   CHECK(XmlCase("<a>&#99999999999999999999;</a>", "(a!10"));
   CHECK(XmlCase("<a>x\x01y</a>", "(a!10"));
   CHECK(XmlCase("<a b=\"<\"/>", "!2"));
   CHECK(XmlCase("<a b=\"1\"c=\"2\"/>", "!2"));
   CHECK(XmlCase("<a b/>", "!2"));
   CHECK(XmlCase("<a b=1/>", "!2"));
   CHECK(XmlCase("<a b=\"1\" b=\"2\"/>", "!7"));
   CHECK(XmlCase("<a xmlns:p=\"u\" xmlns:p=\"v\"/>", "!7"));
   CHECK(XmlCase("<p:a/>", "!6"));
   CHECK(XmlCase("<a p:b=\"1\"/>", "!6"));
   CHECK(XmlCase("<a:b:c/>", "!6"));
   CHECK(XmlCase("<a xmlns:xmlns=\"u\"/>", "!6"));
   CHECK(XmlCaseBytes("\xFF\xFE<\0a\0/\0>\0", 10u, "!12"));

   CheckGroup("XmlPull: depth");

   char deep[XML_MAX_DEPTH * 3u + 16u];
   char deepTrace[XML_MAX_DEPTH * 2u + 16u];
   ui64 used = 0;

   // One more open tag than the stack holds. Everything up to the cap tokenizes and the tag that would
   // pass it is refused: an array and a ceiling, rather than a recursion that would run out of stack.
   for(ui32 index = 0; index <= XML_MAX_DEPTH; ++index) {
      deep[used++] = '<';
      deep[used++] = 'a';
      deep[used++] = '>';
   }
   deep[used] = 0;
   XmlTrace(deep, used, deepTrace, sizeof(deepTrace));
   CHECK(TraceEndsWith(deepTrace, "(a!5"));

   // One element short of the cap is fine, so the ceiling really is where it says it is: the part ends
   // in the middle of an element instead, which is a different refusal entirely.
   used = 0;
   for(ui32 index = 0; index < XML_MAX_DEPTH; ++index) {
      deep[used++] = '<';
      deep[used++] = 'a';
      deep[used++] = '>';
   }
   deep[used] = 0;
   XmlTrace(deep, used, deepTrace, sizeof(deepTrace));
   CHECK(TraceEndsWith(deepTrace, "(a!3"));

   CheckGroup("XmlPull: namespaces");
   CHECK(XmlFirst(&reader, "<w:p xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_W && XmlTextEqual(reader.name, "p") && XmlTextEqual(reader.prefix, "w"));
   CHECK(XmlTextEqual(reader.uri, NS_W));
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<w:p xmlns:w=\"http://purl.oclc.org/ooxml/wordprocessingml/main\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_W && XmlTextEqual(reader.uri, NS_W_STRICT)); // Strict maps onto the same id
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<x:p xmlns:x=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_W); // The prefix a file chooses is never what decides
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a xmlns=\"urn:unknown\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_OTHER);
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a/>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_NONE);
   XmlClose(&reader);

   CheckGroup("XmlPull: namespace scoping");
   CHECK(XmlFirst(&reader, "<a xmlns=\"urn:one\"><b/></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && reader.space == XML_NS_OTHER && XmlTextEqual(reader.uri, "urn:one"));
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a xmlns=\"urn:one\"><b xmlns=\"\"/></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && reader.space == XML_NS_NONE);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT && reader.space == XML_NS_NONE);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT && XmlTextEqual(reader.uri, "urn:one")); // The outer binding survives
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<x w:val=\"1\" xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_W, "val"), "1")); // Declared after the attribute it binds
   XmlClose(&reader);

   CheckGroup("XmlPull: attributes");
   CHECK(XmlFirst(&reader, RELATIONSHIP_ELEMENT) == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_PR && XmlTextEqual(reader.uri, NS_PR));
   // An unprefixed attribute is in no namespace, whatever default the element declares
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "Id"), "rId1"));
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "Target"), "word/document.xml"));
   CHECK(!XmlAttribute(&reader, XML_NS_PR, "Id").bytes);
   CHECK(!XmlAttribute(&reader, XML_NS_NONE, "Type").bytes);
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a b=\"x&amp;y\" c='single' d=\"tab\there\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "b"), "x&y"));
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "c"), "single"));
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "d"), "tab here")); // 3.3.3 normalisation
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a b=\"&#x9;\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "b"), "\t")); // A reference is not normalised
   XmlClose(&reader);

   CheckGroup("XmlPull: xml:space");
   CHECK(XmlFirst(&reader, "<a><b xml:space=\"preserve\"><c/></b></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(!reader.preserveSpace);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && reader.preserveSpace);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && reader.preserveSpace); // Inherited by descendants
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT);
   CHECK(!reader.preserveSpace); // and gone once b closes
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a xml:space=\"preserve\"><b xml:space=\"default\"/></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.preserveSpace);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && !reader.preserveSpace);
   XmlClose(&reader);

   CheckGroup("XmlPull: whitespace reporting");
   CHECK(XmlFirst(&reader, "<a>  <b/>x</a>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_TEXT && reader.allWhitespace);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_TEXT && !reader.allWhitespace);
   XmlClose(&reader);

   CheckGroup("XmlPull: skipping a subtree");
   CHECK(XmlFirst(&reader, "<a><skip><deep><deeper/></deep>text</skip><after/></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && XmlIsElement(&reader, XML_NS_NONE, "skip"));
   CHECK(XmlSkipElement(&reader) && reader.depth == 2u);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && XmlIsElement(&reader, XML_NS_NONE, "after"));
   XmlClose(&reader);
   // Skipping a self-closing element must consume its owed end token and nothing more.
   CHECK(XmlFirst(&reader, "<a><skip/><after/></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && XmlIsElement(&reader, XML_NS_NONE, "skip"));
   CHECK(XmlSkipElement(&reader) && reader.depth == 2u);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && XmlIsElement(&reader, XML_NS_NONE, "after"));
   XmlClose(&reader);
   CHECK(XmlFirst(&reader, "<a><skip><unclosed></a>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT);
   CHECK(!XmlSkipElement(&reader)); // A subtree that never closes fails rather than looping
   XmlClose(&reader);

   CheckGroup("XmlPull: depth reporting");
   CHECK(XmlFirst(&reader, "<a><b><c/></b></a>") == XML_TOKEN_START_ELEMENT && reader.depth == 1u);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && reader.depth == 2u);
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && reader.depth == 3u);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT && reader.depth == 3u);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT && reader.depth == 2u);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_ELEMENT && reader.depth == 1u);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_OF_INPUT);
   CHECK(XmlNext(&reader) == XML_TOKEN_END_OF_INPUT); // and it stays there
   XmlClose(&reader);

   CheckGroup("XmlPull: shapes a real .docx carries");
   // A modern Word w:document root declares about thirty namespaces, every one of them an attribute.
   CHECK(XmlFirst(&reader, WORD_ROOT) == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_W && XmlTextEqual(reader.name, "document"));
   CHECK(reader.attributeCount == 1u); // Only mc:Ignorable survives; xmlns is scoping
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_MC, "Ignorable"), "w14 w15 wp14"));
   CHECK(XmlNext(&reader) == XML_TOKEN_START_ELEMENT && XmlIsElement(&reader, XML_NS_W, "body"));
   XmlClose(&reader);
   // mc:AlternateContent, which M7 walks: the Choice and the Fallback are ordinary elements here.
   CHECK(XmlCase("<mc:AlternateContent xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\">"
                 "<mc:Choice Requires=\"wps\"/><mc:Fallback/></mc:AlternateContent>",
                 "(AlternateContent(Choice)Choice(Fallback)Fallback)AlternateContent$"));
   // A VML fallback, whose namespace is a urn rather than a URL.
   CHECK(XmlFirst(&reader, "<v:shape xmlns:v=\"urn:schemas-microsoft-com:vml\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(reader.space == XML_NS_V);
   XmlClose(&reader);
   CHECK(XmlCase("<a><![CDATA[]] not a close ]]></a>", "(a[]] not a close ])a$"));
   CHECK(XmlCase("<a b=\"one\r\ntwo\"/>", "(a)a$")); // A value spanning a line end
   CHECK(XmlFirst(&reader, "<a b=\"one\r\ntwo\"/>") == XML_TOKEN_START_ELEMENT);
   CHECK(XmlTextEqual(XmlAttribute(&reader, XML_NS_NONE, "b"), "one two")); // CRLF is one space, not two
   XmlClose(&reader);

   CheckGroup("XmlPull: the attribute and binding ceilings");

   char wide[XML_MAX_ATTRIBUTES * 16u + 64u];
   ui64 filled = 0;

   // Exactly the cap parses; one more is refused, and the refusal names the attributes rather than
   // some downstream confusion.
   filled = BuildWideElement(wide, XML_MAX_ATTRIBUTES);
   CHECK(XmlCaseBytes(wide, filled, "(a)a$"));
   filled = BuildWideElement(wide, XML_MAX_ATTRIBUTES + 1u);
   CHECK(XmlCaseBytes(wide, filled, "!7"));

   char bound[XML_MAX_NAMESPACES * 24u + 64u];

   filled = BuildBoundElement(bound, XML_MAX_NAMESPACES);
   CHECK(XmlCaseBytes(bound, filled, "(a)a$"));
   filled = BuildBoundElement(bound, XML_MAX_NAMESPACES + 1u);
   CHECK(XmlCaseBytes(bound, filled, "!8"));

   CheckGroup("XmlPull: sentences");
   CHECK(XmlResultText(XML_OK) && XmlResultText(XML_ERROR_ENCODING));
   CHECK(XmlResultText(XML_RESULT(-1)) && XmlResultText(XML_RESULT_COUNT));
   // Pinned against the enum, because a sentence table and an enum drift apart silently:
   // every row below is the tail of the sentence its own value must map to.
   CHECK(TraceEndsWith(XmlResultText(XML_ERROR_DOCTYPE), "which is never read"));
   CHECK(TraceEndsWith(XmlResultText(XML_ERROR_MISMATCH), "it did not open"));
   CHECK(TraceEndsWith(XmlResultText(XML_ERROR_CHARACTER), "XML does not allow"));
   CHECK(TraceEndsWith(XmlResultText(XML_ERROR_TRAILING), "outside its root element"));
   CHECK(TraceEndsWith(XmlResultText(XML_ERROR_ENCODING), "the converter transcodes first"));
   CHECK(TraceEndsWith(XmlResultText(XML_ERROR_MEMORY), "memory to read a part"));
}
