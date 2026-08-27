# RULE-DEV:r17 GCS r17's file prolog is a C block comment, which Python cannot carry. The module
# docstring below holds the same information in the form the language allows.
"""Converts every golden fixture and byte-compares the result against its expected.md.

This is M5's definition of done made runnable. It builds the fixtures first, so one command covers the
whole check:

    python tests/run_golden.py                                   x64\\Release\\DOCXtoMD.exe
    python tests/run_golden.py --exe x64\\Debug\\DOCXtoMD.exe     any other build

Every case is converted twice, once to a file next to the input and once through --stdout, and the two
must agree with each other as well as with expected.md: they are different code paths in Convert.cpp,
and only comparing both proves the document does not depend on which one was taken.

tests/run_container.py stays with the container and package layers, where the assertion is an exit code
and a sentence rather than a document; this file is where the document itself is the assertion.
"""

import os
import subprocess
import sys

import make_fixtures

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(ROOT)
DEFAULT_EXE = os.path.join(REPO, "x64", "Release", "DOCXtoMD.exe")


def run(exe, args, cwd=None):
    try:
        done = subprocess.run([exe] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=300, cwd=cwd)
    except FileNotFoundError:
        raise SystemExit("cannot run %s -- pass --exe, and remember MSVC output only exists on Windows" % exe)
    except subprocess.TimeoutExpired:
        return None, b"", ""
    return done.returncode, done.stdout, done.stderr.decode("utf-8", "replace")


def show(label, produced, wanted):
    """Prints the first line that differs, which is what a byte comparison is actually useful for."""
    if produced.replace(b"\r\n", b"\n") == wanted.replace(b"\r\n", b"\n"):
        print("      %s: the two differ only in line endings -- the output contract is LF, and\n"
              "      expected.md must stay LF too (.gitattributes marks tests/fixtures/ byte-for-byte)" % label)
        return
    got = produced.split(b"\n")
    want = wanted.split(b"\n")
    for index in range(max(len(got), len(want))):
        mine = got[index] if index < len(got) else b"<end of output>"
        theirs = want[index] if index < len(want) else b"<end of file>"
        if mine != theirs:
            print("      %s line %d" % (label, index + 1))
            print("      expected %r" % theirs)
            print("      produced %r" % mine)
            return
    print("      %s: the bytes differ only in length (%d produced, %d expected)" % (label, len(produced), len(wanted)))


def check_case(exe, row, failures):
    """Converts one fixture to a file and to stdout, and compares both against expected.md."""
    name = row["name"]
    source = os.path.join(make_fixtures.BUILD, name)
    written = os.path.join(make_fixtures.BUILD, name[:-5] + ".md")
    checks = 0

    if not os.path.exists(row["expected"]):
        failures.append((name, "no expected.md", row["case"]))
        print("FAIL  %-28s tests/fixtures/%s/expected.md does not exist" % (name, row["case"]))
        return checks
    with open(row["expected"], "rb") as handle:
        wanted = handle.read()
    if os.path.exists(written):
        os.remove(written)
    clear_dir(media_dir_of(name))

    code, out, err = run(exe, [source])
    checks += 1
    if code != 0:
        failures.append((name, "exit %s, expected 0" % code, "written"))
        print("FAIL  %-28s exit %s, expected 0" % (name, code))
        print("      %s" % err.strip().replace("\n", "\n      "))
        return checks
    if not os.path.exists(written):
        failures.append((name, "no output file", "written"))
        print("FAIL  %-28s wrote no %s" % (name, os.path.basename(written)))
        return checks
    with open(written, "rb") as handle:
        produced = handle.read()
    if produced != wanted:
        failures.append((name, "written bytes differ", "written"))
        print("FAIL  %-28s the written file does not match expected.md" % name)
        show("written", produced, wanted)
    else:
        print("ok    %-28s %d bytes written, byte-identical to %s/expected.md" % (name, len(produced), row["case"]))

    code, out, err = run(exe, ["--stdout", source])
    checks += 1
    if code != 0:
        failures.append((name, "--stdout exit %s, expected 0" % code, "stdout"))
        print("FAIL  %-28s --stdout exit %s, expected 0" % (name, code))
        return checks
    if out != wanted:
        failures.append((name, "--stdout bytes differ", "stdout"))
        print("FAIL  %-28s --stdout does not match expected.md" % name)
        show("stdout", out, wanted)
    else:
        print("ok    %-28s --stdout produces the same %d bytes" % (name, len(out)))
    return checks


def media_dir_of(name):
    """Where a fixture converted with no -o puts its pictures: <stem>_media beside the .md."""
    return os.path.join(make_fixtures.BUILD, name[:-5] + "_media")


