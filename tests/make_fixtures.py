# RULE-DEV:r17 GCS r17's file prolog is a C block comment, which Python cannot carry. The module
# docstring below holds the same information in the form the language allows.
"""Builds the .docx fixtures the DOCXtoMD container tests run against.

Reviewable part trees live under tests/fixtures/<case>/src/ and are zipped here; the hostile cases are
synthesised, because a malformed archive cannot be expressed as a tree of files. Everything lands in
tests/build/, which is generated and git-ignored.

Run it with the interpreter rather than directly -- the file is CRLF like the rest of the repository, so
a shebang would not survive on a POSIX host:

    python tests/make_fixtures.py            build every fixture
    python tests/make_fixtures.py --list     name every fixture and the exit code it should produce

Since M4 every fixture that is expected to be read as a package must also hold well-formed XML in the
namespaces the converter knows: the main document part is tokenised end to end, not merely inflated.

The ZIP writer here is deliberately first-party rather than zipfile: the negatives need control over
individual header fields, and zipfile offers none. Being first-party on both sides is not a problem --
the fixtures are validated against Python's own zlib, which is what actually pins the DEFLATE behaviour.
"""

import os
import struct
import sys
import zlib

LOCAL_SIG = 0x04034B50
CENTRAL_SIG = 0x02014B50
EOCD_SIG = 0x06054B50
EOCD64_SIG = 0x06064B50
LOCATOR64_SIG = 0x07064B50
DESCRIPTOR_SIG = 0x08074B50

# A fixed 1980-01-01 timestamp, so a rebuild produces byte-identical fixtures.
DOS_TIME = 0
DOS_DATE = 0x0021

FLAG_ENCRYPTED = 0x0001
FLAG_DESCRIPTOR = 0x0008
FLAG_UTF8 = 0x0800

MARKER32 = 0xFFFFFFFF
MARKER16 = 0xFFFF

ROOT = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(ROOT, "fixtures")
BUILD = os.path.join(ROOT, "build")

# What each built fixture is for, and the exit code DOCXtoMD must return for it. run_container.py
# reads this table, so the expectations live in exactly one place. The sound flag is a second,
# independent question: whether the bytes are a well-formed ZIP that Python's zipfile must read back.
# Since M4 a package can be a perfectly good archive and still not be a DOCX, so the two do not
# coincide -- and since M5 a sound one exits 0, having actually written its Markdown, rather than
# exiting 5 to say the converter did not exist yet.
EXPECTATIONS = []

# Which built fixture must reproduce which fixture case's expected.md, byte for byte. run_golden.py
# reads this table for the same reason run_container.py reads the one above: a fixture and what it
# must produce are declared together, in one place, or they drift.
GOLDENS = []


def expect(name, code, matches, why, sound=None):
    EXPECTATIONS.append({"name": name, "code": code, "matches": matches, "why": why,
                         "sound": (code == 0 if sound is None else sound)})


def golden(name, case):
    GOLDENS.append({"name": name, "case": case,
                    "expected": os.path.join(FIXTURES, case, "expected.md")})


# ---------------------------------------------------------------------------- deflate helpers


