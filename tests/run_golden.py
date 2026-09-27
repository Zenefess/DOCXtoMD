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

Since M13 it also converts every fixture as one batch -- valid and corrupt inputs together -- and compares
the tree that writes against the tree the same inputs write one at a time, at --threads 1 and at the widest
count the machine allows up to 8, which is M13's definition of done.
"""

import os
import re
import shutil
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

    # A media path is generated, so the three bytes MD_CONTEXT_LINK_DEST leaves alone in a producer's own
    # target are ordinary bytes of a file name here: unencoded, "sh#ots/image1.png" is a fragment of the
    # document rather than a path to a picture. A trailing separator is trimmed for the same reason -- the
    # emitted path joins with one of its own -- so both are asserted from the one run.
    odd = os.path.join(make_fixtures.BUILD, "sh#ots")
    into = os.path.join(make_fixtures.BUILD, "odd-media.md")
    clear_dir(odd)
    if os.path.exists(into):
        os.remove(into)
    code, out, err = run(exe, ["--media-dir", "sh#ots/", "-o", "odd-media.md", "images.docx"], cwd=make_fixtures.BUILD)
    checks += 1
    produced = open(into, "rb").read() if os.path.exists(into) else b""
    if code != 0 or not os.path.exists(os.path.join(odd, "image1.png")) or b"](sh%23ots/image1.png)" not in produced:
        failures.append(("--media-dir #", "exit %s" % code, "encoded directory"))
        print("FAIL  %-28s --media-dir did not encode '#' or trimmed nothing" % "--media-dir #")
        print("      %s" % err.strip().replace("\n", "\n      "))
    else:
        print("ok    %-28s --media-dir encodes a '#' and drops a trailing separator" % "--media-dir #")
    return checks


# What --tables=html-on-merge must make of tests/fixtures/tablemerges, which under the default policy
# is the pipe table its own expected.md pins. Written out here rather than as a second golden tree,
# because what it pins is a *policy* over one document and not a second document.
TABLE_HTML = b"""Merged cells, padded into a GFM grid.

<table>
<tr><th>One</th><th>Two</th><th>Three</th><th></th></tr>
<tr><td colspan="2">spans two columns</td><td>c2</td><td></td></tr>
<tr><td rowspan="2">spans two rows</td><td>b3</td><td>c3</td><td></td></tr>
<tr><td>b4</td><td>c4</td><td></td></tr>
<tr><td colspan="3">the whole width</td><td></td></tr>
<tr><td>a5</td><td>b5</td><td>c5</td><td>past the grid</td></tr>
</table>

