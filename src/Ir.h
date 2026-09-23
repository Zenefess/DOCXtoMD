/*
 * File: Ir.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-25
 * Last Modified: 2026-09-23
 * Description: The intermediate representation: blocks, spans and the arena the walker builds them in.
 * To Do: 1) Record the source paragraph index on a block, so a diagnostic can point at the original.
 *        2) Carry a cell's own w:tcBorders and w:shd, which the HTML fallback could render and the
 *           pipe form could not.
 *        3) Carry a comment's range and body as a note of a third kind, if --comments=footnotes is ever
 *           wanted; the note records below are the shape it would take.
 * Dependencies: typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"

//== Formatting

/// One bit per piece of run formatting the Markdown mapping can express. This is the *output* model, not
/// WordprocessingML's: StyleModel resolves twelve toggles and several plain properties, and the walker
/// keeps only what a delimiter could be emitted for.
/// @note Two spans merge when these bits are equal, so a property that renders identically must land on
///       the same bit. That is why w:b and w:bCs share IR_FMT_BOLD, and why a run that is code because
///       of its character style and one that is code because of its font share IR_FMT_CODE.
constexpr cui32 IR_FMT_NONE   = 0x00u;
constexpr cui32 IR_FMT_BOLD   = 0x01u; ///< ** **
constexpr cui32 IR_FMT_ITALIC = 0x02u; ///< * *
constexpr cui32 IR_FMT_STRIKE = 0x04u; ///< ~~ ~~
constexpr cui32 IR_FMT_SUPER  = 0x08u; ///< <sup> </sup>
constexpr cui32 IR_FMT_SUB    = 0x10u; ///< <sub> </sub>
constexpr cui32 IR_FMT_CODE   = 0x20u; ///< ` `

//== Blocks and spans

/// What one block of the document is.
/// @note A block has exactly one kind, so a paragraph that is both a heading and a quote by style is
///       whichever the walker decides -- see DocWalker.h, which gives a heading precedence over both of
///       the kinds M6 adds, because a heading is the document's structure and the others are its voice.
enum IR_BLOCK_KIND : ui8 {
   IR_BLOCK_PARAGRAPH = 0, ///< An ordinary paragraph
   IR_BLOCK_HEADING,       ///< A heading, whose level is carried beside the kind
   IR_BLOCK_QUOTE,         ///< A blockquote paragraph; every emitted line takes the "> " prefix
   IR_BLOCK_CODE,          ///< One line of a fenced code block; consecutive ones share a fence
   IR_BLOCK_RULE,          ///< A horizontal rule, which carries no spans at all
   IR_BLOCK_TABLE,         ///< A table; it carries no spans, and its rows and cells name the blocks
   IR_BLOCK_KIND_COUNT     ///< Number of values above; not a kind
};

/// Constant form of IR_BLOCK_KIND, spelled per GCS r2.
typedef const IR_BLOCK_KIND cIR_BLOCK_KIND;

/// What one span inside a block is.
/// @note The three M7 kinds are markers rather than content, and that is what makes them useful twice
///       over. A link is a start and an end with its content between them, so the content coalesces
///       normally while the markers stop a merge crossing the brackets -- which is CONVERSION_REFERENCE
///       5.1's "coalesce runs *within* the hyperlink" without a rule of its own.
/// @note M10's note reference is a marker of the same sort. It carries the reference as written -- the
///       w:id, with IR_SPAN_FLAG_END saying which story -- until LinkResolveNotes rewrites it into the label
///       the emitter writes between "[^" and "]". Text on either side of one is not adjacent in the
///       output, so it stops a merge the way a link's brackets do.
enum IR_SPAN_KIND : ui8 {
   IR_SPAN_TEXT = 0,   ///< A run of text with one set of formatting
   IR_SPAN_BREAK,      ///< A hard line break inside the block
   IR_SPAN_LINK_START, ///< Opens a link; its destination is the span's dest
   IR_SPAN_LINK_END,   ///< Closes the link the nearest IR_SPAN_LINK_START opened
   IR_SPAN_IMAGE,      ///< An image: its alt text is the span's text, its source the span's dest
   IR_SPAN_ANCHOR,     ///< A bookmark: a name a link may target, carried as the span's text
   IR_SPAN_NOTE,       ///< A footnote or endnote reference: the note's w:id, then its label, as the dest
   IR_SPAN_KIND_COUNT  ///< Number of values above; not a kind
};

/// Constant form of IR_SPAN_KIND, spelled per GCS r2.
typedef const IR_SPAN_KIND cIR_SPAN_KIND;

//== Span flags

/// What a span's destination currently is, and whether the span will be emitted at all. A destination
/// is rewritten twice on its way to the output -- LinkResolve turns a relationship id into a URL or a
/// part name, and MediaPlan turns a part name into a relative file path -- so the flag says which stage
/// it has reached rather than being guessed from the bytes.
constexpr cui8 IR_SPAN_FLAG_NONE = 0x00u;
constexpr cui8 IR_SPAN_FLAG_REL  = 0x01u; ///< The destination is a relationship id, not yet resolved
constexpr cui8 IR_SPAN_FLAG_PART = 0x02u; ///< The destination is a package part name, not yet extracted
constexpr cui8 IR_SPAN_FLAG_MUTE = 0x04u; ///< The span emits nothing: an anchor, a link marker or a note reference
constexpr cui8 IR_SPAN_FLAG_END  = 0x08u; ///< A note reference names an endnote rather than a footnote

//== List flags

/// What a block's list membership is. It is deliberately not a block *kind*: a list item's kind is what
/// its content is -- an ordinary paragraph, a quotation, a line of code -- and being an item of a list
/// is a second, independent fact about it, so a document can say both and this can carry both.
/// @note A heading is the one combination that cannot arise. CONVERSION_REFERENCE 5.4 rules that a
///       heading carrying numbering is a heading and nothing else, so the walker never records a list
///       reference on one.
constexpr cui8 IR_LIST_NONE    = 0x00u;
constexpr cui8 IR_LIST_ITEM    = 0x01u; ///< The block is one item of a list
constexpr cui8 IR_LIST_ORDERED = 0x02u; ///< Its marker is a computed number rather than a bullet
constexpr cui8 IR_LIST_PLAIN   = 0x04u; ///< It carries no marker at all: w:numFmt none, mapping row 16
constexpr cui8 IR_LIST_FIRST   = 0x08u; ///< It opens a list the item before it was not part of

//== Tables

/// What one table's shape obliges of the emitter. Both bits are facts the walk reads and neither is a
/// policy: which of them turns into raw HTML is the emitter's question, and it depends on --tables.
constexpr cui8 IR_TABLE_NONE   = 0x00u;
constexpr cui8 IR_TABLE_MERGED = 0x01u; ///< Some cell spans columns or rows, which GFM cannot say
constexpr cui8 IR_TABLE_NESTED = 0x02u; ///< Some cell holds a table, which GFM cannot say at all

/// What one row is. IR_ROW_HEADER records a w:tblHeader; the emitter's header is the first row regardless.
constexpr cui8 IR_ROW_NONE   = 0x00u;
constexpr cui8 IR_ROW_HEADER = 0x01u; ///< The row carried w:tblHeader, which the emitter does not read

/// What one cell is beyond its content.
constexpr cui8 IR_CELL_NONE     = 0x00u;
constexpr cui8 IR_CELL_VRESTART = 0x01u; ///< w:vMerge="restart": the cell a vertical merge begins at
constexpr cui8 IR_CELL_VMERGED  = 0x02u; ///< w:vMerge with no value: a continuation of the cell above

/// How a column's content is aligned, read from the first row's own w:jc (mapping row 18).
enum IR_ALIGN : ui8 {
   IR_ALIGN_NONE = 0, ///< The document said nothing, so the delimiter row says nothing either
   IR_ALIGN_LEFT,     ///< ":---"
   IR_ALIGN_CENTRE,   ///< ":---:"
   IR_ALIGN_RIGHT,    ///< "---:"
   IR_ALIGN_COUNT     ///< Number of values above; not an alignment
};

/// Constant form of IR_ALIGN, spelled per GCS r2.
typedef const IR_ALIGN cIR_ALIGN;

/// The most columns one table may have. A GFM row is one line, so a table this wide is already past
/// anything a reader could follow; the cap is here so that a part cannot size an allocation by declaring
/// w:gridCol a million times.
constexpr cui32 IR_MAX_COLUMNS = 256u;

/// How deep tables may nest before one is dropped. Word's own editor stops well short of this, and the
/// tokenizer's own 256-element nesting cap would stop a runaway anyway -- this is the bound that keeps
/// the walker's *stack* off the document's content, which that cap does not.
constexpr cui32 IR_MAX_TABLE_DEPTH = 12u;

/// What a table's alignAt holds when no column of it is aligned, which is the ordinary case: an offset
/// into the arena cannot be a sentinel by being 0, because 0 is where the first table's alignments land.
constexpr cui32 IR_ALIGN_ABSENT = 0xFFFFFFFFu;

/// The end of a row or cell chain, and what a table with no rows holds.
constexpr cui32 IR_NO_INDEX = 0xFFFFFFFFu;

/// One table. blockAt..blockEnd is every block the table owns -- its own block first, then every block
/// of every cell of every row, in document order, nested tables included.
/// @note The block range is what lets the emitter skip the whole table in its top loop, and what lets
///       IrDropEmptyBlocks move a table without looking inside one. Nothing inside a table is ever
///       dropped, so every record of one table moves by the same delta.
/// @note A table's rows and a row's cells are **chains** and not ranges, which are the two places this
///       module gives up a contiguous array, and a nested table is why. A cell's content is walked where
///       it stands, so a table inside the first cell of a row appends its own rows and cells to the same
///       arrays before the outer row's next cell and the outer table's next row -- no order of appending
///       makes both contiguous, because a second nested table in a second cell interleaves again.
///       Chaining costs one field and makes the question disappear; every walk of a chain goes forward.
/// @note The column alignments live in an arena of their own rather than in an array here, for the
///       reason every other arena in this module exists: a fixed array of IR_MAX_COLUMNS would make a
///       record of a one-column table 256 bytes wide, and a part is free to declare a great many
///       one-column tables inside the container's own byte cap.
struct IR_TABLE {
   ui32 firstRow; ///< The table's first row, or IR_NO_INDEX when it has none
   ui32 blockAt;  ///< Index of the table's own IR_BLOCK_TABLE block
   ui32 blockEnd; ///< One past the last block the table owns
   ui32 columns;  ///< The grid width every emitted row is padded to
   ui32 alignAt;  ///< Align-arena offset of this table's columns entries, or IR_ALIGN_ABSENT
   ui8  flags;    ///< The IR_TABLE bits in force
};

/// One row of one table, and a link to the next row of the same table.
/// @note skipBefore is w:gridBefore, the one place WordprocessingML lets a row start part-way across the
///       grid: Word writes it for an indented row and for a row whose leading cells were deleted. The row's
///       first cell starts there rather than at column 0, which is what leaves the columns before it empty
///       instead of sliding every cell of the row left. skipAfter is w:gridAfter, the same at the far end,
///       and counts toward the table's width without placing anything.
struct IR_ROW {
   ui32 firstCell;  ///< The row's first cell, or IR_NO_INDEX when it has none
   ui32 nextRow;    ///< The next row of the same table, or IR_NO_INDEX
   ui16 skipBefore; ///< w:gridBefore: grid columns before the first cell, clamped to IR_MAX_COLUMNS
   ui16 skipAfter;  ///< w:gridAfter: grid columns after the last cell, clamped to IR_MAX_COLUMNS
   ui8  flags;      ///< The IR_ROW bits in force
};

// The two offsets are 16 bits because IR_MAX_COLUMNS fits there, which keeps a row record at sixteen
// bytes: an empty w:tr is seven bytes of input, and a row is one of the records a hostile part can
// make this module hold the most of per byte it spends.
static_assert(IR_MAX_COLUMNS <= 0xFFFFu, "Ir: IR_ROW keeps its grid offsets in 16 bits, so IR_MAX_COLUMNS must fit in them.");

/// One cell of one row, and a link to the next cell of the same row. Its blocks are a contiguous range
/// in the document's own block array, because a cell's content is walked where it stands and every pass
/// above the walk reads blocks in that order.
/// @note column and span are what a merge costs. A cell starting at column 3 with a span of 2 covers
///       columns 3 and 4, and the emitter writes its content in the first and an empty pad in the rest,
///       which is CONVERSION_REFERENCE row 19's policy A.
struct IR_CELL {
   ui32 blockAt;    ///< Index of the cell's first block; the block count with a count of 0 when empty
   ui32 blockCount; ///< How many blocks it has
   ui32 column;     ///< The grid column its content starts at
   ui32 span;       ///< How many grid columns it covers, which is w:gridSpan and at least 1
   ui32 nextCell;   ///< The next cell of the same row, or IR_NO_INDEX
   ui8  flags;      ///< The IR_CELL bits in force
   ui8  align;      ///< The first alignment a w:jc of its paragraphs named, as an IR_ALIGN
};

/// Constant and pointer forms of the table records, spelled per GCS r2/t2.
typedef IR_TABLE       *IR_TABLEptr;
typedef const IR_TABLE *cIR_TABLEptr;
typedef IR_TABLE *const IR_TABLEptrc;
typedef IR_ROW         *IR_ROWptr;
typedef const IR_ROW   *cIR_ROWptr;
typedef IR_CELL        *IR_CELLptr;
typedef const IR_CELL  *cIR_CELLptr;

//== Notes

/// Which of WordprocessingML's two note stories a note was read from. Markdown has one kind of note and
/// the emitter writes both the same way; the kind survives because a reference names its note by kind
/// and w:id together, and the same w:id is routinely both a footnote and an endnote.
enum IR_NOTE_KIND : ui8 {
   IR_NOTE_FOOT = 0,  ///< A w:footnote, read from the part the footnotes relationship names
   IR_NOTE_END,       ///< A w:endnote, read from the part the endnotes relationship names
   IR_NOTE_KIND_COUNT ///< Number of values above; not a kind
};

/// Constant form of IR_NOTE_KIND, spelled per GCS r2.
typedef const IR_NOTE_KIND cIR_NOTE_KIND;

/// One footnote or endnote whose body the walk read. Its blocks are ordinary blocks in the one flat array,
/// after every block of the body, and each carries the note's index -- which is what leaves every pass
/// between the walk and the emitter reading one array, exactly as a table's cells do.
/// @note Only a note something references is ever read, so a record here always has a reference that
///       names it. The number is what LinkResolveNotes assigns in reading order, and it is the label the
///       emitter writes; 0 means the pass has not run, or that every reference to the note was muted.
struct IR_NOTE {
   si32         id;     ///< The note's w:id, as the part declared it
   si32         part;   ///< The part it was read from, which its relationship ids are scoped to; -1 for none
   ui32         number; ///< Its label in the output, from 1, once LinkResolveNotes has run; 0 until then
   IR_NOTE_KIND kind;   ///< Which story it belongs to
};

/// Constant and pointer forms of IR_NOTE, spelled per GCS r2/t2.
typedef IR_NOTE       *IR_NOTEptr;
typedef const IR_NOTE *cIR_NOTEptr;

/// One block. Its spans are a contiguous range, because a block is built to completion before the next
/// one starts and nothing ever inserts into the middle of one.
/// @note The four list fields are written in two stages, exactly as a link's destination is. The walk
///       records the reference the paragraph carried -- listNumId and listLevel, straight out of its
///       w:numPr -- and NumAssignMarkers turns that into the marker the emitter writes, filling
///       listNumber and listFlags and clearing listNumId again when the reference resolves to nothing.
struct IR_BLOCK {
   ui32          spanAt;       ///< Index of the block's first span
   ui32          spanCount;    ///< How many spans it has
   si32          listNumId;    ///< The w:numId the walk read, or -1 when the paragraph is not an item
   ui32          listNumber;   ///< What an ordered item's marker counts; 0 for every other block
   si32          tableAt;      ///< Which table an IR_BLOCK_TABLE block is, or -1 for every other block
   si32          note;         ///< Which note the block belongs to, or -1 for a block of the body
   IR_BLOCK_KIND kind;         ///< What the block is
   ui8           headingLevel; ///< 1 to 6 for a heading, 0 otherwise
   ui8           listLevel;    ///< The w:ilvl an item was written at, 0 to 8; 0 for every other block
   ui8           listFlags;    ///< The IR_LIST bits in force
};

/// One span. Both of its byte ranges live in the document's arenas rather than in the record, so a span
/// is twenty-four bytes and an array of them is still worth scanning.
/// @note Only IR_SPAN_IMAGE uses both ranges at once -- its text is the alt text and its dest the source
///       -- which is what settles the field pair rather than one range and a discriminator: a link start
///       carries only a destination and an anchor only a name, and a record that held one range would
///       need a second span to say the other.
/// @note The two ranges address *different* arenas, and that is load-bearing rather than tidy. Every
///       text span of a block lies end to end in the text arena, which is the invariant RunCoalescer
///       merges on; a destination written between two runs would put a gap in the middle of it, and an
///       anchor -- which CONVERSION_REFERENCE 5.1 says a merge must see straight through, because Word
///       writes _GoBack in the middle of a paragraph -- would then stop the two halves of a word from
///       ever coming back together.
struct IR_SPAN {
   ui32         textAt;    ///< Text-arena offset of the span's bytes
   ui32         textBytes; ///< How many bytes they are; never NUL-terminated
   ui32         destAt;    ///< Destination-arena offset of the span's destination, for the kinds with one
   ui32         destBytes; ///< How many bytes it is; never NUL-terminated
   ui32         fmt;       ///< The IR_FMT bits in force
   IR_SPAN_KIND kind;      ///< What the span is
   ui8          flags;     ///< The IR_SPAN_FLAG bits in force
};

/// Constant and pointer forms of the records, spelled per GCS r2/t2.
typedef IR_BLOCK       *IR_BLOCKptr;
typedef const IR_BLOCK *cIR_BLOCKptr;
typedef IR_SPAN        *IR_SPANptr;
typedef const IR_SPAN  *cIR_SPANptr;

/// Where one block started, so that ending it can trim it or throw it away again.
/// @note The three table counts are here for IrRewind alone. An mc:AlternateContent may wrap a w:tbl at
///       block level, so the first mc:Choice can build a whole table that an mc:Fallback then discards;
///       a rewind that unwound only blocks and spans would leave the row and cell records of a table
///       nothing points at, and the next table would inherit them.
struct IR_MARK {
   si32 block;   ///< The block's index, or -1 when it could not be started
   ui32 spanAt;  ///< The span count when it started
   ui32 tableAt; ///< The table count when it started
   ui32 rowAt;   ///< The row count when it started
   ui32 cellAt;  ///< The cell count when it started
   ui64 heapAt;  ///< The text arena's used size when it started
   ui64 destAt;  ///< The destination arena's used size when it started
   ui64 alignAt; ///< The align arena's used size when it started
};

/// Constant form of IR_MARK, spelled per GCS r2.
typedef const IR_MARK cIR_MARK;

//== Document

/// One document's intermediate representation. A worker owns one of these and never shares it (D6), so
/// nothing here takes a lock.
struct al32 IR_DOCUMENT {
   IR_BLOCKptr blocks;        ///< Every block, in document order
   IR_SPANptr  spans;         ///< Every span, grouped by block
   IR_TABLEptr tables;        ///< Every table, in the order the walk reached them
   IR_ROWptr   rows;          ///< Every row of every table; a table's own rows are chained by nextRow
   IR_CELLptr  cells;         ///< Every cell of every row; a row's own cells are chained by nextCell
   IR_NOTEptr  notes;         ///< Every note whose body was read, in the order the walk read them
   chptr       heap;          ///< Every byte of paragraph text, addressed by offset
   chptr       dest;          ///< Every byte of every destination and anchor name, addressed by offset
   ui8ptr      align;         ///< Every column alignment, grouped by table and addressed by offset
   ui64        blockCapacity; ///< Records allocated at blocks
   ui64        spanCapacity;  ///< Records allocated at spans
   ui64        tableCapacity; ///< Records allocated at tables
   ui64        rowCapacity;   ///< Records allocated at rows
   ui64        cellCapacity;  ///< Records allocated at cells
   ui64        noteCapacity;  ///< Records allocated at notes
   ui64        heapCapacity;  ///< Bytes allocated at heap
   ui64        heapUsed;      ///< Bytes of heap in use
   ui64        destCapacity;  ///< Bytes allocated at dest
   ui64        destUsed;      ///< Bytes of dest in use
   ui64        alignCapacity; ///< Bytes allocated at align
   ui64        alignUsed;     ///< Bytes of align in use
   ui32        blockCount;    ///< Blocks in blocks
   ui32        spanCount;     ///< Spans in spans
   ui32        tableCount;    ///< Tables in tables
   ui32        rowCount;      ///< Rows in rows
   ui32        cellCount;     ///< Cells in cells
   ui32        noteCount;     ///< Notes in notes
   si32        note;          ///< The note every block begun now belongs to, or -1 while the body is read
   bool        failed;        ///< Whether any append ran out of memory; sticky once set
};

// Zeroed with mzero, which dispatches on SIZE: a size that is a multiple of 32 takes a path of aligned
// 256-bit stores, so the object must be 32-byte aligned wherever it lives. al32 says so, and the
// assertion below keeps it said whatever a later field does to the size.
static_assert(alignof(IR_DOCUMENT) >= 32u, "Ir: IR_DOCUMENT is zeroed with mzero, whose 256-bit path needs 32-byte alignment.");

/// Constant and pointer forms of IR_DOCUMENT, spelled per GCS r2/t2.
typedef IR_DOCUMENT       *IR_DOCUMENTptr;
typedef const IR_DOCUMENT *cIR_DOCUMENTptr;
typedef IR_DOCUMENT *const IR_DOCUMENTptrc;

//== Entry points

/// Prepares an empty document.
/// @param document  Receives the document. Every field is written, so it need not be initialised, and
///                  IrClose is safe to call afterwards whatever happens next.
void IrOpen(IR_DOCUMENTptrc document);

/// Releases everything the document holds, and leaves it safe to close again.
/// @param document  A document previously passed to IrOpen.
void IrClose(IR_DOCUMENTptrc document);

/// Starts a block.
/// @param document      A prepared document.
/// @param kind          What the block is.
/// @param headingLevel  1 to 6 for a heading, 0 otherwise.
/// @return Where the block started. Its block field is -1 when the document could not grow, and the
///         document's failed flag is set; a caller may keep building and check the flag once at the end.
cIR_MARK IrBeginBlock(IR_DOCUMENTptrc document, cIR_BLOCK_KIND kind, cui8 headingLevel);

/// Ends a block, trimming it and dropping it when it holds nothing worth emitting.
/// @param document  A prepared document.
/// @param mark      What IrBeginBlock returned.
/// @return true when the block was kept, false when it was dropped.
/// @note A paragraph whose text is empty or nothing but ASCII whitespace is dropped whole, which is what
///       gives CONVERSION_REFERENCE row 40's "runs of N empty paragraphs collapse" for free: blocks are
///       separated by exactly one blank line, so a block that never existed leaves no gap.
/// @note A table block is exempt too, and for a reason of its own: it carries no spans at all, because
///       its content is the blocks of its cells rather than spans of its own. A table that turned out
///       to have no rows is unwound by the walker instead, which is where that is known.
/// @note Two kinds and one flag are exempt from the emptiness test. IR_BLOCK_RULE is an empty paragraph
///       by construction -- CONVERSION_REFERENCE row 25 makes it a lone w:pBdr bottom on a paragraph
///       with nothing in it -- so the test would throw away every one; its spans are dropped instead,
///       since a rule emits none. IR_BLOCK_CODE is kept because an empty code paragraph is a blank line
///       inside a fence, and the emitter trims one only where it falls at the fence's edge. And a block
///       carrying a list reference is kept because an empty list item is a marker on a line of its own,
///       which Word draws and CommonMark spells; dropping it would take the numbers of every item after
///       it down by one, which is the one thing M8 exists to compute correctly. The emitter trims a
///       content-free item off either *edge* of a list, which is where Word's own artefact lands -- the
///       paragraph a user leaves behind on pressing Enter to get out of a list.
/// @note Leading and trailing break spans are trimmed. A break at the end of a paragraph would emit a
///       hard-break marker with nothing after it, which is a stray backslash at the end of a line.
/// @note A non-breaking space counts as content. CONVERSION_REFERENCE row 35 keeps U+00A0 verbatim, and
///       a paragraph holding one was written to hold something.
/// @note An image counts as content and so does an anchor, because a paragraph holding nothing but a
///       picture is a picture and one holding nothing but a bookmark is a link target. A link's own two
///       markers do not: "[](url)" is a link with no text, which CONVERSION_REFERENCE 5.6 skips. An
///       anchor is judged again by IrDropEmptyBlocks once LinkResolve has muted the ones nothing points
///       at, and so are a note reference LinkResolveNotes muted and a picture MediaPlan turned into an
///       empty alt text; those are the ways a block can be emptied after it was ended.
cbool IrEndBlock(IR_DOCUMENTptrc document, cIR_MARK mark);

/// Starts a table on the block a mark named, which must have been begun as IR_BLOCK_TABLE.
/// @param document  A prepared document.
/// @param mark      What IrBeginBlock returned; a mark whose block is -1 yields -1.
/// @return Which table it is, or -1 when the document could not grow.
/// @note A table is its own block so that every pass above the walk keeps reading one flat array in
///       document order -- which is what NumAssignMarkers, LinkResolve and MediaPlan all rely on -- and
///       so that the emitter's top loop meets it where the document put it. What the block carries is
///       the index; the shape is in the table, the row and the cell records beside it.
csi32 IrBeginTable(IR_DOCUMENTptrc document, cIR_MARK mark);

/// Closes the table the walk has finished, recording every block it turned out to own.
/// @param document  A prepared document.
/// @param table     What IrBeginTable returned; a negative index does nothing.
/// @param lastRow   The table's last row, as IrBeginRow returned it, or -1 when it has none. The row
///                  chain is terminated there, which is what makes a rewound row disappear.
/// @param grid      How many w:gridCol the table declared, which is a floor and not a ceiling.
/// @note Called after the last cell, because blockEnd is only known then -- a cell's content is walked
///       where it stands, so a table owns every block between its own and whatever follows it.
/// @note Everything a table says about its own shape is **derived here**, by chasing the chains this
///       call has just terminated: how many columns it has, whether any cell merges, and what the
///       first row said about each column's alignment. None of it is accumulated as the walk goes, and
///       that is a correctness rule rather than tidiness -- an mc:AlternateContent may wrap a w:tr or
///       a w:tc, so a row or a cell that was rewound would otherwise leave behind a column the table
///       does not have, an alignment no surviving cell asked for, or a merge that was discarded with
///       the branch that declared it. IR_TABLE_NESTED is derived here too, by looking for a table
///       among the blocks each cell ended up holding rather than having each nested table mark its
///       parent as it closed -- which left the flag set on a parent whose only nested table a
///       mc:Fallback had since discarded, and emitted it as raw HTML it did not need.
/// @note The grid is authoritative for a table's width (CONVERSION_REFERENCE 2.5) but it is not a
///       ceiling: a row whose cells reach past it has columns the grid did not declare, and clamping
///       to the grid is the silent loss mapping row 19 forbids. The table is as wide as the wider.
void IrEndTable(IR_DOCUMENTptrc document, csi32 table, csi32 lastRow, cui32 grid);

/// Starts a row of one table, linking it behind the row before it.
/// @param document  A prepared document.
/// @param table     Which table, as IrBeginTable returned it.
/// @param after     The row this one follows, or -1 when it is the table's first.
/// @param header    Whether the row carried w:trPr/w:tblHeader.
/// @param skipBefore  The row's w:gridBefore, 0 when it has none; a value past IR_MAX_COLUMNS is clamped.
/// @param skipAfter   The row's w:gridAfter, 0 when it has none; clamped the same way.
/// @return Which row it is, or -1 when the document could not grow.
/// @note The caller holds the chain's tail rather than this module, and that is what makes a rewind
///       cost nothing: an mc:AlternateContent may wrap a w:tr, and the walker that unwinds a discarded
///       mc:Choice restores its own tail, so the next row links behind the row that really precedes it
///       and IrEndTable writes the terminator. Nothing here has to find a chain's severed end.
csi32 IrBeginRow(IR_DOCUMENTptrc document, csi32 table, csi32 after, cbool header, cui32 skipBefore, cui32 skipAfter);

/// Closes a row, terminating its cell chain.
/// @param document  A prepared document.
/// @param row       Which row, as IrBeginRow returned it; a negative index does nothing.
/// @param lastCell  The row's last cell, or -1 when it has none.
void IrEndRow(IR_DOCUMENTptrc document, csi32 row, csi32 lastCell);

/// Starts a cell of one row, linking it behind the cell before it.
/// @param document  A prepared document.
/// @param row       Which row, as IrBeginRow returned it.
/// @param after     The cell this one follows, or -1 when it is the row's first.
/// @param span      The w:gridSpan, clamped to at least 1.
/// @param flags     The IR_CELL bits the cell's w:tcPr named.
/// @return Which cell it is, or -1 when the document could not grow.
/// @note The cell's column is IrNextColumn's answer, because w:gridSpan says how many columns a cell
///       covers and nothing says which -- so a row is read left to right from its w:gridBefore and a cell
///       begins where its predecessor stopped. A row whose cells reach past the grid still reports each
///       one's start.
csi32 IrBeginCell(IR_DOCUMENTptrc document, csi32 row, csi32 after, cui32 span, cui8 flags);

/// The grid column the next cell of a row would start at.
/// @param document  A prepared document.
/// @param row       Which row, as IrBeginRow returned it.
/// @param after     The cell the next one would follow, or -1 when it would be the row's first.
/// @return Where the cell before it ended, or the row's w:gridBefore for its first cell; 0 for a row
///         outside the document.
/// @note Exposed so the walker can refuse a cell before any of it is stored. A cell starting at or past
///       IR_MAX_COLUMNS is outside every grid the emitter writes, so the walker skips it whole -- its
///       record, its content, and every picture, list item and note reference inside it -- which is the
///       cap on cells per row that M11 put beside the cap on columns. Without it a row could hold any
///       number of IR_CELL records at seven bytes of input apiece, and a picture in a cell nobody could
///       see was still extracted to disk.
cui32 IrNextColumn(cIR_DOCUMENTptr document, csi32 row, csi32 after);

/// Closes a cell, recording the blocks its content turned out to be.
/// @param document  A prepared document.
/// @param cell      Which cell, as IrBeginCell returned it; a negative index does nothing.
/// @param blockAt   The block count before the cell's content was walked.
/// @param align     The first alignment a w:jc of its paragraphs named. It is stored on the cell
///                  rather than spread over the table's columns here, because which row this cell is
///                  in is the caller's question and only the first row's cells reach a delimiter row.
void IrEndCell(IR_DOCUMENTptrc document, csi32 cell, cui32 blockAt, cIR_ALIGN align);

/// One table by index.
/// @return The table, or null for an index outside the document.
cIR_TABLEptr IrTableAt(cIR_DOCUMENTptr document, csi32 index);

/// One row by index.
/// @return The row, or null for an index outside the document, IR_NO_INDEX included -- which is what
///         ends a walk of a table's row chain.
cIR_ROWptr IrRowAt(cIR_DOCUMENTptr document, cui32 index);

/// One cell by index.
/// @return The cell, or null for an index outside the document, IR_NO_INDEX included -- which is what
///         ends a walk of a row's cell chain.
cIR_CELLptr IrCellAt(cIR_DOCUMENTptr document, cui32 index);

/// What one table's column was aligned to.
/// @param document  A prepared document.
/// @param table     The table the column belongs to.
/// @param column    Which column.
/// @return The alignment, or IR_ALIGN_NONE for a column the table does not have, and for every column
///         of a table whose alignments could not be stored.
/// @note Only the first row's cells ever settle one, because a GFM delimiter row is the only place an
///       alignment can be written and it stands under the header. A cell spanning several columns
///       aligns all of them, which is the only reading available when one cell speaks for two.
/// @note IrEndTable fills the arena by chasing the first row it has just terminated, so a cell an
///       mc:Fallback replaced never speaks for a column the document does not have.
cIR_ALIGN IrAlignOf(cIR_DOCUMENTptr document, cIR_TABLEptr table, cui32 column);

/// Starts one note, so that every block begun until IrEndNote belongs to it.
/// @param document  A prepared document.
/// @param kind      Which story the note was read from.
/// @param id        Its w:id.
/// @param part      The part it was read from, which its references resolve against; -1 when there is none.
/// @return Which note it is, or -1 when the document could not grow.
/// @note A note is not a block. Its blocks are ordinary blocks appended after the body's, each stamped
///       with the note's index, so every pass above the walk keeps reading one flat array -- which is the
///       same bargain a table's cells strike, and for the same reason. Two readers group them:
///       LinkResolveNotes, which numbers each note's references in the order the notes were numbered, and
///       the emitter, which writes a note somewhere other than where the walk put it.
csi32 IrBeginNote(IR_DOCUMENTptrc document, cIR_NOTE_KIND kind, csi32 id, csi32 part);

/// Ends the note IrBeginNote started, so that the blocks begun after it belong to the body again.
/// @param document  A prepared document.
void IrEndNote(IR_DOCUMENTptrc document);

/// How many notes the document holds.
/// @return The count, or 0 for a document that was never opened.
cui32 IrNoteCount(cIR_DOCUMENTptr document);

/// One note by index.
/// @return The note, or null for an index outside the document.
cIR_NOTEptr IrNoteAt(cIR_DOCUMENTptr document, csi32 index);

/// One note by index, for the pass that numbers it.
/// @return The note, or null for an index outside the document.
IR_NOTEptr IrNoteMutable(IR_DOCUMENTptrc document, csi32 index);

/// Records the list reference a paragraph's w:numPr carried, on the block being built.
/// @param document  A prepared document.
/// @param mark      What IrBeginBlock returned for the block; a mark whose block is -1 does nothing.
/// @param numId     The w:numId exactly as written, 0 included; -1 records no reference at all.
/// @param level     The w:ilvl; a value above 8 is clamped, since that is every level the schema has.
/// @note Separate from IrBeginBlock because a paragraph's numbering is settled with the rest of its
///       w:pPr, before any content reaches the block, so the three arguments would be dead on every one
///       of the other calls that begin one. It clamps here so that nothing downstream has to.
/// @note This records a *reference*. NumAssignMarkers resolves it, which is the same division of labour
///       IR_SPAN_FLAG_REL gives a link's destination: the walk writes down what the part said and a
///       later pass, which can see the whole document, turns it into what the emitter writes.
void IrSetListRef(IR_DOCUMENTptrc document, cIR_MARK mark, csi32 numId, cui32 level);

/// Starts a span inside the block being built.
/// @param document  A prepared document.
/// @param kind      What the span is.
/// @param fmt       The IR_FMT bits in force.
/// @return true when the span was started, false when the document could not grow.
/// @note Spans are never merged here. Coalescing adjacent runs with identical formatting is M6's
///       RunCoalescer, and doing it early would hide the fragmentation M6 has to be measured against.
cbool IrAddSpan(IR_DOCUMENTptrc document, cIR_SPAN_KIND kind, cui32 fmt);

/// Appends bytes to the span most recently started.
/// @param document   A prepared document.
/// @param bytes      UTF-8 bytes; they are copied.
/// @param byteCount  How many.
/// @return true when they were appended, false when there is no span or the document could not grow.
/// @note One w:t can reach the walker as several text tokens -- a comment or a processing instruction
///       ends a run of character data -- so appending has to be possible after a span has been started.
cbool IrAppendText(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount);

/// Appends bytes to the destination of the span most recently started.
/// @param document   A prepared document.
/// @param bytes      UTF-8 bytes; they are copied.
/// @param byteCount  How many.
/// @return true when they were appended, false when there is no span or the document could not grow.
/// @note The twin of IrAppendText for the other of a span's two ranges. A destination is written once by
///       the walker, so the two never interleave on one span -- which is what lets both be plain offsets
///       into one arena rather than needing a range of their own.
cbool IrAppendDest(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount);

/// Replaces the destination of one span, wherever it stands.
/// @param document   A prepared document.
/// @param index      Which span, as an index into the document's spans.
/// @param bytes      UTF-8 bytes; they are copied to the end of the arena.
/// @param byteCount  How many.
/// @return true when the destination was replaced, false for an index outside the document or when the
///         arena could not grow.
/// @note The old bytes are left where they are rather than compacted. A destination is rewritten at most
///       twice per span -- LinkResolve, then MediaPlan -- so the waste is bounded by the document's own
///       link count, and compacting would move every offset in the arena.
/// @note This appends to the arena after the walk has finished, which is safe only because RunCoalesce
///       has already run: that pass is the one thing relying on a block's text spans lying end to end,
///       and it checks the invariant rather than assuming it.
/// @note The bytes may be inside the arena they are being copied into. That is what LinkResolve does
///       when it puts a heading's slug on the anchor that reaches it, and growing the arena would
///       otherwise free the block being read from; the offset is remembered across the growth.
cbool IrSetDest(IR_DOCUMENTptrc document, cui32 index, cchptr bytes, cui64 byteCount);

/// Appends bytes to the destination arena, for a pass that has to build one out of several pieces.
/// @param document   A prepared document.
/// @param bytes      UTF-8 bytes; they are copied.
/// @param byteCount  How many.
/// @return Where they landed, or -1 when the arena could not grow.
/// @note LinkResolve stores a heading's slug here so that the anchor pointing at it and the index that
///       deduplicates it can both address it by offset -- an offset survives the growth an address
///       does not, which is what makes one arena serve a pass that keeps adding to it.
csi64 IrStoreDest(IR_DOCUMENTptrc document, cchptr bytes, cui64 byteCount);

/// Records where the document currently ends, so that a speculative walk can be undone.
/// @param document  A prepared document.
/// @return The current block count, span count and arena size.
/// @note This is what makes mc:AlternateContent right with a parser that cannot look ahead: the first
///       mc:Choice is walked into the document, and if an mc:Fallback turns up afterwards the Choice's
///       output is rewound and the Fallback walked in its place.
cIR_MARK IrMark(cIR_DOCUMENTptr document);

/// Undoes everything added since a mark.
/// @param document  A prepared document.
/// @param mark      What IrMark or IrBeginBlock returned.
/// @note Only whole blocks and whole spans are unwound. A mark taken in the middle of a block leaves
///       that block's own record alone, because its span range is not written until IrEndBlock.
void IrRewind(IR_DOCUMENTptrc document, cIR_MARK mark);

/// How many blocks the document holds.
/// @return The count, or 0 for a document that was never opened.
cui32 IrBlockCount(cIR_DOCUMENTptr document);

/// How many spans the document holds, over every block.
/// @return The count, or 0 for a document that was never opened.
cui32 IrSpanCount(cIR_DOCUMENTptr document);

/// Whether a range of spans puts anything on the page.
/// @param document  A prepared document.
/// @param first     The first span of the range.
/// @param last      One past its last span; a value beyond the document is clamped.
/// @return true when the range holds an image, a note reference nothing has muted, or text that is not
///         all ASCII whitespace.
/// @note An anchor does not count, which is the one place this and the emptiness test IrEndBlock
///       applies come apart. The walker asks this to decide mapping row 25's horizontal rule, which
///       is about a paragraph that came to nothing -- and a paragraph holding one bookmark did,
///       even though the bookmark is reason enough to keep the block.
cbool IrHasInk(cIR_DOCUMENTptr document, cui32 first, cui32 last);

/// Whether a range of spans holds anything worth keeping a block for.
/// @param document  A prepared document.
/// @param first     The first span of the range.
/// @param last      One past its last span; a value beyond the document is clamped.
/// @return true when the range holds ink, or an anchor nothing has muted.
/// @note The wider twin of IrHasInk, and the test IrEndBlock and IrDropEmptyBlocks themselves apply. A
///       caller trimming a block off the edge of a group asks this one and not the narrower one, or an
///       item holding nothing but a picture or a live bookmark would be trimmed away as empty.
cbool IrHasContent(cIR_DOCUMENTptr document, cui32 first, cui32 last);

/// One block by index.
/// @return The block, or null for an index outside the document.
cIR_BLOCKptr IrBlockAt(cIR_DOCUMENTptr document, cui32 index);

/// One span by index.
/// @return The span, or null for an index outside the document.
cIR_SPANptr IrSpanAt(cIR_DOCUMENTptr document, cui32 index);

/// One block by index, for a caller that has to rewrite it.
/// @return The block, or null for an index outside the document.
/// @note The mutable twin of IrBlockAt. RunCoalescer rewrites a block's span range, and the walker
///       settles a paragraph's kind only once every one of its runs has been seen -- neither can be
///       expressed through a const view, and neither is worth a second entry point per field.
IR_BLOCKptr IrBlockMutable(IR_DOCUMENTptrc document, cui32 index);

/// One span by index, for a caller that has to rewrite it.
/// @return The span, or null for an index outside the document.
/// @note The mutable twin of IrSpanAt, and the same bargain IrBlockMutable strikes. LinkResolve mutes an
///       anchor nothing points at and MediaPlan clears a destination's part flag; neither is a field a
///       const view can reach, and neither is worth an entry point of its own.
IR_SPANptr IrSpanMutable(IR_DOCUMENTptrc document, cui32 index);

/// Drops every block that no longer holds anything worth emitting.
/// @param document  A prepared document.
/// @note IrEndBlock keeps the invariant the emitter rests on -- every block it is handed produces at
///       least one byte -- and later passes can break it after the fact: LinkResolve mutes an anchor
///       nothing points at, LinkResolveNotes mutes a note reference whose note the document does not
///       hold, and MediaPlan can leave an image with nothing to show. Re-testing every block here
///       restores the invariant in one place rather than making the emitter carry a case for a block
///       that emits nothing.
/// @note The exemptions are IrEndBlock's, and the two tests have to agree or a block that survived
///       being ended would be thrown away on the second look. A list item is exempt only while it carries
///       a reference, and only a real item does by the time this runs: the walk records a reference only
///       for a w:numId the numbering part resolves, and NumAssignMarkers clears any other before this, so
///       an empty paragraph whose w:numId named nothing is dropped like any other while an empty *item*
///       keeps its marker.
/// @note Blocks keep their order and their spans; only the records move down over the dropped ones. The
///       arena is not compacted, for the same reason IrSetDest does not compact it.
/// @note Nothing inside a table is ever dropped, and that is what makes the table records survive the
///       compaction. A cell's blocks are named by index, so a dropped one would have to be found again
///       in every record that could reach past it; instead the whole of a table moves as a unit and
///       every record of it is shifted by the one delta that applies where the table stands. What a
///       cell then has to cope with is a block that emits nothing, which is an empty cell -- a shape
///       a table has anyway, and which the emitter already has to write.
void IrDropEmptyBlocks(IR_DOCUMENTptrc document);

/// The bytes one text-arena offset names.
/// @return The bytes, or an empty string when the arena is empty. Never null, never NUL-terminated at
///         the span's own end.
cchptr IrText(cIR_DOCUMENTptr document, cui32 at);

/// The bytes one destination-arena offset names.
/// @return The bytes, or an empty string when the arena is empty. Never null, never NUL-terminated at
///         the span's own end.
cchptr IrDest(cIR_DOCUMENTptr document, cui32 at);

/// Whether any append ran out of memory.
/// @return true once an append has failed, and from then on. A failed document holds no usable IR.
cbool IrFailed(cIR_DOCUMENTptr document);

/// Marks the document as out of memory, for a pass that allocates on its behalf.
/// @param document  A prepared document.
/// @note RunCoalescer builds a new span array, and a failure there has to reach the same sticky flag
///       every other allocation in this module reports through, or a caller would have to test two.
void IrFail(IR_DOCUMENTptrc document);

/// Replaces the span array wholesale, taking ownership of the replacement.
/// @param document  A prepared document.
/// @param spans     A block amalloc returned, holding count spans; the document frees it from now on.
/// @param capacity  How many spans were allocated at spans, which is what IrClose has to release.
/// @param count     How many of them are in use.
/// @note This is RunCoalescer's one privilege. Hoisting whitespace out of a formatted span splits it in
///       three, so the pass cannot rewrite the array in place, and every block's spanAt moves with it --
///       which is why the caller rewrites the blocks in the same pass and hands the finished array over.
void IrAdoptSpans(IR_DOCUMENTptrc document, IR_SPANptr spans, cui64 capacity, cui32 count);