def deflate(raw, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    """Raw DEFLATE with no zlib wrapper, which is what a ZIP entry stores."""
    engine = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    return engine.compress(raw) + engine.flush()


def deflate_blocks(chunks, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    """One raw DEFLATE stream carrying several blocks: every chunk but the last ends at a full flush.

    zlib picks its own block boundaries otherwise, and for a payload of the size a .docx part actually is
    it picks exactly one -- so this is the only way to build a fixture that really is multi-block.
    """
    engine = zlib.compressobj(level, zlib.DEFLATED, -15, 8, strategy)
    out = []
    for chunk in chunks[:-1]:
        out.append(engine.compress(chunk))
        out.append(engine.flush(zlib.Z_FULL_FLUSH))
    out.append(engine.compress(chunks[-1]))
    out.append(engine.flush())
    return b"".join(out)


def deflate_zeros(total, level=9):
    """Deflates `total` zero bytes without ever holding them all in memory."""
    engine = zlib.compressobj(level, zlib.DEFLATED, -15)
    block = b"\0" * (1 << 20)
    out = []
    left = total
    while left:
        take = min(left, len(block))
        out.append(engine.compress(block[:take]))
        left -= take
    out.append(engine.flush())
    return b"".join(out)


def crc_zeros(total):
    """CRC-32 of `total` zero bytes, computed the same way."""
    block = b"\0" * (1 << 20)
    crc = 0
    left = total
    while left:
        take = min(left, len(block))
        crc = zlib.crc32(block[:take], crc)
        left -= take
    return crc & MARKER32


def eocd_fields(data):
    """Locates the end-of-central-directory record and reads the three fields the negatives patch."""
    at = data.rfind(b"PK\x05\x06")
    total, cd_size, cd_at = struct.unpack("<HII", data[at + 10:at + 20])
    return at, total, cd_size, cd_at


def first_block_type(payload):
    """BTYPE of a raw DEFLATE stream's first block: 0 stored, 1 fixed, 2 dynamic."""
    if not payload:
        return None
    return (payload[0] >> 1) & 3


# ---------------------------------------------------------------------------- ZIP writer


def make_entry(name, raw, method="deflate", level=6, strategy=zlib.Z_DEFAULT_STRATEGY, **over):
    """One entry, with every header field open to being overridden by a hostile fixture."""
    if method == "store":
        payload, code = raw, 0
    elif method == "deflate":
        payload, code = deflate(raw, level, strategy), 8
    else:
        payload, code = raw, int(method)  # An unsupported method, storing the bytes as they are

    entry = {
        "name": name,
        "payload": payload,
        "method": code,
        "crc": zlib.crc32(raw) & MARKER32,
        "usize": len(raw),
        "csize": len(payload),
        "flags": 0,
        "descriptor": False,
    }
    entry.update(over)
    # A fixture that supplies its own payload gets a matching compressed size unless it deliberately
    # overrode that too, so an override cannot silently truncate the stream the reader is handed.
    if "csize" not in over:
        entry["csize"] = len(entry["payload"])
    return entry


def build_zip(entries, zip64=False, comment=b""):
    """Assembles local headers, data, the central directory and the end record into one archive."""
    out = bytearray()
    central = bytearray()

    for entry in entries:
        offset = len(out)
        name = entry["name"].encode("utf-8")
        flags = entry["flags"]
        if entry["descriptor"]:
            flags |= FLAG_DESCRIPTOR
        if any(byte > 0x7F for byte in name):
            flags |= FLAG_UTF8

        # A streamed entry leaves the local header's copies at zero and repeats them after the data; the
        # central directory is authoritative either way, which is what DOCXtoMD relies on.
        if entry["descriptor"]:
            local_crc, local_csize, local_usize = 0, 0, 0
        else:
            local_crc, local_csize, local_usize = entry["crc"], entry["csize"], entry["usize"]

        out += struct.pack("<IHHHHHIIIHH", LOCAL_SIG, 20, flags, entry["method"], DOS_TIME, DOS_DATE,
                           local_crc, local_csize & MARKER32, local_usize & MARKER32, len(name), 0)
        out += name
        out += entry["payload"]
        if entry["descriptor"]:
            out += struct.pack("<IIII", DESCRIPTOR_SIG, entry["crc"], entry["csize"], entry["usize"])

        extra = b""
        usize, csize, header_at = entry["usize"], entry["csize"], offset
        if zip64:
            extra = struct.pack("<HHQQQ", 0x0001, 24, usize, csize, header_at)
            usize = csize = header_at = MARKER32

        central += struct.pack("<IHHHHHHIIIHHHHHII", CENTRAL_SIG, 20, 20, flags, entry["method"], DOS_TIME,
                               DOS_DATE, entry["crc"], csize & MARKER32, usize & MARKER32, len(name),
                               len(extra), 0, 0, 0, 0, header_at & MARKER32)
        central += name
        central += extra

    cd_at = len(out)
    out += central
    cd_size = len(central)
    count = len(entries)

    if zip64:
        record_at = len(out)
        out += struct.pack("<IQHHIIQQQQ", EOCD64_SIG, 44, 45, 45, 0, 0, count, count, cd_size, cd_at)
        out += struct.pack("<IIQI", LOCATOR64_SIG, 0, record_at, 1)
        out += struct.pack("<IHHHHIIH", EOCD_SIG, MARKER16, MARKER16, MARKER16, MARKER16, MARKER32,
                           MARKER32, len(comment))
    else:
        out += struct.pack("<IHHHHIIH", EOCD_SIG, 0, 0, count, count, cd_size, cd_at, len(comment))
    out += comment
    return bytes(out)


# ---------------------------------------------------------------------------- part trees


def read_part_tree(case):
    """Reads tests/fixtures/<case>/src/ into (part name, bytes) pairs in package order."""
    root = os.path.join(FIXTURES, case, "src")
    if not os.path.isdir(root):
        raise SystemExit("fixture part tree not found: " + root)

    parts = []
    for folder, _, files in os.walk(root):
        for leaf in files:
            path = os.path.join(folder, leaf)
            name = os.path.relpath(path, root).replace(os.sep, "/")
            with open(path, "rb") as handle:
                parts.append((name, handle.read()))
    # Sorting puts [Content_Types].xml first and _rels/.rels second, which is the order Word writes and
    # the order a reader is least surprised by.
    parts.sort(key=lambda part: part[0])
    return parts


# --list only needs the expectation table, so it builds nothing: the two bomb fixtures cost hundreds of
# megabytes of compression each, and printing a list should not.
WRITING = True


def write(name, data):
    if not WRITING:
        return None
    path = os.path.join(BUILD, name)
    with open(path, "wb") as handle:
        handle.write(data)
    return path


# ---------------------------------------------------------------------------- fixtures


def build_all(verbose=True, writing=True):
    global WRITING
    WRITING = writing
    del EXPECTATIONS[:]
    del GOLDENS[:]
    if writing:
        os.makedirs(BUILD, exist_ok=True)
    parts = read_part_tree("minimal")
    body = dict(parts)["word/document.xml"]
    report = []

    def note(name, entries):
        payloads = [e["payload"] for e in entries if e["method"] == 8]
        types = sorted({first_block_type(p) for p in payloads if p})
        # A full flush ends a block and emits an empty stored one, so each marker means two more blocks.
        # That makes this a lower bound on the block count, which is all the report claims.
        blocks = 1 + 2 * max([p.count(b"\x00\x00\xff\xff") for p in payloads] or [0])
        report.append((name, len(entries), types, blocks))

    # -- sound containers. Each exits 5: the container is verified, but no build before M5 converts.

    stored = [make_entry(name, raw, method="store") for name, raw in parts]
    note("minimal-stored.docx", stored)
    write("minimal-stored.docx", build_zip(stored))
    expect("minimal-stored.docx", 0, ["wrote", "minimal-stored.md"],
           "every entry stored, so ZipReader copies rather than inflates")

    deflated = [make_entry(name, raw) for name, raw in parts]
    note("minimal-deflated.docx", deflated)
    write("minimal-deflated.docx", build_zip(deflated))
    expect("minimal-deflated.docx", 0, ["wrote", "minimal-deflated.md"],
           "every entry deflated, CRC-32 verified after inflation")

    fixed = [make_entry(name, raw, strategy=zlib.Z_FIXED) for name, raw in parts]
    note("fixed-huffman.docx", fixed)
    write("fixed-huffman.docx", build_zip(fixed))
    expect("fixed-huffman.docx", 0, ["wrote", "fixed-huffman.md"], "RFC 1951 fixed-Huffman blocks (BTYPE 1)")

    # A body big and varied enough that zlib chooses dynamic Huffman, with long matches to exercise the
    # window. Generated rather than committed, so the repository does not carry 100 KiB of filler.
    filler = []
    for index in range(900):
        filler.append("      <w:p><w:r><w:t>Paragraph %d of the dynamic Huffman fixture, "
                      "repeated text repeated text.</w:t></w:r></w:p>" % index)
    grown = body.replace(b"  </w:body>", ("\n".join(filler) + "\n  </w:body>").encode("utf-8"))

    # Level 0 emits stored blocks inside a deflate stream, which is a different code path from a stored
    # ZIP entry and the only way to reach InflateStoredBlock. A stored block caps at 65535 bytes, so the
    # grown body is what makes this more than one of them.
    raw_blocks = [make_entry(name, grown if name == "word/document.xml" else raw, level=0)
                  for name, raw in parts]
    note("deflate-stored-blocks.docx", raw_blocks)
    write("deflate-stored-blocks.docx", build_zip(raw_blocks))
    expect("deflate-stored-blocks.docx", 0, ["wrote", "deflate-stored-blocks.md"],
           "several RFC 1951 stored blocks inside one deflate stream (BTYPE 0)")

    dynamic = [make_entry(name, grown if name == "word/document.xml" else raw) for name, raw in parts]
    note("dynamic-huffman.docx", dynamic)
    write("dynamic-huffman.docx", build_zip(dynamic))
    expect("dynamic-huffman.docx", 0, ["wrote", "dynamic-huffman.md"],
           "one RFC 1951 dynamic-Huffman block (BTYPE 2) over a 100 KiB part")

    # Several blocks in one stream, and a mixture of types: a full flush ends the current block and emits
    # an empty stored one, so this reaches the block loop's hand-off between types as well as its repeat.
    pieces = [grown[i:i + 20000] for i in range(0, len(grown), 20000)]
    mixed = [make_entry(name, raw) for name, raw in parts if name != "word/document.xml"]
    mixed.append(make_entry("word/document.xml", grown, payload=deflate_blocks(pieces)))
    note("multi-block.docx", mixed)
    write("multi-block.docx", build_zip(mixed))
    expect("multi-block.docx", 0, ["wrote", "multi-block.md"],
           "one deflate stream carrying many blocks of mixed type, ended by a final block")

    # Overlapping match copies -- a match that reads bytes it is still producing -- are a named M3 scope
    # item, and only a run of repeated bytes reaches them. Distances 1, 2 and 3 are the interesting ones.
    runs = b"".join([b"      <w:p><w:r><w:t>",
                     b"A" * 4000, b"</w:t></w:r></w:p>\n      <w:p><w:r><w:t>",
                     b"ab" * 3000, b"</w:t></w:r></w:p>\n      <w:p><w:r><w:t>",
                     b"xyz" * 2000, b"</w:t></w:r></w:p>\n"])
    repeated = body.replace(b"  </w:body>", runs + b"  </w:body>")
    overlap = [make_entry(name, repeated if name == "word/document.xml" else raw) for name, raw in parts]
    note("overlapping-matches.docx", overlap)
    write("overlapping-matches.docx", build_zip(overlap))
    expect("overlapping-matches.docx", 0, ["wrote", "overlapping-matches.md"],
           "runs at distance 1, 2 and 3: matches that read the bytes they are still producing")

    write("zip64.docx", build_zip([make_entry(name, raw) for name, raw in parts], zip64=True))
    expect("zip64.docx", 0, ["wrote", "zip64.md"],
           "ZIP64 end record, locator and extra fields on an archive small enough to review")

    streamed = [make_entry(name, raw, descriptor=True) for name, raw in parts]
    write("data-descriptor.docx", build_zip(streamed))
    expect("data-descriptor.docx", 0, ["wrote", "data-descriptor.md"],
           "local headers zeroed and sizes in a trailing data descriptor")

    without_body = [make_entry(name, raw) for name, raw in parts if name != "word/document.xml"]
    write("no-document.docx", build_zip(without_body))
    expect("no-document.docx", 3, ["main document part the package names is not in the archive"],
           "_rels/.rels names word/document.xml and the archive does not contain it", sound=True)

    # Duplicate names: ZipReader documents first-in-central-directory-order as the tie-break, so the note
    # must report the first entry's size and never the decoy's.
    decoy = body.replace(b"Minimal fixture", b"Decoy that must never be read") + b"<!-- padding -->" * 40
    doubled = [make_entry(name, raw) for name, raw in parts]
    doubled.append(make_entry("word/document.xml", decoy))
    write("duplicate-names.docx", build_zip(doubled))
    expect("duplicate-names.docx", 0, ["wrote", "duplicate-names.md"],
           "two entries named word/document.xml: the first in directory order wins")

    write("with-comment.docx", build_zip([make_entry(name, raw) for name, raw in parts],
                                         comment=b"an archive comment, so the end record is not the last 22 bytes"))
    expect("with-comment.docx", 0, ["wrote", "with-comment.md"],
           "an archive comment, so the end record is not the last 22 bytes of the file")

    # -- packages the M4 model has to resolve, or refuse. Every one is a sound container: what is being
    # tested is the package on top of it, so a failure here is never the ZIP layer's.

    def swap(source, replacements):
        """One part list with byte substitutions applied to named parts."""
        out = []
        for name, raw in source:
            for target, before, after in replacements:
                if name == target:
                    raw = raw.replace(before, after)
            out.append((name, raw))
        return out

    # The definition-of-done fixture: no word/ folder anywhere, so a converter that reaches for
    # word/document.xml by name finds nothing and only relationship resolution can succeed.
    relocated = read_part_tree("relocated")
    write("relocated-main.docx", build_zip([make_entry(name, raw) for name, raw in relocated]))
    expect("relocated-main.docx", 0, ["wrote", "relocated-main.md"],
           "no word/ folder at all: rId7 rather than rId1 names the body, and its prefix is x, not w")

    # ISO 29500 Strict spells every namespace and relationship type differently. Both families have to
    # walk the same code, which is what matching on the URI rather than the prefix buys.
    strict = []
    for name, raw in parts:
        raw = raw.replace(b"http://schemas.openxmlformats.org/wordprocessingml/2006/main",
                          b"http://purl.oclc.org/ooxml/wordprocessingml/main")
        raw = raw.replace(b"http://schemas.openxmlformats.org/officeDocument/2006/relationships/",
                          b"http://purl.oclc.org/ooxml/officeDocument/relationships/")
        strict.append((name, raw))
    write("strict-namespaces.docx", build_zip([make_entry(name, raw) for name, raw in strict]))
    expect("strict-namespaces.docx", 0, ["wrote", "strict-namespaces.md"],
           "ISO 29500 Strict URIs throughout, which must resolve exactly as Transitional ones do")

    # A byte-order mark in front of a part, which Word writes on some of them.
    marked = swap(parts, [("word/document.xml", b"<?xml", b"\xEF\xBB\xBF<?xml")])
    write("bom-part.docx", build_zip([make_entry(name, raw) for name, raw in marked]))
    expect("bom-part.docx", 0, ["wrote", "bom-part.md"], "a UTF-8 byte-order mark ahead of the XML declaration")

    # A UTF-16 part, which ISO/IEC 29500 permits and Utf transcodes before anything tokenises it.
    wide = []
    for name, raw in parts:
        if name == "word/document.xml":
            raw = raw.decode("utf-8").encode("utf-16-le")
            raw = b"\xFF\xFE" + raw
        wide.append((name, raw))
    write("utf16-part.docx", build_zip([make_entry(name, raw) for name, raw in wide]))
    expect("utf16-part.docx", 0, ["wrote", "utf16-part.md"], "word/document.xml written as UTF-16LE with a mark")

    # The relationship names a part that is not there, but [Content_Types].xml still says which part is
    # the body. The relationship decides first; this is the one path where the content type takes over.
    recovered = swap(parts, [("_rels/.rels", b'Target="word/document.xml"', b'Target="word/missing.xml"')])
    write("main-by-content-type.docx", build_zip([make_entry(name, raw) for name, raw in recovered]))
    expect("main-by-content-type.docx", 0, ["wrote", "main-by-content-type.md"],
           "a dangling officeDocument relationship, recovered through the content-type override")
    # The relationship names a part that is there but is typed as something else. The relationship is the
    # specification's discovery mechanism and [Content_Types].xml is metadata, so the relationship wins.
    # That reading of "cross-check" is decision D9, ruled 2026-08-24, and this fixture is what stops it
    # changing silently: a session that starts refusing the file fails here rather than only contradicting
    # CLAUDE.md, which is the whole reason a ruled decision gets a fixture and not just a table row.
    mistyped = swap(parts, [("[Content_Types].xml",
                             b'PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-'
                             b'officedocument.wordprocessingml.document.main+xml"',
                             b'PartName="/word/document.xml" ContentType="application/xml"')])
    write("content-type-mismatch.docx", build_zip([make_entry(name, raw) for name, raw in mistyped]))
    expect("content-type-mismatch.docx", 0, ["wrote", "content-type-mismatch.md"],
           "the body is typed application/xml, and the officeDocument relationship still decides")
    # Two officeDocument relationships, the decoy first and typed application/xml, the real body second
    # and typed as a main document. Presence alone picks the decoy, so only a working content-type
    # resolution picks the body -- which is what makes that half of the module testable at all.
    decoyed = []
    for name, raw in parts:
        if name == "_rels/.rels":
            raw = raw.replace(b'<Relationship Id="rId1"',
                              b'<Relationship Id="rId9" Type="http://schemas.openxmlformats.org/'
                              b'officeDocument/2006/relationships/officeDocument" Target="word/decoy.xml"/>'
                              b'<Relationship Id="rId1"')
        decoyed.append((name, raw))
    decoyed.append(("word/decoy.xml", body.replace(b"Minimal fixture", b"Decoy typed application/xml")))
    decoyed.sort(key=lambda part: part[0])
    write("decoy-main-rel.docx", build_zip([make_entry(name, raw) for name, raw in decoyed]))
    expect("decoy-main-rel.docx", 0, ["wrote", "decoy-main-rel.md"],
           "an untyped decoy relationship first: only the content-type cross-check reaches the real body")

    # OPC compares part names case-insensitively. The entries stay lowercase while the Override and the
    # relationship Target are written in mixed case, so a case-sensitive comparison finds neither.
    mixed_case = swap(parts, [("[Content_Types].xml", b'PartName="/word/document.xml"',
                               b'PartName="/word/Document.xml"'),
                              ("_rels/.rels", b'Target="word/document.xml"', b'Target="Word/Document.xml"')])
    write("mixed-case-names.docx", build_zip([make_entry(name, raw) for name, raw in mixed_case]))
    expect("mixed-case-names.docx", 0, ["wrote", "mixed-case-names.md"],
           "part names written in a different case than the entries they name")



    # -- packages that are not usable DOCX files. Every one exits 3.

    broken_utf8 = swap(parts, [("word/document.xml", b"Minimal fixture", b"Minimal \xFF fixture")])
    write("bad-utf8.docx", build_zip([make_entry(name, raw) for name, raw in broken_utf8]))
    expect("bad-utf8.docx", 3, ["cannot begin a UTF-8 sequence"],
           "a part carrying a byte no UTF-8 sequence can start with", sound=True)

    truncated_utf8 = swap(parts, [("word/document.xml", b"Minimal fixture", b"Minimal \xE2\x82 fixture")])
    write("truncated-utf8.docx", build_zip([make_entry(name, raw) for name, raw in truncated_utf8]))
    expect("truncated-utf8.docx", 3, ["broken UTF-8 sequence"], "a three-byte sequence cut to two", sound=True)

    doctype = swap(parts, [("word/document.xml", b"?>\n", b"?>\n<!DOCTYPE w:document [<!ENTITY x \"y\">]>\n")])
    write("doctype.docx", build_zip([make_entry(name, raw) for name, raw in doctype]))
    expect("doctype.docx", 3, ["document type declaration"],
           "a DTD in the body, which is the entity-expansion attack and is refused where it stands", sound=True)

    unclosed = swap(parts, [("word/document.xml", b"</w:document>", b"")])
    write("malformed-xml.docx", build_zip([make_entry(name, raw) for name, raw in unclosed]))
    expect("malformed-xml.docx", 3, ["ends in the middle of an element"], "a body whose root never closes",
           sound=True)

    not_types = swap(parts, [("[Content_Types].xml", b"Types>", b"NotTypes>"),
                             ("[Content_Types].xml", b"<Types ", b"<NotTypes ")])
    write("bad-content-types.docx", build_zip([make_entry(name, raw) for name, raw in not_types]))
    expect("bad-content-types.docx", 3, ["does not declare content types"],
           "[Content_Types].xml whose root element is something else entirely", sound=True)

    no_office = swap(parts, [("_rels/.rels", b"relationships/officeDocument", b"relationships/styles")])
    write("no-office-rel.docx", build_zip([make_entry(name, raw) for name, raw in no_office]))
    expect("no-office-rel.docx", 3, ["names no main document part"],
           "_rels/.rels with no officeDocument relationship in it at all", sound=True)

    external_main = swap(parts, [("_rels/.rels", b'Target="word/document.xml"',
                                  b'Target="http://example.com/x" TargetMode="External"')])
    write("external-main-rel.docx", build_zip([make_entry(name, raw) for name, raw in external_main]))
    expect("external-main-rel.docx", 3, ["names no main document part"],
           "an officeDocument relationship pointing outside the package, which names no part of it", sound=True)

    traversal = swap(parts, [("_rels/.rels", b'Target="word/document.xml"', b'Target="../../../etc/passwd"')])
    write("traversal-target.docx", build_zip([make_entry(name, raw) for name, raw in traversal]))
    expect("traversal-target.docx", 3, ["not a part of the package"],
           "a relationship target climbing out of the package, refused rather than clamped", sound=True)

    encoded_traversal = swap(parts, [("_rels/.rels", b'Target="word/document.xml"',
                                      b'Target="%2e%2e/%2e%2e/etc/passwd"')])
    write("encoded-traversal.docx", build_zip([make_entry(name, raw) for name, raw in encoded_traversal]))
    expect("encoded-traversal.docx", 3, ["not a part of the package"],
           "the same climb spelled in percent escapes, which decoding after normalising is what catches", sound=True)

    absolute_target = swap(parts, [("_rels/.rels", b'Target="word/document.xml"',
                                    b'Target="C:\\Windows\\System32\\drivers\\etc\\hosts"')])
    write("drive-letter-target.docx", build_zip([make_entry(name, raw) for name, raw in absolute_target]))
    expect("drive-letter-target.docx", 3, ["not a part of the package"],
           "a drive-lettered target, which is a one-letter URI scheme and refused as one", sound=True)
    # The main part's own relationships part is malformed. It is read on the path that runs whether or
    # not anything is being printed, so -q cannot decide whether the defect is noticed.
    bad_doc_rels = swap(parts, [("word/_rels/document.xml.rels", b"</Relationships>", b"")])
    write("bad-document-rels.docx", build_zip([make_entry(name, raw) for name, raw in bad_doc_rels]))
    expect("bad-document-rels.docx", 3, ["ends in the middle of an element", "document.xml.rels"],
           "word/_rels/document.xml.rels never closes its root element", sound=True)

    # A relationships part that is well-formed XML but is not a Relationships document.
    not_rels = swap(parts, [("word/_rels/document.xml.rels", b"Relationships>", b"NotRelationships>"),
                            ("word/_rels/document.xml.rels", b"<Relationships ", b"<NotRelationships ")])
    write("bad-rels-root.docx", build_zip([make_entry(name, raw) for name, raw in not_rels]))
    expect("bad-rels-root.docx", 3, ["does not hold relationships", "document.xml.rels"],
           "a relationships part whose root element is something else entirely", sound=True)

    # A structural cap of the package model rather than of the container: every byte cap in ZipReader is
    # satisfied, and it is the number of rows that is refused.
    crowded = list(parts)
    for index, (name, raw) in enumerate(crowded):
        if name != "[Content_Types].xml":
            continue
        rows = b"".join(b'<Override PartName="/word/pad%05d.xml" ContentType="application/xml"/>' % row
                        for row in range(5000))
        crowded[index] = (name, raw.replace(b"</Types>", rows + b"</Types>"))
    write("too-many-overrides.docx", build_zip([make_entry(name, raw) for name, raw in crowded]))
    expect("too-many-overrides.docx", 3, ["more structure than the converter will read"],
           "five thousand Override rows, past the content-types cap and inside every byte cap", sound=True)


    # -- containers that are not usable DOCX files. Every one exits 3.

    write("not-a-zip.docx", b"This file is plain text, not a ZIP archive, and is padded past 22 bytes.\n")
    expect("not-a-zip.docx", 3, ["no ZIP end-of-central-directory"], "no EOCD signature anywhere")

    write("empty.docx", b"")
    expect("empty.docx", 3, ["no ZIP end-of-central-directory"], "zero bytes, shorter than an EOCD record")

    write("legacy-doc.docx", b"\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1" + b"\0" * 504)
    expect("legacy-doc.docx", 3, ["OLE compound file"],
           "OLE magic: an encrypted .docx or a legacy .doc, neither of which is a ZIP")

    scrambled = [make_entry(name, raw, flags=FLAG_ENCRYPTED if name == "word/document.xml" else 0)
                 for name, raw in parts]
    write("encrypted.docx", build_zip(scrambled))
    expect("encrypted.docx", 3, ["encrypted entries"], "general purpose bit 0 set on an entry")

    whole = build_zip([make_entry(name, raw) for name, raw in parts])
    write("truncated.docx", whole[:-30])
    expect("truncated.docx", 3, ["no ZIP end-of-central-directory"], "the end record chopped off")

    broken_crc = [make_entry(name, raw, method="store",
                             **({"crc": 0x01020304} if name == "word/document.xml" else {}))
                  for name, raw in parts]
    write("bad-crc.docx", build_zip(broken_crc))
    expect("bad-crc.docx", 3, ["CRC-32"], "a stored entry whose declared CRC-32 does not match its bytes")

    def corrupt(payload):
        # Byte 3 lands inside a dynamic block's code-length table, so the stream fails structurally
        # rather than only failing its checksum.
        damaged = bytearray(payload)
        damaged[3] ^= 0xFF
        return bytes(damaged)

    broken_stream = []
    for name, raw in parts:
        entry = make_entry(name, raw)
        if name == "word/document.xml":
            entry["payload"] = corrupt(entry["payload"])
        broken_stream.append(entry)
    write("corrupt-deflate.docx", build_zip(broken_stream))
    expect("corrupt-deflate.docx", 3, ["deflate stream"], "a flipped byte inside a deflate stream")

    lying = []
    for name, raw in parts:
        entry = make_entry(name, raw)
        if name == "word/document.xml":
            entry["usize"] = len(raw) // 2  # The stream really produces twice this
        lying.append(entry)
    write("lying-size.docx", build_zip(lying))
    expect("lying-size.docx", 3, ["produces more data than its directory entry declares"],
           "a directory entry understating its size: the inflater must stop at the cap, not trust it")

    def patch(data, at, raw):
        out = bytearray(data)
        out[at:at + len(raw)] = raw
        return bytes(out)

    _, _, _, cd_at = eocd_fields(whole)
    write("bad-directory.docx", patch(whole, cd_at, b"XX"))
    expect("bad-directory.docx", 3, ["ZIP structure is malformed"],
           "the first central directory record's signature corrupted")

    eocd_at, _, _, _ = eocd_fields(whole)
    write("overlong-directory.docx", patch(whole, eocd_at + 12, struct.pack("<I", 0x00100000)))
    expect("overlong-directory.docx", 3, ["archive is truncated"],
           "an end record claiming a central directory that runs past the end of the file")

    write("no-content-types.docx",
          build_zip([make_entry(name, raw) for name, raw in parts if name != "[Content_Types].xml"]))
    expect("no-content-types.docx", 3, ["[Content_Types].xml"], "the first entry point OPC guarantees is absent",
           sound=True)

    write("no-rels.docx",
          build_zip([make_entry(name, raw) for name, raw in parts if name != "_rels/.rels"]))
    expect("no-rels.docx", 3, ["_rels/.rels"], "the second entry point OPC guarantees is absent", sound=True)

    odd_method = []
    for name, raw in parts:
        odd_method.append(make_entry(name, raw, method=12) if name == "[Content_Types].xml"
                          else make_entry(name, raw))
    write("unsupported-method.docx", build_zip(odd_method))
    expect("unsupported-method.docx", 3, ["compression method"], "method 12 (bzip2), which DOCXtoMD does not read")

    # A truthful bomb: the entry really does inflate to more than the per-entry cap, so the cap is what
    # stops it rather than a header being disbelieved.
    bomb_bytes = 300 << 20
    bomb_payload = deflate_zeros(bomb_bytes) if WRITING else b""
    bomb = [make_entry(name, raw) for name, raw in parts if name != "word/document.xml"]
    bomb.append({"name": "word/document.xml", "payload": bomb_payload, "method": 8,
                 "crc": crc_zeros(bomb_bytes), "usize": bomb_bytes, "csize": len(bomb_payload),
                 "flags": 0, "descriptor": False})
    write("bomb.docx", build_zip(bomb))
    expect("bomb.docx", 3, ["decompression limit"], "an entry inflating to 300 MiB, past the 256 MiB per-entry cap")

    ratio_bytes = 16 << 20
    ratio_payload = deflate_zeros(ratio_bytes) if WRITING else b""
    if WRITING and ratio_bytes // len(ratio_payload) <= 1000:
        raise SystemExit("ratio fixture no longer exceeds 1000:1; enlarge it")
    ratio = [make_entry(name, raw) for name, raw in parts if name != "word/document.xml"]
    ratio.append({"name": "word/document.xml", "payload": ratio_payload, "method": 8,
                  "crc": crc_zeros(ratio_bytes), "usize": ratio_bytes, "csize": len(ratio_payload),
                  "flags": 0, "descriptor": False})
    write("ratio-bomb.docx", build_zip(ratio))
    expect("ratio-bomb.docx", 3, ["decompression limit"],
           "16 MiB from 16 KiB: under every byte cap, over the 1000:1 ratio cap")

    crowd = [make_entry(name, raw) for name, raw in parts]
    crowd += [make_entry("word/media/pad%05d.bin" % index, b"x", method="store")
              for index in range(10001 if WRITING else 1)]
    write("too-many-entries.docx", build_zip(crowd))
    expect("too-many-entries.docx", 3, ["decompression limit"], "more central directory records than the cap allows")

    # -- golden fixtures. Each of these is an ordinary sound container; what makes it a golden is the
    # expected.md beside its part tree, which run_golden.py byte-compares the converted output against.
    # The two minimal cases are registered rather than rebuilt, because a fixture that is already proving
    # something about the container layer proves the conversion of the same bytes for free.

    # Fourteen containers carry the minimal document unaltered under fourteen different container and
    # package shapes -- stored and deflated entries, fixed-Huffman blocks, ZIP64, a data descriptor, an
    # archive comment, ISO 29500 Strict URIs, a byte-order mark, a UTF-16 part, a content type that
    # disagrees with the relationship, a decoy relationship, mixed-case part names and a duplicated
    # entry name. Every one of them has to produce the same bytes, which is a stronger statement than
    # any exit code, and it is where run_container.py's substring assertions moved to at M5.
    for same in ["minimal-stored.docx", "minimal-deflated.docx", "fixed-huffman.docx", "zip64.docx",
                 "data-descriptor.docx", "with-comment.docx", "strict-namespaces.docx", "bom-part.docx",
                 "utf16-part.docx", "main-by-content-type.docx", "content-type-mismatch.docx",
                 "decoy-main-rel.docx", "mixed-case-names.docx", "duplicate-names.docx"]:
        golden(same, "minimal")
    golden("relocated-main.docx", "relocated")
    for case in ["headings", "toggles", "textflow", "nostyles", "wrappers"]:
        tree = read_part_tree(case)
        write(case + ".docx", build_zip([make_entry(name, raw) for name, raw in tree]))
        expect(case + ".docx", 0, ["wrote", case + ".md"], "the %s golden fixture" % case)
        golden(case + ".docx", case)


    if verbose:
        print("built %d fixtures in %s" % (len(EXPECTATIONS), BUILD))
        print()
        print("%-28s %7s %7s  %s" % ("fixture", "entries", "blocks", "first deflate block type per entry"))
        names = {0: "stored", 1: "fixed", 2: "dynamic"}
        for name, count, types, blocks in report:
            spelt = ", ".join(names.get(kind, "reserved(%s)" % kind) for kind in types) or "-"
            print("%-28s %7d %6s+  %s" % (name, count, blocks, spelt))
    return EXPECTATIONS


def main(argv):
    if "--list" in argv:
        build_all(verbose=False, writing=False)
        for row in EXPECTATIONS:
            print("%-28s exit %d  %s" % (row["name"], row["code"], row["why"]))
        for row in GOLDENS:
            print("%-28s golden  compares against %s" % (row["name"], row["case"] + "/expected.md"))
        return 0
    build_all()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