def clear_dir(path):
    if not os.path.isdir(path):
        return
    for leaf in os.listdir(path):
        os.remove(os.path.join(path, leaf))
    os.rmdir(path)


def check_media(exe, row, failures):
    """Converts one extracting fixture and compares every file it wrote, byte for byte."""
    name = row["name"]
    source = os.path.join(make_fixtures.BUILD, name)
    folder = media_dir_of(name)
    checks = 0

    clear_dir(folder)
    code, out, err = run(exe, [source])
    checks += 1
    if code != 0:
        failures.append((name, "exit %s, expected 0" % code, "media"))
        print("FAIL  %-28s exit %s, expected 0" % (name, code))
        return checks
    if not os.path.isdir(folder):
        failures.append((name, "no media directory", "media"))
        print("FAIL  %-28s wrote no %s" % (name, os.path.basename(folder)))
        return checks

    wanted = dict(row["files"])
    found = sorted(os.listdir(folder))
    if found != sorted(wanted):
        failures.append((name, "media directory holds %s" % found, "media"))
        print("FAIL  %-28s expected %s, found %s" % (name, sorted(wanted), found))
        return checks
    for leaf in sorted(wanted):
        with open(os.path.join(folder, leaf), "rb") as handle:
            produced = handle.read()
        checks += 1
        if produced != wanted[leaf]:
            failures.append((name, "%s differs" % leaf, "media"))
            print("FAIL  %-28s %s: %d bytes extracted, %d expected" % (name, leaf, len(produced), len(wanted[leaf])))
        else:
            print("ok    %-28s %s is the %d bytes the part holds" % (name, leaf, len(produced)))
    return checks


def check_media_options(exe, failures):
    """--no-images keeps the alt text and writes nothing; --media-dir puts the files where it says."""
    checks = 0
    source = os.path.join(make_fixtures.BUILD, "images.docx")

    named = os.path.join(make_fixtures.BUILD, "no-images.md")
    folder = os.path.join(make_fixtures.BUILD, "no-images_media")
    clear_dir(folder)
    if os.path.exists(named):
        os.remove(named)
    code, out, err = run(exe, ["--no-images", "-o", named, source])
    checks += 1
    produced = open(named, "rb").read() if os.path.exists(named) else b""
    # The document still holds a hyperlink, so "](" alone proves nothing: what --no-images promises
    # is that no image marker and no media path survive, and that the alt text does.
    if code != 0 or os.path.isdir(folder) or b"![" in produced or b"images_media/" in produced or b"A cat" not in produced:
        failures.append(("--no-images", "exit %s" % code, "images off"))
        print("FAIL  %-28s --no-images wrote a picture, a media path, or no alt text" % "--no-images")
    else:
        print("ok    %-28s --no-images keeps the alt text and writes no files" % "--no-images")

    # --media-dir is a path as the user typed it, so the run has to happen where they would have
    # typed it: the emitted path is that same string, and only a matching working directory makes
    # the document and the files agree.
    pics = os.path.join(make_fixtures.BUILD, "pics")
    into = os.path.join(make_fixtures.BUILD, "named-media.md")
    clear_dir(pics)
    if os.path.exists(into):
        os.remove(into)
    code, out, err = run(exe, ["--media-dir", "pics", "-o", "named-media.md", "images.docx"], cwd=make_fixtures.BUILD)
    checks += 1
    produced = open(into, "rb").read() if os.path.exists(into) else b""
    if code != 0 or not os.path.exists(os.path.join(pics, "image1.png")) or b"](pics/image1.png)" not in produced:
        failures.append(("--media-dir", "exit %s" % code, "named directory"))
        print("FAIL  %-28s --media-dir did not fill the directory it named" % "--media-dir")
        print("      %s" % err.strip().replace("\n", "\n      "))
    else:
        print("ok    %-28s --media-dir puts the files where it says and links them there" % "--media-dir")
    return checks


