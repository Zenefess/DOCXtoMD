/*
 * File: ZipReader.h
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-24
 * Description: First-party ZIP container reader: directory parsing, entry lookup, and verified extraction.
 * To Do: 1) Validate entry names against ZIP path-traversal shapes at M11, which decision D10 gave the question to.
 *        2) Expose the decompression caps on the command line as --max-decompressed and friends.
 *        3) Read entry names as CP437 when the UTF-8 flag is clear; every OPC part name seen so far is ASCII.
 * Dependencies: Inflate.h, typedefs.h
 * ISA: Scalar
 * Thread-safety: Reentrant
 * Reviewers: David William Bull
 * License: MIT  Copyright: David William Bull
 */
#pragma once

#include "typedefs.h"
#include "Inflate.h"

//== Results

/// Why a container operation stopped. Every value but ZIP_OK means nothing usable was produced.
/// @note ZIP_ERROR_OPEN maps to exit code 2, and ZIP_ERROR_MEMORY and ZIP_ERROR_RANGE to 5; every other
///       failure is exit code 3, because it describes a file that is not a usable DOCX rather than a
///       problem with the environment or with this reader's own callers.
enum ZIP_RESULT : si32 {
   ZIP_OK = 0,          ///< The operation succeeded
   ZIP_ERROR_OPEN,      ///< The file could not be opened or read
   ZIP_ERROR_MEMORY,    ///< An allocation failed
   ZIP_ERROR_NOT_ZIP,   ///< No end-of-central-directory record; this is not a ZIP archive
   ZIP_ERROR_OLE,       ///< An OLE compound file: an encrypted .docx, or a legacy .doc
   ZIP_ERROR_ENCRYPTED, ///< An entry is encrypted, so its bytes cannot be read
   ZIP_ERROR_METHOD,    ///< An entry uses a compression method other than stored or deflate
   ZIP_ERROR_MALFORMED, ///< A record's signature, ordering or field values are inconsistent
   ZIP_ERROR_TRUNCATED, ///< A record or an entry's data reaches past the end of the file
   ZIP_ERROR_SIZE,      ///< An entry inflated to a different size than its directory entry declares
   ZIP_ERROR_INFLATE,   ///< An entry's deflate stream is corrupt; the reader records which rule broke
   ZIP_ERROR_CRC,       ///< An entry's bytes do not match the CRC-32 its directory entry declares
   ZIP_ERROR_LIMIT,     ///< A decompression cap was reached, so the archive is refused as a bomb
   ZIP_ERROR_TOO_LARGE, ///< The file is bigger than the reader will load; a size problem, not a bomb
   ZIP_ERROR_RANGE      ///< An entry index outside the archive was asked for; a caller mistake
};

/// Constant form of ZIP_RESULT, spelled per GCS r2: the qualifier lives in the typedef.
typedef const ZIP_RESULT cZIP_RESULT;

//== Limits

/// The caps that stop a decompression bomb. Every one is checked before an entry is read and again, by
/// construction, while it inflates: the output buffer is exactly the declared size, so a stream that lies
/// about how much it produces is stopped at the byte that overruns rather than trusted and measured after.
struct ZIP_LIMITS {
   ui64 maxArchiveBytes; ///< Largest .docx that will be read into memory at all
   ui64 maxTotalBytes;   ///< Largest total of inflated bytes across every entry read from one archive
   ui64 maxEntryBytes;   ///< Largest inflated size of a single entry
   ui64 ratioFloorBytes; ///< Entries inflating to less than this are exempt from the ratio cap
   ui32 maxEntries;      ///< Largest number of central directory records
   ui32 maxRatio;        ///< Largest inflated-to-stored ratio allowed above ratioFloorBytes
};

/// Constant and pointer forms of ZIP_LIMITS, spelled per GCS r2/t2.
typedef const ZIP_LIMITS        cZIP_LIMITS;
typedef const ZIP_LIMITS       *cZIP_LIMITSptr;
typedef const ZIP_LIMITS *const cZIP_LIMITSptrc;

/// The caps a .docx is read under unless a caller says otherwise. Sized from the figures
/// docs/CONVERSION_REFERENCE.md 5.12 recommends; no real document comes near any of them.
constexpr cZIP_LIMITS ZIP_DEFAULT_LIMITS = {1024ull << 20, 512ull << 20, 256ull << 20, 1ull << 20, 10000u, 1000u};

//== Entries

/// One central directory record, reduced to the fields a converter needs. Sizes and CRC come from the
/// central directory, which is authoritative: a streamed entry leaves its local header's copies at zero.
struct ZIP_ENTRY {
   cchptr name;              ///< NUL-terminated entry name; owned by the reader, valid until ZipClose
   ui64   compressedBytes;   ///< Bytes of stored or deflated data
   ui64   uncompressedBytes; ///< Bytes the entry declares it inflates to
   ui64   localHeaderOffset; ///< Offset of the entry's local file header within the archive
   ui32   crc32;             ///< CRC-32 the entry declares over its uncompressed bytes
   ui32   nameBytes;         ///< Bytes in name, excluding the terminator
   ui16   method;            ///< 0 stored or 8 deflate; anything else is refused when the entry is read
   ui16   flags;             ///< General purpose bit flag, as stored
};

