# kppsite: kpp.net's site generator

A kpp program that writes kpp.net's website as static files: stage 1 of
[docs/library-registry.md](../../docs/library-registry.md), "The website and the library provider
in kpp". It runs on Windows today; NFS's Apache serves what it writes. It uses the official
libraries: [markdown](../../libraries/markdown/README.md), [html](../../libraries/html/README.md),
[json](../../libraries/json/README.md) and [url](../../libraries/url/README.md).

```
cd kppnet\site
kpp --release
build\release\kppsite.exe --repository ..\.. --out C:\kpp-sandbox\site --commit 86c29dc --registry <copy of kpp.net/registry>
```

It writes into `--out`:

| Path | What |
|---|---|
| `index.html` | the home page: what kpp is, the list of documents, the libraries |
| `docs/**.html`, `libraries/<name>/README.html` | every `.md` under the `--pages` directories (`docs` and `libraries` when none is given), as a page |
| `libraries/index.html` | every library: the repository's `libraries/` and the registry's index, with its latest release and its README's first paragraph; a search box filters the table in the browser |
| `libraries/<name>.html` | a library's releases (newest first, with the archive, tree hash and yank reason) and its README |
| `libraries/search.json` | the list as JSON (`name`, `version`, `summary`), for a search that needs more than the table |
| `style.css`, `libraries/search.js` | the one stylesheet, and the table filter |

**Links.** Markdown links between documents become links between their pages (`design.md#x` to
`design.html#x`). A link to a file that is not a page goes to its source on `--source-url`
(GitHub's mirror by default; `blob/master/` for a file, `tree/master/` for a directory). A link to
a file that does not exist, a link that is not a URL, and a link to an anchor that the target page
has no heading for are **warnings** (printed, counted; the page keeps the link as written). Errors
(a missing directory, an index file that is not JSON, one whose name differs from its file's) stop
the run with exit code 1. Headings get GitHub's ids, so anchors written for GitHub work.

**The registry.** `--registry DIR` is a copy of the store's directory
([kppnet/registry/layout.md](../registry/layout.md)): kppsite reads `DIR/index/<name>.json`.
Archive links point at `--registry-url` (https://kpp.net/registry/). The site's library pages live
under `libraries/`, apart from `registry/`, the store kpp downloads from. Without `--registry`
every library shows as not published.

**Not done here:** uploading (the RUNBOOK's rsync to NFS, by kovarex), dates (the standard library
has no date formatting yet; it is asked of thread C), a page per release, rendered docs for
libraries that are only in the registry (their README is in their archive).

## Tests

`kpp --test` in this directory: relative links, titles, summaries, the release table.
`compiler\tests\run_tests.cmd` also builds kppsite and runs it over the repository with
`fixture/registry` (two libraries, one release yanked) as the registry, and checks that it writes
the fixture's pages.

Measured: the repository's 48 Markdown files and 7 libraries in 0.23 s, debug build, one run.
