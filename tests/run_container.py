# RULE-DEV:r17 GCS r17's file prolog is a C block comment, which Python cannot carry. The module
# docstring below holds the same information in the form the language allows.
"""Runs every container fixture through DOCXtoMD and checks the documented exit code and message.

This is M3's definition of done made runnable. It builds the fixtures first, so one command covers the
whole check:

    python tests/run_container.py                                   x64\\Release\\DOCXtoMD.exe
    python tests/run_container.py --exe x64\\Debug\\DOCXtoMD.exe     any other build

M5 adds run_golden.py, which byte-compares converted Markdown; this file stays with the container layer,
where the assertion is an exit code and a sentence rather than a document.
"""

import os
import subprocess
import sys
import zipfile

import make_fixtures

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(ROOT)
DEFAULT_EXE = os.path.join(REPO, "x64", "Release", "DOCXtoMD.exe")

# Command lines that are not about a fixture file, kept here so a rewrite of main.cpp cannot quietly
# change the surface M2 published.
CLI_CASES = [
    ([], 1, ["no input file given", "Usage: DOCXtoMD"], "no arguments prints the usage text"),
    (["--version"], 0, ["DOCXtoMD "], "--version reports and stops"),
    (["--help"], 0, ["Usage: DOCXtoMD"], "--help reports and stops"),
    (["--nonsense"], 1, ["unrecognised option"], "an unknown option is a usage error"),
]


def run(exe, args):
    try:
        done = subprocess.run([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300)
    except FileNotFoundError:
        raise SystemExit("cannot run %s -- pass --exe, and remember MSVC output only exists on Windows" % exe)
    except subprocess.TimeoutExpired:
        return None, "", ""
    return done.returncode, done.stdout.decode("utf-8", "replace"), done.stderr.decode("utf-8", "replace")


def check(exe, args, expected, matches, why, failures):
    code, out, err = run(exe, args)
    text = out + err
    if code is None:
        failures.append((args, "timed out", why))
        print("FAIL  %-28s timed out" % (args[-1] if args else "<no arguments>"))
        return
    if code != expected:
        failures.append((args, "exit %s, expected %d" % (code, expected), why))
        print("FAIL  %-28s exit %s, expected %d" % (args[-1] if args else "<no arguments>", code, expected))
        print("      %s" % text.strip().replace("\n", "\n      "))
        return
    missing = [needle for needle in matches if needle not in text]
    if missing:
        failures.append((args, "message missing %r" % missing, why))
        print("FAIL  %-28s exit %d, but the message does not mention %s" %
              (args[-1] if args else "<no arguments>", code, ", ".join(repr(m) for m in missing)))
        print("      %s" % text.strip().replace("\n", "\n      "))
        return
    print("ok    %-28s exit %d  %s" % (args[-1] if args else "<no arguments>", code, why))


# Fixtures whose word/document.xml is the part tree's file unaltered, so an independent reader must
# produce exactly those bytes. The rest deliberately carry a grown or repeated body, and duplicate-names
# is excluded on purpose: zipfile takes the last record of a repeated name and ZipReader documents taking
# the first, which is a divergence rather than a defect.
SAME_BODY = ["minimal-stored.docx", "minimal-deflated.docx", "fixed-huffman.docx", "zip64.docx",
             "data-descriptor.docx", "with-comment.docx"]


def cross_check(expectations, failures):
    """Reads every sound fixture back with Python's zipfile.

    make_fixtures.py writes ZIP records itself and DOCXtoMD reads them itself, so the two agreeing proves
    less than it looks: one shared misreading of the format would satisfy both. An independent
    implementation reading the same archives is what closes that. `testzip` decompresses every entry and
    checks it against the CRC-32 in its header, so it exercises the whole archive, not just the body.
    """
    want = open(os.path.join(ROOT, "fixtures", "minimal", "src", "word", "document.xml"), "rb").read()
    checked = 0

    for row in expectations:
        if row["code"] != 5:
            continue
        name = row["name"]
        checked += 1
        try:
            with zipfile.ZipFile(os.path.join(make_fixtures.BUILD, name)) as archive:
                broken = archive.testzip()
                body = archive.read("word/document.xml") if name in SAME_BODY else want
        except Exception as trouble:                                        # noqa: BLE001 - report anything
            failures.append((name, "python zipfile could not read it: %s" % trouble, "cross-check"))
            print("FAIL  %-28s python zipfile could not read it: %s" % (name, trouble))
            continue
        if broken is not None:
            failures.append((name, "python zipfile reports a bad entry: %s" % broken, "cross-check"))
            print("FAIL  %-28s python zipfile reports a bad entry: %s" % (name, broken))
        elif body != want:
            failures.append((name, "python zipfile reads different bytes", "cross-check"))
            print("FAIL  %-28s python zipfile reads %d bytes, not the part tree's %d" % (name, len(body), len(want)))
        elif name in SAME_BODY:
            print("ok    %-28s every entry decompresses and its CRC-32 matches; the body is the part tree's" % name)
        else:
            print("ok    %-28s every entry decompresses and its CRC-32 matches" % name)
    return checked


def main(argv):
    exe = DEFAULT_EXE
    if "--exe" in argv:
        exe = argv[argv.index("--exe") + 1]

    expectations = make_fixtures.build_all(verbose=False)
    failures = []

    print("command line")
    for args, code, matches, why in CLI_CASES:
        check(exe, args, code, matches, why, failures)

    print()
    print("an input that is not there")
    absent = os.path.join(make_fixtures.BUILD, "no-such-file.docx")
    if os.path.exists(absent):
        os.remove(absent)
    check(exe, [absent], 2, ["cannot open input file"], "an unopenable input is exit 2, not exit 3", failures)

    print()
    print("the fixtures, read by an independent implementation")
    crossed = cross_check(expectations, failures)

    print()
    print("containers")
    for row in expectations:
        path = os.path.join(make_fixtures.BUILD, row["name"])
        check(exe, [path], row["code"], row["matches"], row["why"], failures)

    total = len(expectations) + len(CLI_CASES) + crossed + 1

    print()
    if failures:
        print("%d of %d checks failed" % (len(failures), total))
        return 1
    print("all %d checks passed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