After the merges.
"""


def check_table_option(exe, failures):
    """--tables: html-on-merge keeps a merge as colspan and rowspan; a table with none is unaffected."""
    checks = 0
    merges = os.path.join(make_fixtures.BUILD, "tablemerges.docx")
    plain = os.path.join(make_fixtures.BUILD, "tables.docx")

    code, out, err = run(exe, ["--tables=html-on-merge", "--stdout", merges])
    checks += 1
    if code != 0 or out != TABLE_HTML:
        failures.append(("--tables=html-on-merge", "exit %s" % code, "merged table"))
        print("FAIL  %-28s a merged table did not become the expected raw <table>" % "--tables")
        show("html-on-merge", out, TABLE_HTML)
    else:
        print("ok    %-28s a merged table becomes a raw <table> with colspan and rowspan" % "--tables")

    # A table with no merge in it is the same bytes under either policy, which is what keeps the flag
    # about merges rather than about tables.
    with open(os.path.join(make_fixtures.FIXTURES, "tables", "expected.md"), "rb") as handle:
        wanted = handle.read()
    code, out, err = run(exe, ["--tables=html-on-merge", "--stdout", plain])
    checks += 1
    if code != 0 or out != wanted:
        failures.append(("--tables unmerged", "exit %s" % code, "unmerged table"))
        print("FAIL  %-28s a table with no merge changed under html-on-merge" % "--tables unmerged")
    else:
        print("ok    %-28s a table with no merge is the same bytes under either policy" % "--tables unmerged")

    # A nested table has no pipe form at all, so it is raw HTML under the default policy too.
    with open(os.path.join(make_fixtures.FIXTURES, "tablenested", "expected.md"), "rb") as handle:
        wanted = handle.read()
    code, out, err = run(exe, ["--tables=gfm", "--stdout", os.path.join(make_fixtures.BUILD, "tablenested.docx")])
    checks += 1
    if code != 0 or out != wanted:
        failures.append(("--tables=gfm nested", "exit %s" % code, "nested table"))
        print("FAIL  %-28s a nested table did not stay raw HTML under gfm" % "--tables=gfm nested")
    else:
        print("ok    %-28s a nested table is raw HTML under gfm as well" % "--tables=gfm nested")

    code, out, err = run(exe, ["--tables=nonsense", plain])
    checks += 1
    if code != 1 or "--tables takes gfm or html-on-merge" not in err:
        failures.append(("--tables bad value", "exit %s" % code, "usage"))
        print("FAIL  %-28s a bad --tables value is exit %s, expected 1" % ("--tables bad value", code))
    else:
        print("ok    %-28s a bad --tables value is a usage error that names both spellings" % "--tables bad")
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


# -- M13: the batch

# The phrase Diag gives each per-input verdict in the failure list, which is user-facing text and so is
# pinned here as well as in the unit suite.
VERDICT_TEXT = {2: "the input could not be read", 3: "not a valid DOCX", 4: "the output could not be written",
                5: "an internal error"}

# An input no fixture is called, so that the batch carries an unopenable file as well as unusable ones.
ABSENT = "no-such-input.docx"

# How many times the many-thread batch is repeated. A race shows up as a difference between two runs of
# the same command line, and one repeat is a small sample of the orders a scheduler can choose.
REPEATS = 3


def tree_of(root):
    """Every file under root, keyed by its path relative to root with forward slashes."""
    found = {}
    for folder, dirs, files in os.walk(root):
        for leaf in files:
            path = os.path.join(folder, leaf)
            with open(path, "rb") as handle:
                found[os.path.relpath(path, root).replace(os.sep, "/")] = handle.read()
    return found


def fresh_dir(path):
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path)


def widest_threads(exe):
    """The exe's own ceiling for --threads, which is the virtual core count the machine reports.

    It is read from the sentence the exe refuses a larger count with, so the runner and the exe cannot
    disagree about how many cores there are.
    """
    code, out, err = run(exe, ["--threads", "4294967295", ABSENT])
    found = re.search(r"--threads must be 1 to (\d+),", err)
    return code, (int(found.group(1)) if found else None)


def summary_of(err, failed, total):
    """The failure list is the last thing a run writes: a heading and one line per failed input."""
    lines = err.splitlines()
    return lines[-(failed + 1):] if failed and len(lines) > failed else []


def diff_trees(label, left, right, failures, why):
    """Reports the first difference between two trees; returns whether they were identical."""
    if left == right:
        return True
    only_left = sorted(set(left) - set(right))
    only_right = sorted(set(right) - set(left))
    differ = sorted(path for path in set(left) & set(right) if left[path] != right[path])
    failures.append((label, why, "trees differ"))
    print("FAIL  %-28s %s" % (label, why))
    for name, paths in (("only in the first", only_left), ("only in the second", only_right), ("bytes differ", differ)):
        if paths:
            print("      %s: %s" % (name, ", ".join(paths[:6]) + (" ..." if len(paths) > 6 else "")))
    return False


def check_batch(exe, failures):
    """M13's definition of done: one batch against the same inputs one at a time, at 1 thread and at many."""
    checks = 0
    exe = os.path.abspath(exe)
    build = make_fixtures.BUILD
    rows = make_fixtures.EXPECTATIONS # main() has built them; the table is every fixture and its exit code
    if os.path.exists(os.path.join(build, ABSENT)):
        os.remove(os.path.join(build, ABSENT))

    # Every fixture, in the table's order, with the unopenable input among them rather than at an end.
    names = [row["name"] for row in rows]
    names.insert(len(names) // 3, ABSENT)
    wanted = dict((row["name"], row["code"]) for row in rows)
    wanted[ABSENT] = 2
    failed = [name for name in names if wanted[name] != 0]
    goldens = dict((row["name"], row["expected"]) for row in make_fixtures.GOLDENS)

    code, cores = widest_threads(exe)
    checks += 1
    if code != 1 or not cores:
        failures.append(("--threads", "exit %s" % code, "core count"))
        print("FAIL  %-28s a --threads count above the core count is exit %s and names no ceiling" % "--threads")
        return checks
    many = min(8, cores)
    print("ok    %-28s --threads above %d is a usage error, so the many-thread runs use %d" % ("--threads", cores, many))
    if many < 2:
        print("note  %-28s this machine reports one core, so no run here converts two inputs at once" % "--threads")

    # One at a time, each with -o naming its own file, so the pictures land beside it exactly as a batch
    # with -o naming the directory puts them.
    single = os.path.join(build, "batch-single")
    fresh_dir(single)
    for name in names:
        code, out, err = run(exe, ["-o", os.path.join("batch-single", name[:-5] + ".md"), name], cwd=build)
        checks += 1
        if code != wanted[name]:
            failures.append((name, "exit %s, expected %s" % (code, wanted[name]), "one at a time"))
            print("FAIL  %-28s converted alone, exit %s, expected %s" % (name, code, wanted[name]))
    one_by_one = tree_of(single)
    print("ok    %-28s %d inputs converted one at a time wrote %d files" % ("one at a time", len(names), len(one_by_one)))

    # The same inputs as one batch at --threads 1, and then at the widest count, more than once.
    batch = os.path.join(build, "batch-run")
    header = "DOCXtoMD: error: %d of %d inputs failed:" % (len(failed), len(names))
    listed = [header] + ["DOCXtoMD: error: failed (exit %d, %s): %s" % (wanted[name], VERDICT_TEXT[wanted[name]], name)
                         for name in failed]
    runs = []
    for threads in [1] + [many] * REPEATS:
        fresh_dir(batch)
        code, out, err = run(exe, ["--threads", str(threads), "-o", "batch-run" + os.sep] + names, cwd=build)
        runs.append((threads, code, err, tree_of(batch)))

    first_threads, first_code, first_err, first_tree = runs[0]
    for threads, code, err, tree in runs:
        label = "batch --threads %d" % threads
        checks += 1
        if code != 6:
            failures.append((label, "exit %s, expected 6" % code, "mixed batch"))
            print("FAIL  %-28s a batch of valid and corrupt inputs is exit %s, expected 6" % (label, code))
        else:
            print("ok    %-28s a batch of %d valid and %d failing inputs is exit 6" % (label, len(names) - len(failed), len(failed)))
        checks += 1
        if diff_trees(label, one_by_one, tree, failures, "the batch did not write what one-at-a-time wrote"):
            print("ok    %-28s the batch wrote the same %d files, byte for byte, as one at a time" % (label, len(tree)))
        checks += 1
        if summary_of(err, len(failed), len(names)) != listed:
            failures.append((label, "failure list", "mixed batch"))
            print("FAIL  %-28s the run did not end by listing every failed input in argument order" % label)
            for line in summary_of(err, len(failed), len(names))[:4]:
                print("      %s" % line)
        else:
            print("ok    %-28s the run ends by listing all %d failed inputs, in argument order" % (label, len(failed)))
        # Every line a worker writes is written whole, so the lines of two runs are the same lines, only
        # perhaps in another order -- and the list at the end is in the same order in both.
        checks += 1
        if sorted(err.splitlines()) != sorted(first_err.splitlines()):
            failures.append((label, "stderr lines", "mixed batch"))
            print("FAIL  %-28s the console lines differ from the --threads 1 run's, beyond their order" % label)
        else:
            print("ok    %-28s the same %d console lines as --threads 1, each one whole" % (label, len(err.splitlines())))

    # What the batch wrote, against the documents themselves: every valid input converted, every golden to
    # its expected.md, and nothing at all for an input that failed.
    checks += 1
    missing = [name for name in names if wanted[name] == 0 and name[:-5] + ".md" not in first_tree]
    stray = [name for name in failed if name[:-5] + ".md" in first_tree]
    wrong = []
    for name in names:
        if name in goldens and name[:-5] + ".md" in first_tree:
            with open(goldens[name], "rb") as handle:
                if first_tree[name[:-5] + ".md"] != handle.read():
                    wrong.append(name)
    if missing or stray or wrong:
        failures.append(("batch outputs", "missing %s stray %s wrong %s" % (missing, stray, wrong), "mixed batch"))
        print("FAIL  %-28s missing %s, written for a failed input %s, differing from expected.md %s"
              % ("batch outputs", missing[:3], stray[:3], wrong[:3]))
    else:
        print("ok    %-28s every valid input converted, %d of them to their expected.md, and no failed one"
              % ("batch outputs", len([name for name in names if name in goldens])))

    # The valid inputs alone are a run with nothing to list.
    valid = [name for name in names if wanted[name] == 0]
    fresh_dir(batch)
    code, out, err = run(exe, ["--threads", str(many), "-o", "batch-run" + os.sep] + valid, cwd=build)
    tree = tree_of(batch)
    checks += 1
    if code != 0 or "inputs failed" in err or tree != first_tree:
        failures.append(("valid batch", "exit %s" % code, "all converted"))
        print("FAIL  %-28s the valid inputs alone are exit %s, or listed failures, or wrote other bytes" % ("valid batch", code))
    else:
        print("ok    %-28s the %d valid inputs alone are exit 0, list nothing and write the same files" % ("valid batch", len(valid)))

    checks += check_batch_preflight(exe, many, failures)
    return checks


def check_batch_preflight(exe, many, failures):
    """What the pre-flight refuses, and what it keeps to one conversion, before any worker starts."""
    checks = 0
    build = make_fixtures.BUILD
    source = make_fixtures.GOLDENS[0]
    with open(source["expected"], "rb") as handle:
        first = handle.read()

    # One input named twice, in two spellings of one path, is one document: converted once, exit 0.
    target = os.path.join(build, "batch-repeat")
    fresh_dir(target)
    twice = [source["name"], os.path.join(".", source["name"])]
    code, out, err = run(exe, ["--threads", str(many), "-o", "batch-repeat" + os.sep] + twice, cwd=build)
    tree = tree_of(target)
    checks += 1
    if code != 0 or list(tree) != [source["name"][:-5] + ".md"] or "so converted once" not in err:
        failures.append(("repeat", "exit %s" % code, "one input twice"))
        print("FAIL  %-28s one input named twice is exit %s, or wrote %s" % ("repeat", code, sorted(tree)))
    else:
        print("ok    %-28s one input named in two spellings is converted once, exit 0" % "repeat")

    # Two inputs with one leaf name in two directories both target one .md under -o: the first keeps it.
    # The second is a different document under the first one's name, so which of them won is visible.
    unlike = next(row for row in make_fixtures.GOLDENS if row["expected"] != source["expected"])
    other = os.path.join(build, "batch-leaf")
    fresh_dir(other)
    shutil.copyfile(os.path.join(build, unlike["name"]), os.path.join(other, source["name"]))
    target = os.path.join(build, "batch-claimed")
    fresh_dir(target)
    second = os.path.join("batch-leaf", source["name"])
    code, out, err = run(exe, ["--threads", str(many), "-o", "batch-claimed" + os.sep, source["name"], second], cwd=build)
    tree = tree_of(target)
    checks += 1
    if (code != 6 or tree.get(source["name"][:-5] + ".md") != first
            or "an earlier input already writes that output file: " + second not in err):
        failures.append(("claimed", "exit %s" % code, "same leaf twice"))
        print("FAIL  %-28s two inputs with one leaf name are exit %s, or the first lost its output" % ("claimed", code))
    else:
        print("ok    %-28s the first of two inputs with one leaf name keeps the output, the second is refused" % "claimed")

    # A --media-dir named with several inputs is one directory, which the first input converted owns.
    shared = os.path.join(build, "batch-pics")
    target = os.path.join(build, "batch-media")
    shutil.rmtree(shared, ignore_errors=True)
    fresh_dir(target)
    code, out, err = run(exe, ["--threads", str(many), "--media-dir", "batch-pics", "-o", "batch-media" + os.sep,
                               "images.docx", "footnotes.docx"], cwd=build)
    tree = tree_of(target)
    pictures = tree_of(shared) if os.path.isdir(shared) else {}
    owned = dict(next(row["files"] for row in make_fixtures.MEDIA if row["name"] == "images.docx"))
    checks += 1
    if (code != 6 or list(tree) != ["images.md"] or pictures != owned
            or "already extracts its pictures into that --media-dir: footnotes.docx" not in err):
        failures.append(("--media-dir x2", "exit %s" % code, "shared media directory"))
        print("FAIL  %-28s a shared --media-dir is exit %s, wrote %s, or holds other pictures" % ("--media-dir x2", code, sorted(tree)))
    else:
        print("ok    %-28s a shared --media-dir belongs to the first input; the second is refused" % "--media-dir x2")

    # --no-images writes no picture, so the same command line shares nothing and converts both.
    fresh_dir(target)
    shutil.rmtree(shared, ignore_errors=True)
    code, out, err = run(exe, ["--threads", str(many), "--no-images", "--media-dir", "batch-pics", "-o", "batch-media" + os.sep,
                               "images.docx", "footnotes.docx"], cwd=build)
    checks += 1
    if code != 0 or sorted(tree_of(target)) != ["footnotes.md", "images.md"] or os.path.isdir(shared):
        failures.append(("--media-dir --no-images", "exit %s" % code, "nothing shared"))
        print("FAIL  %-28s --no-images with a --media-dir and two inputs is exit %s" % ("--media-dir no-img", code))
    else:
        print("ok    %-28s --no-images shares no directory, so both inputs convert" % "--media-dir no-img")
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
    total += check_table_option(exe, failures)

    print()
    print("the batch (M13)")
    total += check_batch(exe, failures)

    print()
    if failures:
        print("%d of %d checks failed" % (len(failures), total))
        return 1
    print("all %d checks passed" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
