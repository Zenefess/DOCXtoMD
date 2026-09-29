# DOCXtoMD

DOCXtoMD is a native Windows command-line tool that converts Word documents (`.docx`, Office Open XML
WordprocessingML) into GitHub-flavored Markdown. It is written in C++ for x64 with no third-party code
at all: the ZIP container, the DEFLATE decoder and the XML tokenizer are all first-party, so the
executable depends on nothing but Windows and the C runtime. One document is converted on one thread;
a list of documents is converted on a bounded pool of worker threads, one document per worker.

It reads the package the way the specification says to (parts are found through relationships, never
by name), resolves effective formatting through the style chain, merges the run fragments Word leaves
behind so that emphasis delimiters land where they parse, and escapes every byte of text through one
context-aware writer so that document text is never re-read as markup. Hostile input is expected: ZIP
bombs are capped while inflating, entry names that could escape the package are refused, `<!DOCTYPE`
is rejected outright, and a password-protected `.docx` or a legacy `.doc` is reported rather than
crashed on.

Version 0.1.0.

## Requirements

- 64-bit Windows on a processor with AVX2. The binary is compiled with `/arch:AVX2` (which also
  licenses FMA3 and BMI2) and has no fallback path for older CPUs.
- To build: Visual Studio 2022 or later with the v143 toolset, MSBuild and a Windows 10 SDK. The
  code is C++20. x64 is the only platform; a Win32 build fails by design.
- To run the tests: Python 3 (CI uses 3.12 and 3.14) and, for the formatter check only,
  clang-format 18.1.3.

## Usage

```
Usage: DOCXtoMD [options] <input.docx> [input2.docx [input3.docx [...]]]
  -o, --output <path>    Output path: the .md filename for a single input,
                         the destination directory when several are given
  -j, --threads <n>      Worker threads (default: system virtual core count)
  --media-dir <dir>      Image dir (default: <stem>_media\)
  --no-images            Alt text only

  --hard-break=<backslash|spaces>    (default: backslash)
  --tables=<gfm|html-on-merge>       (default: gfm)

  --stdout               Markdown to stdout - single input only
  --version              Print version
  -q, --quiet            Errors only
  -h, --help             Usage
```

Every operand that does not begin with `-` is an input file; there is no positional output operand.
Each input is converted to `<stem>.md` beside it unless `-o` says otherwise, and any pictures it draws
are extracted into a directory next to the Markdown.

### Options