/// Constant and pointer forms of ZIP_ENTRY, spelled per GCS r2/t2.
typedef const ZIP_ENTRY        cZIP_ENTRY;
typedef ZIP_ENTRY             *ZIP_ENTRYptr;
typedef const ZIP_ENTRY       *cZIP_ENTRYptr;
typedef ZIP_ENTRY *const       ZIP_ENTRYptrc;
typedef const ZIP_ENTRY *const cZIP_ENTRYptrc;

//== Reader

/// One archive held in memory, with its central directory parsed. One worker owns one of these and never
/// shares it, which is why nothing here takes a lock (D6).
struct ZIP_READER {
   ui8ptr         bytes;         ///< The whole archive
   ui64           byteCount;     ///< Bytes in bytes
   ZIP_ENTRYptr   entries;       ///< Central directory records, in central directory order
   chptr          nameHeap;      ///< Backing store every entry's name points into
   ui64           inflatedBytes; ///< Running total of inflated bytes, checked against maxTotalBytes
   ZIP_LIMITS     limits;        ///< The caps this archive is being read under
   INFLATE_RESULT lastInflate;   ///< Which RFC 1951 rule broke, when a read returned ZIP_ERROR_INFLATE
   ui32           entryCount;    ///< Number of entries
};

/// Constant and pointer forms of ZIP_READER, spelled per GCS r2/t2.
typedef ZIP_READER       *ZIP_READERptr;
typedef const ZIP_READER *cZIP_READERptr;
typedef ZIP_READER *const ZIP_READERptrc;

//== Entry points

/// Reads an archive into memory and parses its central directory.
/// @param reader  Receives the opened archive. Every field is written before anything can fail, so
///                ZipClose is safe to call whatever this returns.
/// @param path    Path to the file, wide so that names outside the active code page survive.
/// @param limits  The caps to read under; pass &ZIP_DEFAULT_LIMITS unless a caller has its own.
/// @return ZIP_OK, or why the archive was refused. No entry data is read here -- parts are inflated one at
///         a time by ZipReadEntry, so a package's unused media never costs anything.
cZIP_RESULT ZipOpen(ZIP_READERptrc reader, cwchptr path, cZIP_LIMITSptrc limits);

/// Releases everything an opened archive holds, and leaves the reader safe to close again.
/// @param reader  A reader previously passed to ZipOpen, whatever that returned.
void ZipClose(ZIP_READERptrc reader);

/// Finds an entry by exact name.
/// @param reader  An opened archive.
/// @param name    NUL-terminated entry name, compared byte for byte.
/// @return The entry's index, or -1 when the archive has no such entry.
/// @note Duplicate names resolve to the first record in central directory order, which is the reader's
///       documented tie-break. OPC's case-insensitive part-name comparison is deliberately not applied
///       here: normalising part names belongs to OpcPackage, which M4 adds.
csi32 ZipFindEntry(cZIP_READERptr reader, cchptr name);

/// Extracts one entry, verifying it as it goes.
/// @param reader     An opened archive; its running inflated-byte total is updated on success.
/// @param index      An entry index, as ZipFindEntry returns.
/// @param bytes      Receives a buffer holding the entry's uncompressed bytes; release it with mdealloc.
/// @param byteCount  Receives the number of bytes in that buffer.
/// @return ZIP_OK, or why the entry was refused. Nothing is allocated unless this returns ZIP_OK.
/// @note Three independent checks have to agree before the bytes are handed over: the stream may not write
///       past the declared size, it must reach exactly that size, and the CRC-32 must match. That is what
///       makes the declared sizes safe to allocate against -- they are never simply believed.
/// @note The run total is charged when the entry is accepted, not when it succeeds, so a stream that
///       inflates most of a cap's worth and then fails its CRC-32 still costs what it spent.
cZIP_RESULT ZipReadEntry(ZIP_READERptrc reader, cui32 index, ui8ptrptrc bytes, ui64ptrc byteCount);

/// The user-facing sentence for a result, ready to hand to DiagErrorText.
/// @param reader  The reader the result came from; a null pointer still yields a usable sentence.
/// @param result  The result to describe.
/// @return A static, NUL-terminated ASCII sentence with no trailing punctuation. For ZIP_ERROR_INFLATE the
///         reader's record of which RFC 1951 rule broke is folded in, so one call gives the best message
///         available. The wording lives here rather than in Inflate because this is the layer that knows
///         the stream came out of a .docx.
cchptr ZipResultText(cZIP_READERptr reader, cZIP_RESULT result);