def check_output_option(exe, failures):
    """The -o rules of D7b: a filename for one input, a directory for several."""
    checks = 0
    golden = make_fixtures.GOLDENS[0]
    source = os.path.join(make_fixtures.BUILD, golden["name"])
    with open(golden["expected"], "rb") as handle:
        wanted = handle.read()

    named = os.path.join(make_fixtures.BUILD, "named-output.md")
    if os.path.exists(named):
        os.remove(named)
    code, out, err = run(exe, ["-o", named, source])
    checks += 1
    if code != 0 or not os.path.exists(named) or open(named, "rb").read() != wanted:
        failures.append(("-o <file>", "exit %s" % code, "one input"))
        print("FAIL  %-28s -o with one input did not write the named file" % "-o <file>")
    else:
        print("ok    %-28s -o with one input writes exactly that file" % "-o <file>")

    folder = os.path.join(make_fixtures.BUILD, "out-dir")
    if not os.path.isdir(folder):
        os.makedirs(folder)
    second = make_fixtures.GOLDENS[1]
    into = [os.path.join(folder, golden["name"][:-5] + ".md"), os.path.join(folder, second["name"][:-5] + ".md")]
    for path in into:
        if os.path.exists(path):
            os.remove(path)
    code, out, err = run(exe, ["-o", folder, source, os.path.join(make_fixtures.BUILD, second["name"])])
    checks += 1
    if code != 0 or not all(os.path.exists(path) for path in into):
        failures.append(("-o <dir>", "exit %s" % code, "two inputs"))
        print("FAIL  %-28s -o with two inputs did not fill the directory" % "-o <dir>")
        print("      %s" % err.strip().replace("\n", "\n      "))
    else:
        with open(second["expected"], "rb") as handle:
            other = handle.read()
        if open(into[0], "rb").read() != wanted or open(into[1], "rb").read() != other:
            failures.append(("-o <dir>", "bytes differ", "two inputs"))
            print("FAIL  %-28s -o with two inputs wrote the wrong bytes" % "-o <dir>")
        else:
            print("ok    %-28s -o with two inputs writes one .md per input into it" % "-o <dir>")

    code, out, err = run(exe, ["--stdout", source, os.path.join(make_fixtures.BUILD, second["name"])])
    checks += 1
    if code != 1:
        failures.append(("--stdout x2", "exit %s, expected 1" % code, "two inputs"))
        print("FAIL  %-28s --stdout with two inputs is exit %s, expected 1" % ("--stdout x2", code))
    else:
        print("ok    %-28s --stdout with two inputs is a usage error" % "--stdout x2")

    corrupt = os.path.join(make_fixtures.BUILD, "corrupt-deflate.docx")

    code, out, err = run(exe, [source, corrupt])
    checks += 1
    if code != 6 or "deflate stream" not in err:
        failures.append(("exit 6", "exit %s, expected 6" % code, "one good input and one bad"))
        print("FAIL  %-28s a mixed run is exit %s, expected 6" % ("partial success", code))
    else:
        print("ok    %-28s one good input and one bad is exit 6, and the bad one is named" % "partial success")

    code, out, err = run(exe, [corrupt, corrupt])
    checks += 1
    if code != 3:
        failures.append(("all failed", "exit %s, expected 3" % code, "two bad inputs"))
        print("FAIL  %-28s a run where everything failed is exit %s, expected 3" % ("total failure", code))
    else:
        print("ok    %-28s a run where everything failed returns the per-file verdict" % "total failure")

    trailing = os.path.join(make_fixtures.BUILD, "out-dir") + os.sep
    expected_leaf = os.path.join(make_fixtures.BUILD, "out-dir", golden["name"][:-5] + ".md")
    if os.path.exists(expected_leaf):
        os.remove(expected_leaf)
    code, out, err = run(exe, ["-o", trailing, source])
    checks += 1
    if code != 0 or not os.path.exists(expected_leaf):
        failures.append(("-o <dir>/", "exit %s" % code, "one input"))
        print("FAIL  %-28s -o with a trailing separator did not write into the directory" % "-o <dir>/")
    else:
        print("ok    %-28s -o with a trailing separator is a directory even for one input" % "-o <dir>/")

    code, out, err = run(exe, ["-q", source])
    checks += 1
    if code != 0 or "note:" in err:
        failures.append(("-q", "exit %s" % code, "quiet"))
        print("FAIL  %-28s -q still printed a note, or did not convert" % "-q")
    else:
        print("ok    %-28s -q converts and says nothing" % "-q")
    return checks


def main(argv):
    exe = DEFAULT_EXE
    if "--exe" in argv:
        exe = argv[argv.index("--exe") + 1]

    make_fixtures.build_all(verbose=False)
    failures = []
    total = 0

    print("golden fixtures")
    for row in make_fixtures.GOLDENS:
        total += check_case(exe, row, failures)

    print()
    print("extracted media")
    for row in make_fixtures.MEDIA:
        total += check_media(exe, row, failures)

    print()
    print("the output options")
    total += check_output_option(exe, failures)
    total += check_media_options(exe, failures)

    print()
    if failures:
        print("%d of %d checks failed" % (len(failures), total))
        return 1
    print("all %d checks passed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
