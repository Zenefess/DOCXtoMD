/*
 * File: ZipReader.cpp
 * Version: v0.1.0
 * Owner: David William Bull
 * Created: 2026-08-19
 * Last Modified: 2026-08-19
 * Description: ZIP container reader: EOCD and ZIP64 discovery, directory parsing, and capped extraction.
 * To Do: 1) Return the parsed parts to OpcPackage rather than to a probe, once M4 resolves them by relationship.
 *        2) Expose the decompression caps on the command line as --max-decompressed and friends.
 *        3) Read entry names as CP437 when the UTF-8 flag is clear; every OPC part name seen so far is ASCII.
 * Dependencies: BuildGuards.h, Crc32.h, Inflate.h, ZipReader.h, typedefs.h, memory management.h, windows.h
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
#include "Crc32.h"
#include "Inflate.h"
#include "ZipReader.h"

//== Format constants

// Record signatures, as they read once the four little-endian bytes are assembled.
constexpr cui32 ZIP_SIG_LOCAL     = 0x04034B50u; // PK\3\4  local file header
constexpr cui32 ZIP_SIG_CENTRAL   = 0x02014B50u; // PK\1\2  central directory record
constexpr cui32 ZIP_SIG_EOCD      = 0x06054B50u; // PK\5\6  end of central directory
constexpr cui32 ZIP_SIG_EOCD64    = 0x06064B50u; // PK\6\6  ZIP64 end of central directory
constexpr cui32 ZIP_SIG_LOCATOR64 = 0x07064B50u; // PK\6\7  ZIP64 end of central directory locator

// An OLE compound file, which is what a password-protected .docx and a legacy .doc both are.
constexpr cui32 ZIP_OLE_MAGIC_LOW  = 0xE011CFD0u; // D0 CF 11 E0
constexpr cui32 ZIP_OLE_MAGIC_HIGH = 0xE11AB1A1u; // A1 B1 1A E1

// Fixed portions of each record, before the variable-length name, extra and comment fields.
constexpr cui64 ZIP_LOCAL_FIXED   = 30u;
constexpr cui64 ZIP_CENTRAL_FIXED = 46u;
constexpr cui64 ZIP_EOCD_FIXED    = 22u;
constexpr cui64 ZIP_EOCD64_FIXED  = 56u;
constexpr cui64 ZIP_LOCATOR_FIXED = 20u;

// The end-of-central-directory record may be followed by a comment of up to 65535 bytes, so the search
// window is that plus the record itself.
constexpr cui64 ZIP_EOCD_SEARCH = ZIP_EOCD_FIXED + 65535u;

// Field offsets within the end-of-central-directory record.
constexpr cui64 ZIP_EOCD_DISK        = 4u;
constexpr cui64 ZIP_EOCD_CD_DISK     = 6u;
constexpr cui64 ZIP_EOCD_TOTAL       = 10u;
constexpr cui64 ZIP_EOCD_CD_SIZE     = 12u;
constexpr cui64 ZIP_EOCD_CD_OFFSET   = 16u;
constexpr cui64 ZIP_EOCD_COMMENT_LEN = 20u;

// Field offsets within the ZIP64 end-of-central-directory record and its locator.
constexpr cui64 ZIP_EOCD64_TOTAL     = 32u;
constexpr cui64 ZIP_EOCD64_CD_SIZE   = 40u;
constexpr cui64 ZIP_EOCD64_CD_OFFSET = 48u;
constexpr cui64 ZIP_LOCATOR_RECORD   = 8u;

// Field offsets within a central directory record.
constexpr cui64 ZIP_CD_FLAGS        = 8u;
constexpr cui64 ZIP_CD_METHOD       = 10u;
constexpr cui64 ZIP_CD_CRC          = 16u;
constexpr cui64 ZIP_CD_COMPRESSED   = 20u;
constexpr cui64 ZIP_CD_UNCOMPRESSED = 24u;
constexpr cui64 ZIP_CD_NAME_LEN     = 28u;
constexpr cui64 ZIP_CD_EXTRA_LEN    = 30u;
constexpr cui64 ZIP_CD_COMMENT_LEN  = 32u;
constexpr cui64 ZIP_CD_DISK_START   = 34u;
constexpr cui64 ZIP_CD_LOCAL_OFFSET = 42u;

// Field offsets within a local file header.
constexpr cui64 ZIP_LOCAL_NAME_LEN  = 26u;
constexpr cui64 ZIP_LOCAL_EXTRA_LEN = 28u;

// The all-ones values that stand in for a field too large for its 32- or 16-bit slot, and the extra
// field that carries the real one.
constexpr cui64 ZIP_MARKER32    = 0xFFFFFFFFull;
constexpr cui32 ZIP_MARKER16    = 0xFFFFu;
constexpr cui32 ZIP_EXTRA_ZIP64 = 0x0001u;

// General purpose bit flags. Bit 0 is traditional PKWARE encryption, bit 6 strong encryption, and bit 13
// masks the local header's copies of the sizes -- all three mean the bytes cannot simply be read. Bit 3,
// the data descriptor, is deliberately absent: it says only that the local header's sizes are zero, and
// the central directory this reader trusts carries the real ones either way.
constexpr cui32 ZIP_FLAGS_ENCRYPTED = 0x2041u;

constexpr cui16 ZIP_METHOD_STORED  = 0u;
constexpr cui16 ZIP_METHOD_DEFLATE = 8u;

// Bytes per ReadFile call. A single call takes a DWORD count, and a smaller ceiling keeps a short read on
// a network path from being mistaken for the end of the file.
constexpr cui64 ZIP_READ_CHUNK = 0x04000000ull;

// The sentence a caller shows for each way a deflate stream can be corrupt, indexed by INFLATE_RESULT.
// The wording lives here because this is the layer that knows the stream came out of a .docx.
static constexpr cchptr ZIP_INFLATE_TEXT[INFLATE_RESULT_COUNT] = {
    // One sentence per INFLATE_RESULT, in the order the enum declares them
    "the deflate stream is intact",                                                            // INFLATE_OK
    "not a valid DOCX; a deflate stream ends before its data does",                            // TRUNCATED
    "not a valid DOCX; a deflate stream produces more data than its directory entry declares", // OVERFLOW
    "not a valid DOCX; a deflate stream uses the reserved block type",                         // BLOCK_TYPE
    "not a valid DOCX; a stored deflate block's length field is corrupt",                      // STORED_LENGTH
    "not a valid DOCX; a deflate stream's code-length table is corrupt",                       // CODE_LENGTHS
    "not a valid DOCX; a deflate stream's Huffman code is corrupt",                            // HUFFMAN
    "not a valid DOCX; a deflate stream uses an undefined length symbol",                      // SYMBOL
    "not a valid DOCX; a deflate stream matches data from before the start of the entry"       // DISTANCE
};

//-- Little-endian field readers

// ZIP stores every integer field little-endian whatever the host is, so the bytes are assembled by hand
// rather than punned through a pointer -- which would also be an unaligned read. ZipRead16 returns ui32,
// not ui16: every caller widens or compares against a ui32, and returning the narrow type would leave
// those expressions relying on integer promotion instead of saying what they mean.
static cui32 ZipRead16(cui8ptr at) { return ui32(at[0]) | (ui32(at[1]) << 8u); }

static cui32 ZipRead32(cui8ptr at) { return ui32(at[0]) | (ui32(at[1]) << 8u) | (ui32(at[2]) << 16u) | (ui32(at[3]) << 24u); }

static cui64 ZipRead64(cui8ptr at) { return ui64(ZipRead32(at)) | (ui64(ZipRead32(at + 4u)) << 32u); }

// Exact comparison of two NUL-terminated entry names.
static cbool ZipNameEqual(cchptr a, cchptr b) {
   ui64 i = 0;

   while(a[i] && a[i] == b[i]) ++i;
   return a[i] == b[i];
}

//-- Signature checks

// Reports whether the file starts with the OLE compound file magic, which is what both a password-
// protected .docx and a legacy .doc are. Naming that is far more use than reporting a missing directory.
static cbool ZipIsOleCompoundFile(cZIP_READERptr reader) {
   if(reader->byteCount < 8u) return false;
   return ZipRead32(reader->bytes) == ZIP_OLE_MAGIC_LOW && ZipRead32(reader->bytes + 4u) == ZIP_OLE_MAGIC_HIGH;
}

//-- File reading

// Reads the whole archive into one aligned block. A .docx is small enough that streaming buys nothing,
// and having every byte addressable is what lets the directory be validated before anything is inflated.
static cZIP_RESULT ZipReadWholeFile(cwchptr path, ui8ptrptrc bytes, ui64ptrc byteCount, cui64 maxBytes) {
   cHANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

   if(file == INVALID_HANDLE_VALUE) return ZIP_ERROR_OPEN;

   LARGE_INTEGER size = {};

   if(!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
      CloseHandle(file);
      return ZIP_ERROR_OPEN;
   }
   if(ui64(size.QuadPart) > maxBytes) {
      CloseHandle(file);
      return ZIP_ERROR_TOO_LARGE; // A size problem, not necessarily an attack, and worth saying so
   }
   if(ui64(size.QuadPart) < ZIP_EOCD_FIXED) {
      CloseHandle(file);
      return ZIP_ERROR_NOT_ZIP;
   }

   cui64  total  = ui64(size.QuadPart);
   ui8ptr buffer = (ui8ptr)amalloc(total, 64u);

   if(!buffer) {
      CloseHandle(file);
      return ZIP_ERROR_MEMORY;
   }

   ui64 done = 0;

   while(done < total) {
      cui64 want = ((total - done) > ZIP_READ_CHUNK ? ZIP_READ_CHUNK : (total - done));
      DWORD got  = 0;

      // A zero-byte read with no error means the file shrank under us, which would otherwise spin here.
      if(!ReadFile(file, buffer + done, DWORD(want), &got, nullptr) || !got) {
         mdealloc(buffer);
         CloseHandle(file);
         return ZIP_ERROR_OPEN;
      }
      done += ui64(got);
   }
   CloseHandle(file);
   *bytes     = buffer;
   *byteCount = total;
   return ZIP_OK;
}

//-- Directory discovery

// Finds the end-of-central-directory record by scanning backwards over the last 65557 bytes. The record
// whose comment length accounts for exactly the rest of the file is preferred, because the signature can
// also occur inside compressed data; the highest bare signature is the fallback.
static csi64 ZipFindEocd(cui8ptr bytes, cui64 byteCount) {
   if(byteCount < ZIP_EOCD_FIXED) return -1;

   cui64 window = (byteCount < ZIP_EOCD_SEARCH ? byteCount : ZIP_EOCD_SEARCH);
   cui64 lowest = byteCount - window;
   si64  loose  = -1;

   for(ui64 at = byteCount - ZIP_EOCD_FIXED + 1u; at-- > lowest;) {
      if(ZipRead32(bytes + at) != ZIP_SIG_EOCD) continue;

      cui64 comment = ui64(ZipRead16(bytes + at + ZIP_EOCD_COMMENT_LEN));

      if(at + ZIP_EOCD_FIXED + comment == byteCount) return si64(at);
      if(loose < 0) loose = si64(at);
   }
   return loose;
}

// Reads the ZIP64 extended information extra field, which stands in for any central directory field too
// large for its slot. The values appear in a fixed order, and only those whose slot holds the all-ones
// marker are present at all.
static cbool ZipReadZip64Extra(cui8ptr extra, cui64 extraBytes, ui64ptrc uncompressed, ui64ptrc compressed, ui64ptrc headerAt, ui32ptrc diskStart) {
   ui64 at = 0;

   while(at + 4u <= extraBytes) {
      cui32 id   = ZipRead16(extra + at);
      cui64 size = ui64(ZipRead16(extra + at + 2u));

      if(at + 4u + size > extraBytes) return false;
      if(id != ZIP_EXTRA_ZIP64) {
         at += 4u + size;
         continue;
      }

      cui8ptr field = extra + at + 4u;
      ui64    used  = 0;

      if(*uncompressed == ZIP_MARKER32) {
         if(used + 8u > size) return false;
         *uncompressed = ZipRead64(field + used);
         used += 8u;
      }
      if(*compressed == ZIP_MARKER32) {
         if(used + 8u > size) return false;
         *compressed = ZipRead64(field + used);
         used += 8u;
      }
      if(*headerAt == ZIP_MARKER32) {
         if(used + 8u > size) return false;
         *headerAt = ZipRead64(field + used);
         used += 8u;
      }
      if(*diskStart == ZIP_MARKER16) {
         if(used + 4u > size) return false;
         *diskStart = ZipRead32(field + used);
      }
      return true;
   }
   // No ZIP64 extra field. Any marker still standing has nothing to resolve it, which is malformed.
   return *uncompressed != ZIP_MARKER32 && *compressed != ZIP_MARKER32 && *headerAt != ZIP_MARKER32;
}

// Walks the records once without recording anything, so the name heap can be sized exactly rather than
// from the directory's declared extent. A directory padded with extra fields or comments would otherwise
// reserve every one of those bytes for names that do not exist, which is an allocation a hostile archive
// gets to choose. It also proves the chain is well formed before a single byte is allocated for it.
static cZIP_RESULT ZipMeasureCentral(cui8ptr bytes, cui64 cdOffset, cui64 cdBytes, cui32 count, ui64ptrc nameTotal) {
   cui64 limit = cdOffset + cdBytes;
   ui64  at    = cdOffset;
   ui64  total = 0;

   for(ui32 i = 0; i < count; ++i) {
      if(at + ZIP_CENTRAL_FIXED > limit) return ZIP_ERROR_TRUNCATED;
      if(ZipRead32(bytes + at) != ZIP_SIG_CENTRAL) return ZIP_ERROR_MALFORMED;

      cui64 nameBytes  = ui64(ZipRead16(bytes + at + ZIP_CD_NAME_LEN));
      cui64 extraBytes = ui64(ZipRead16(bytes + at + ZIP_CD_EXTRA_LEN));
      cui64 commentLen = ui64(ZipRead16(bytes + at + ZIP_CD_COMMENT_LEN));

      if(at + ZIP_CENTRAL_FIXED + nameBytes + extraBytes + commentLen > limit) return ZIP_ERROR_TRUNCATED;
      if(!nameBytes) return ZIP_ERROR_MALFORMED;
      total += nameBytes + 1u;
      at += ZIP_CENTRAL_FIXED + nameBytes + extraBytes + commentLen;
   }
   *nameTotal = total;
   return ZIP_OK;
}

// Walks the central directory, recording every entry. Names are copied into one heap block so a caller
// never holds a pointer into the archive bytes, and every record is bounded against the directory extent
// before a single field is trusted.
static cZIP_RESULT ZipParseCentral(ZIP_READERptrc reader, cui64 cdOffset, cui64 cdBytes, cui64 declared) {
   if(declared > ui64(reader->limits.maxEntries)) return ZIP_ERROR_LIMIT;
   if(cdOffset > reader->byteCount || cdBytes > reader->byteCount - cdOffset) return ZIP_ERROR_TRUNCATED;

   cui32 count     = ui32(declared);
   ui64  nameTotal = 0;

   cZIP_RESULT measured = ZipMeasureCentral(reader->bytes, cdOffset, cdBytes, count, &nameTotal);

   if(measured != ZIP_OK) return measured;

   // Exactly the bytes the names need, plus one so a directory with no entries still allocates something.
   reader->entries  = (ZIP_ENTRYptr)amalloc(sizeof(ZIP_ENTRY) * ui64(count ? count : 1u), 32u);
   reader->nameHeap = (chptr)amalloc(nameTotal + 1u, 16u);
   if(!reader->entries || !reader->nameHeap) return ZIP_ERROR_MEMORY;

   cui8ptr bytes = reader->bytes;
   cui64   limit = cdOffset + cdBytes;
   ui64    at    = cdOffset;
   ui64    used  = 0;

   for(ui32 i = 0; i < count; ++i) {
      if(at + ZIP_CENTRAL_FIXED > limit) return ZIP_ERROR_TRUNCATED;
      if(ZipRead32(bytes + at) != ZIP_SIG_CENTRAL) return ZIP_ERROR_MALFORMED;

      cui32 flags      = ZipRead16(bytes + at + ZIP_CD_FLAGS);
      cui64 nameBytes  = ui64(ZipRead16(bytes + at + ZIP_CD_NAME_LEN));
      cui64 extraBytes = ui64(ZipRead16(bytes + at + ZIP_CD_EXTRA_LEN));
      cui64 commentLen = ui64(ZipRead16(bytes + at + ZIP_CD_COMMENT_LEN));
      cui64 recordEnd  = at + ZIP_CENTRAL_FIXED + nameBytes + extraBytes + commentLen;

      if(recordEnd > limit) return ZIP_ERROR_TRUNCATED;
      if(!nameBytes) return ZIP_ERROR_MALFORMED;
      if(flags & ZIP_FLAGS_ENCRYPTED) return ZIP_ERROR_ENCRYPTED;

      cui8ptrc nameAt  = bytes + at + ZIP_CENTRAL_FIXED;
      cui8ptrc extraAt = nameAt + nameBytes;

      ui64 uncompressed = ui64(ZipRead32(bytes + at + ZIP_CD_UNCOMPRESSED));
      ui64 compressed   = ui64(ZipRead32(bytes + at + ZIP_CD_COMPRESSED));
      ui64 headerAt     = ui64(ZipRead32(bytes + at + ZIP_CD_LOCAL_OFFSET));
      ui32 diskStart    = ZipRead16(bytes + at + ZIP_CD_DISK_START);

      if(!ZipReadZip64Extra(extraAt, extraBytes, &uncompressed, &compressed, &headerAt, &diskStart)) return ZIP_ERROR_MALFORMED;
      if(diskStart) return ZIP_ERROR_MALFORMED; // Spanned archives are not read

      chptrc name = reader->nameHeap + used;

      for(ui64 c = 0; c < nameBytes; ++c) {
         if(!nameAt[c]) return ZIP_ERROR_MALFORMED; // An embedded NUL would silently truncate the name
         name[c] = char(nameAt[c]);
      }
      name[nameBytes] = '\0';
      used += nameBytes + 1u;

      ZIP_ENTRYptrc entry = &reader->entries[i];

      entry->name              = name;
      entry->compressedBytes   = compressed;
      entry->uncompressedBytes = uncompressed;
      entry->localHeaderOffset = headerAt;
      entry->crc32             = ZipRead32(bytes + at + ZIP_CD_CRC);
      entry->nameBytes         = ui32(nameBytes);
      entry->method            = ui16(ZipRead16(bytes + at + ZIP_CD_METHOD));
      entry->flags             = ui16(flags);
      at                       = recordEnd;
   }
   reader->entryCount = count;
   return ZIP_OK;
}

// Locates the directory, following the ZIP64 records when they are present, and hands it to the walker.
static cZIP_RESULT ZipParseDirectory(ZIP_READERptrc reader) {
   csi64 found = ZipFindEocd(reader->bytes, reader->byteCount);

   if(found < 0) return ZIP_ERROR_NOT_ZIP;

   cui8ptr bytes  = reader->bytes;
   cui64   at     = ui64(found);
   cui32   disk   = ZipRead16(bytes + at + ZIP_EOCD_DISK);
   cui32   cdDisk = ZipRead16(bytes + at + ZIP_EOCD_CD_DISK);
   ui64    total  = ui64(ZipRead16(bytes + at + ZIP_EOCD_TOTAL));
   ui64    cdSize = ui64(ZipRead32(bytes + at + ZIP_EOCD_CD_SIZE));
   ui64    cdAt   = ui64(ZipRead32(bytes + at + ZIP_EOCD_CD_OFFSET));

   if((disk && disk != ZIP_MARKER16) || (cdDisk && cdDisk != ZIP_MARKER16)) return ZIP_ERROR_MALFORMED;

   // The ZIP64 locator sits immediately before the 32-bit record and points at the 64-bit one, which
   // carries the counts and offsets that would not fit.
   if(at >= ZIP_LOCATOR_FIXED && ZipRead32(bytes + at - ZIP_LOCATOR_FIXED) == ZIP_SIG_LOCATOR64) {
      cui64 record = ZipRead64(bytes + at - ZIP_LOCATOR_FIXED + ZIP_LOCATOR_RECORD);

      if(record > reader->byteCount || reader->byteCount - record < ZIP_EOCD64_FIXED) return ZIP_ERROR_TRUNCATED;
      if(ZipRead32(bytes + record) != ZIP_SIG_EOCD64) return ZIP_ERROR_MALFORMED;

      total  = ZipRead64(bytes + record + ZIP_EOCD64_TOTAL);
      cdSize = ZipRead64(bytes + record + ZIP_EOCD64_CD_SIZE);
      cdAt   = ZipRead64(bytes + record + ZIP_EOCD64_CD_OFFSET);
   } else if(total == ui64(ZIP_MARKER16) || cdSize == ZIP_MARKER32 || cdAt == ZIP_MARKER32) {
      return ZIP_ERROR_MALFORMED; // ZIP64 markers with no ZIP64 record to resolve them
   }
   return ZipParseCentral(reader, cdAt, cdSize, total);
}

//== Entry points

cZIP_RESULT ZipOpen(ZIP_READERptrc reader, cwchptr path, cZIP_LIMITSptrc limits) {
   reader->bytes         = nullptr;
   reader->byteCount     = 0;
   reader->entries       = nullptr;
   reader->nameHeap      = nullptr;
   reader->inflatedBytes = 0;
   reader->limits        = *limits;
   reader->lastInflate   = INFLATE_OK;
   reader->entryCount    = 0;

   cZIP_RESULT read = ZipReadWholeFile(path, &reader->bytes, &reader->byteCount, limits->maxArchiveBytes);

   if(read != ZIP_OK) return read;

   if(ZipIsOleCompoundFile(reader)) return ZIP_ERROR_OLE;
   return ZipParseDirectory(reader);
}

void ZipClose(ZIP_READERptrc reader) {
   mdealloc(reader->bytes);
   mdealloc(reader->entries);
   mdealloc(reader->nameHeap);
   reader->bytes         = nullptr;
   reader->byteCount     = 0;
   reader->entries       = nullptr;
   reader->nameHeap      = nullptr;
   reader->inflatedBytes = 0;
   reader->entryCount    = 0;
}

csi32 ZipFindEntry(cZIP_READERptr reader, cchptr name) {
   if(!reader || !name) return -1;

   for(ui32 i = 0; i < reader->entryCount; ++i)
      if(ZipNameEqual(reader->entries[i].name, name)) return si32(i);
   return -1;
}

cZIP_RESULT ZipReadEntry(ZIP_READERptrc reader, cui32 index, ui8ptrptrc bytes, ui64ptrc byteCount) {
   *bytes     = nullptr;
   *byteCount = 0;
   if(index >= reader->entryCount) return ZIP_ERROR_RANGE;

   cZIP_ENTRYptrc entry    = &reader->entries[index];
   cui64          outBytes = entry->uncompressedBytes;

   if(entry->method != ZIP_METHOD_STORED && entry->method != ZIP_METHOD_DEFLATE) return ZIP_ERROR_METHOD;
   if(outBytes > reader->limits.maxEntryBytes) return ZIP_ERROR_LIMIT;
   if(reader->inflatedBytes + outBytes > reader->limits.maxTotalBytes) return ZIP_ERROR_LIMIT;
   if(outBytes && !entry->compressedBytes) return ZIP_ERROR_MALFORMED;

   // The ratio cap only applies once an entry is big enough for the ratio to mean anything: a few hundred
   // bytes of XML routinely compresses better than 1000:1 without being an attack. It is measured against
   // the declared compressed size, which an attacker can pad -- but only with bytes that must really be in
   // the file, since the data range is bounds-checked below. So the ratio cap buys an early refusal, and
   // maxEntryBytes and maxTotalBytes are what actually bound the work this function will do.
   if(outBytes > reader->limits.ratioFloorBytes && outBytes / entry->compressedBytes > ui64(reader->limits.maxRatio)) return ZIP_ERROR_LIMIT;

   cui64 header = entry->localHeaderOffset;

   if(header > reader->byteCount || reader->byteCount - header < ZIP_LOCAL_FIXED) return ZIP_ERROR_TRUNCATED;
   if(ZipRead32(reader->bytes + header) != ZIP_SIG_LOCAL) return ZIP_ERROR_MALFORMED;

   // The local header's own name and extra lengths decide where the data starts: writers are allowed to
   // put different extra fields here than in the central directory, and several do.
   cui64 nameBytes  = ui64(ZipRead16(reader->bytes + header + ZIP_LOCAL_NAME_LEN));
   cui64 extraBytes = ui64(ZipRead16(reader->bytes + header + ZIP_LOCAL_EXTRA_LEN));
   cui64 dataAt     = header + ZIP_LOCAL_FIXED + nameBytes + extraBytes;

   if(dataAt > reader->byteCount || reader->byteCount - dataAt < entry->compressedBytes) return ZIP_ERROR_TRUNCATED;

   // Charge the run total now the entry is accepted rather than when it succeeds: an entry that inflates
   // most of a cap's worth and only then fails its CRC-32 has still spent that work, and charging on
   // success alone would let a caller loop over failing entries without ever reaching maxTotalBytes.
   reader->inflatedBytes += outBytes;

   // Allocate at least one byte so an empty part still yields a pointer the caller can free unconditionally.
   ui8ptr buffer = (ui8ptr)amalloc(outBytes ? outBytes : 1u, 64u);

   if(!buffer) return ZIP_ERROR_MEMORY;

   if(entry->method == ZIP_METHOD_STORED) {
      if(entry->compressedBytes != outBytes) {
         mdealloc(buffer);
         return ZIP_ERROR_SIZE;
      }
      Copy(reader->bytes + dataAt, buffer, outBytes);
   } else {
      ui64            produced = 0;
      cINFLATE_RESULT result   = InflateRaw(reader->bytes + dataAt, entry->compressedBytes, buffer, outBytes, &produced);

      // The buffer is exactly the declared size, so a stream that produces more is stopped at the byte
      // that overruns rather than after the fact, and one that produces less is caught right here.
      if(result != INFLATE_OK) {
         reader->lastInflate = result;
         mdealloc(buffer);
         return ZIP_ERROR_INFLATE;
      }
      if(produced != outBytes) {
         mdealloc(buffer);
         return ZIP_ERROR_SIZE;
      }
   }
   if(Crc32(buffer, outBytes) != entry->crc32) {
      mdealloc(buffer);
      return ZIP_ERROR_CRC;
   }
   *bytes     = buffer;
   *byteCount = outBytes;
   return ZIP_OK;
}

cchptr ZipResultText(cZIP_READERptr reader, cZIP_RESULT result) {
   switch(result) {
   case ZIP_OK: return "the container is intact";
   case ZIP_ERROR_OPEN: return "cannot open input file";
   case ZIP_ERROR_MEMORY: return "out of memory while reading the container";
   case ZIP_ERROR_NOT_ZIP: return "not a valid DOCX; no ZIP end-of-central-directory record was found";
   case ZIP_ERROR_OLE: return "not a valid DOCX; this is an OLE compound file, so an encrypted .docx or a legacy .doc";
   case ZIP_ERROR_ENCRYPTED: return "not a valid DOCX; the archive holds encrypted entries";
   case ZIP_ERROR_METHOD: return "not a valid DOCX; an entry uses a compression method other than stored or deflate";
   case ZIP_ERROR_MALFORMED: return "not a valid DOCX; the ZIP structure is malformed";
   case ZIP_ERROR_TRUNCATED: return "not a valid DOCX; the archive is truncated";
   case ZIP_ERROR_SIZE: return "not a valid DOCX; an entry's size does not match the size its directory entry declares";
   case ZIP_ERROR_CRC: return "not a valid DOCX; an entry does not match the CRC-32 its directory entry declares";
   case ZIP_ERROR_LIMIT: return "refusing this file; it exceeds a decompression limit, so it may be a ZIP bomb";
   case ZIP_ERROR_TOO_LARGE: return "refusing this file; it is larger than this reader will load into memory";
   case ZIP_ERROR_RANGE: return "internal error; an entry outside the archive was asked for";
   default: break;
   }
   if(!reader || ui32(reader->lastInflate) >= ui32(INFLATE_RESULT_COUNT)) return "not a valid DOCX; a deflate stream is corrupt";
   return ZIP_INFLATE_TEXT[reader->lastInflate];
}