| Option | Effect |
|---|---|
| `-o <path>`, `--output <path>` | With one input, the `.md` file to write, used exactly as written. With several inputs, the directory each `<stem>.md` is written into. A value ending in `\` or `/` is a directory whatever the input count. The directory must already exist. Cannot be combined with `--stdout`. |
| `-j <n>`, `--threads <n>` | The most worker threads a run of several inputs may use, the calling thread included. Default: the number of logical processors the system reports, across all processor groups. `n` must be a whole number from 1 to that count; `0`, a larger value or anything that is not a number is a usage error. A single input always converts on one thread. |
| `--media-dir <dir>` | The directory extracted images are written to. The Markdown links to the same path -- with `\` folded to `/`, any trailing separator dropped, and `#`, `%` and `?` percent-encoded -- so the links are relative to the working directory rather than to the `.md`. Default: `<stem>_media\` beside the Markdown, linked as `<stem>_media/imageN.ext`. The directory itself is created when the first picture is written; its parents must exist. With several inputs one named directory is claimed by the first input the pre-flight lets through, and every later input is refused with exit 4 unless `--no-images` is also given. |
| `--no-images` | Extract nothing; every picture becomes its alt text. |
| `--hard-break=<backslash\|spaces>` | How a line break inside a paragraph is written: a trailing backslash (default) or two trailing spaces. |
| `--tables=<gfm\|html-on-merge>` | `gfm` (default) always writes a pipe table, padding merged cells into empty ones. `html-on-merge` writes a raw `<table>` for any table holding a horizontal or vertical merge. A table that contains another table is raw HTML under either setting, because a pipe table cannot express it. |
| `--stdout` | Write the Markdown to standard output instead of a file. Single input only, and not with `-o`. Notes and errors go to standard error, so stdout carries only the document. Images are still extracted, into `<stem>_media\` beside the input; add `--no-images` to write nothing at all. |
| `-q`, `--quiet` | Suppress progress notes. Errors are still reported. |
| `--version` | Print `DOCXtoMD 0.1.0` and exit 0. |
| `-h`, `--help` | Print the usage text to stdout and exit 0. |

Every long option that takes a value accepts both `--name value` and `--name=value`; the short forms
`-o` and `-j` take the following argument only. `-h`, `--help` and `--version` are answered the moment
they are seen, so anything after them on the line is not parsed; a bad option earlier on the line
still wins. A usage error prints the message and the usage text to standard error and exits 1.

### What the output looks like

- The Markdown is UTF-8 without a byte-order mark, with LF line endings. Blocks are separated by a
  blank line, with the exceptions Markdown itself requires: a run of list items is one list, a run of
  code paragraphs one fence, and two consecutive quotation paragraphs stay in one blockquote.
- An existing file at the output path is overwritten. If the write does not finish, the partial file
  is deleted rather than left looking converted.
- Images are named `image1.ext`, `image2.ext`, ... in the order the document first draws them. The
  extension comes from the content type the package declares; where that type is missing or not one
  the converter knows, a short alphanumeric extension on the part's own name is used, and failing that
  `.bin`. A picture drawn twice is written once.
- Progress notes and errors are one line each on standard error, prefixed `DOCXtoMD: note:` or
  `DOCXtoMD: error:`.

## Examples

### One file

```bat
DOCXtoMD report.docx
```

writes `report.md` beside the input and any pictures into `report_media\`, then reports what it did
(byte and image counts are illustrative):

```
DOCXtoMD: note: wrote 18342 bytes: report.md
DOCXtoMD: note: extracted 3 images into: report_media
```

The output file, the media directory and the table policy can all be named:

```bat
DOCXtoMD -o docs\report.md --media-dir docs\img --tables=html-on-merge report.docx
```

Here `docs\` must already exist, `docs\img\` is created when the first picture is written, and the
pictures are linked from the Markdown as `docs/img/image1.png` -- the `--media-dir` value, not a path
relative to `docs\report.md`.

To send the Markdown down a pipe rather than to a file:

```bat
DOCXtoMD --stdout --no-images report.docx > report.md
DOCXtoMD --stdout --no-images report.docx | more
```

`--stdout` may only be used with one input and cannot be combined with `-o`.

### Several files

```bat
DOCXtoMD -o converted\ -j 4 chapter1.docx chapter2.docx appendix.docx
```

converts the three documents on a pool of up to four workers (three here, since there are only three
inputs), writing `converted\chapter1.md`, `converted\chapter2.md` and `converted\appendix.md`, with
each document's pictures in its own `converted\<stem>_media\`. The run ends with a summary line:

```
DOCXtoMD: note: wrote 40311 bytes: converted\chapter1.md
DOCXtoMD: note: extracted 2 images into: converted\chapter1_media
DOCXtoMD: note: wrote 38720 bytes: converted\chapter2.md
DOCXtoMD: note: wrote 5102 bytes: converted\appendix.md
DOCXtoMD: note: 3 of 3 inputs converted by 3 workers
```

Because the workers run concurrently, the per-file lines may arrive in any order. Each line is written
whole, so lines from two workers never interleave within one another.

When some inputs fail, every valid input is still converted, each failure is reported as it happens,
and the run closes with the failures listed once more in argument order:

```bat
DOCXtoMD -o converted\ chapter1.docx draft.doc missing.docx
```

```
DOCXtoMD: note: wrote 40311 bytes: converted\chapter1.md
DOCXtoMD: error: not a valid DOCX; this is an OLE compound file, so an encrypted .docx or a legacy .doc: draft.doc
DOCXtoMD: error: cannot open input file: missing.docx
DOCXtoMD: note: 1 of 3 inputs converted by 3 workers
DOCXtoMD: error: 2 of 3 inputs failed:
DOCXtoMD: error: failed (exit 3, not a valid DOCX): draft.doc
DOCXtoMD: error: failed (exit 2, the input could not be read): missing.docx
```

That run exits 6: something converted and something did not.

Before any worker starts, the input list is checked in argument order. An input named twice is
converted once, and the repeat takes the first spelling's result. An input whose output would
overwrite the input itself, another input, or an earlier input's output -- however each is spelled --
is refused with exit 4 and named, so two documents called `report.docx` in different directories
cannot silently overwrite each other in one `-o` directory.

Neither `cmd.exe` nor the executable expands wildcards, so `DOCXtoMD *.docx` looks for a file literally
named `*.docx`. List the files, or let PowerShell expand the pattern into separate arguments:

```powershell
.\x64\Release\DOCXtoMD.exe -o converted\ (Get-ChildItem *.docx).Name
```

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Every input converted |
| 1 | Usage error; the message and the usage text are printed to standard error |
| 2 | An input file was missing or could not be read |
| 3 | An input file is not a valid DOCX; the message names the rule it broke and, where one part or entry caused it, which one |
| 4 | An output file or directory could not be written, or the input list was refused before conversion started |
| 5 | Internal error, such as running out of memory |
| 6 | Partial success: at least one input converted and at least one did not |

Codes 2 to 5 are per-input verdicts. With one input the verdict is the exit code. With several, the
process exits 0 when everything converted, 6 when the run was mixed, and otherwise the highest verdict
among the failures.

## What is converted

Paragraphs and headings (heading styles and outline levels, including `Title` and `Subtitle`); bold,
italic, strikethrough, superscript, subscript and inline code; fenced code blocks, blockquotes and
horizontal rules; hyperlinks, bookmark anchors and GitHub-style heading slugs; images, extracted from
the package; bullet and numbered lists with real computed numbers, nesting, restarts and numbering that
arrives through a paragraph style; tables, as GFM pipe tables with alignment from the first row and a
raw `<table>` where the pipe form cannot cope; fields, with `HYPERLINK` and `REF \h` becoming links, a
table of contents vanishing and every other field showing its cached result; footnotes and endnotes as
`[^n]` references and definitions, renumbered in the order they are read; and tracked changes, applied
as accept-all. Hard breaks, non-breaking spaces and smart punctuation are kept; a tab becomes one space
and a non-breaking hyphen an ordinary one; soft hyphens are removed.

Dropped by policy, because Markdown has no spelling for them: underline, highlight, colour and font
size. Skipped: comments, headers and footers, text boxes, equations (`m:oMath`) and symbol characters
inserted through `w:sym`.

The fixture corpus includes real pandoc and LibreOffice exports alongside hand-authored documents, and
`docs/CONVERSION_REFERENCE.md` sets out the complete feature mapping, escaping rules and edge cases.

## Building

From a Visual Studio Developer prompt at the repository root:

```bat
msbuild DOCXtoMD.sln /m /p:Configuration=Release /p:Platform=x64
```

The converter lands at `x64\Release\DOCXtoMD.exe` and the unit-test binary at
`tests\x64\Release\DOCXtoMD.Tests.exe`. Substitute `Debug` for a debug build; the outputs move to the
matching `Debug` directories.

## Tests

After a Release build:

```bat
:: The unit suite. Prints a tally and returns 0 or 1.
tests\x64\Release\DOCXtoMD.Tests.exe

:: Builds every test .docx into tests\build\.
python tests\make_fixtures.py

:: Checks the exit code and message for every fixture.
python tests\run_container.py

:: Byte-compares each conversion against its expected.md.
python tests\run_golden.py

:: Coding-standard checks over src\ and tests\. Needs clang-format 18.1.3 on PATH.
python tests\validate_gcs.py --format
```

`run_container.py` and `run_golden.py` build the fixtures themselves, so either can be run on its own.
The same commands run in CI on `windows-latest` for every push and pull request.

## Repository guide

- `src/` -- the converter, one module per stage of the pipeline.
- `include/` -- shared headers (type aliases, allocators, spin locks) the code builds on.
- `tests/` -- the unit suite, the fixture trees, the three Python runners and the coding-standard
  validator.
- `docs/CONVERSION_REFERENCE.md` -- the DOCX-to-Markdown domain specification.
- `CLAUDE.md` -- design record: architecture, mapping policies, decisions and milestone history.
- `CHANGELOG.md` -- the change history; file prologs carry none.
- `GDC_GCS_v1_1_4.md` and `CONTRIBUTING.MD` -- the coding standard every submission is reviewed against.

## License

MIT. Copyright (c) 2026 David William Bull.
