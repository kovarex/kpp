# A library registry for Kpp (proposed 2026-10-30; hosting decided 2026-10-31)

**The ask** (kovarex): a package installer for Kpp libraries, like Factorio's mods: a service
written in Kpp where libraries are registered, from which a library is added to a project in one
step. This document is research and a proposal. Decided so far: where it lives ([Hosting on
kpp.net](#hosting-on-kppnet-decided-2026-10-31-revised-the-same-day), kovarex 2026-10-31) and, in
[build.md](build.md), how a project adds a library (`project.use("json")` in build.kpp, versions
pinned in build.lock). The open decisions are listed at the end
([Decisions for kovarex](#decisions-for-kovarex)).

**The proposal in one paragraph.** A library is a directory of Kpp source (a top-level code
package, [design.md](design.md) "Code packages") with its own build.kpp
([build.md](build.md)); C code it needs ships as C/C++
source that kppc compiles with its bundled clang, and only prebuilt binaries that cannot be built
from source (Steam SDK, Logitech LED) ship as files, flagged. Every release is an immutable
archive identified by a SHA-256 of its file tree (Zig's model), so any mirror, a cache or a
vendored copy is as good as the registry. Versions are three numbers as in Factorio; a program
holds exactly one version of each library (the one global scope allows no other), chosen by Go's
minimal version selection, and build.lock records the hashes. Because Kpp compile-time code runs
in the interpreter and cannot call C, **adding a library never runs its code on your machine
until you run your program**: no build scripts, no install hooks, the attack surface npm and
cargo have. The registry indexes the top-level names each release declares, so `kppc add json`
reports a name clash with your program or another library before it changes anything. Everything is
hosted on kpp.net at NearlyFreeSpeech.NET (decided): canonical git repositories there, GitHub as
the mirror where pull requests happen, and the website and the registry written in Kpp, first as
static files that Kpp programs generate on Windows, then as Kpp CGI programs and a Kpp HTTP
daemon on FreeBSD; the libraries those need are the first ones the registry hosts. The service in
Kpp (accounts, web pages, publishing API) comes
after, as a dogfooding target, when the standard library has networking.

## Contents
- [What exists today](#what-exists-today)
- [Research](#research): [Factorio mods](#factorio-the-mod-portal-and-the-in-game-mod-manager),
  [cargo and crates.io](#cargo-and-cratesio), [Go modules](#go-modules),
  [vcpkg and Conan](#vcpkg-and-conan), [Zig](#zigs-package-manager), [npm](#npm-and-its-incidents),
  [lessons](#lessons-for-kpp)
- [Design for Kpp](#design-for-kpp): [what a library is](#what-a-library-is),
  [names](#names-the-one-global-scope), [C dependencies](#c-dependencies),
  [versions and the lockfile](#versions-resolution-and-the-lockfile),
  [the commands](#the-commands-and-the-project-description),
  [the service](#the-service), [hosting on kpp.net](#hosting-on-kppnet-decided-2026-10-31-revised-the-same-day),
  [security](#security-and-moderation),
  [mirrors and offline](#mirrors-offline-and-private-registries), [IDE and VS](#ide-and-visual-studio),
  [licensing](#licensing-decided-2026-11-01)
- [Phases and effort](#phases-and-effort), [Risks](#risks),
  [Decisions for kovarex](#decisions-for-kovarex), [Sources](#sources)

## What exists today
- `kppc` takes the program's files, compiles the standard library from source with every program
  (`--std DIR`, `--no-std`), and links C libraries given with `--lib`; `extern "C" import "h.h";`
  imports C headers found through `--cinclude DIR` ([design.md](design.md) section 5).
- Code packages (proposed 2026-10-27): a directory is a package, the source roots (program,
  `stdlib/`, "each library") are the top-level packages, package names never appear in code, and
  all top-level names are in **one global scope**, so a clash between two packages is a compile
  error that names both.
- The converted game already has the shape of a project with C dependencies:
  `programs/factorio/native/` holds C interfaces over C++ libraries (`imgui_c.cpp`, `re2_c.cpp`,
  `open_vcdiff_c.cpp`, `jc_voronoi.cpp`...) compiled by clang, plus prebuilt `.lib` files
  (`LogitechLEDLib.lib`), linked by `link.cmd`. Each of these is what a registry library with a C
  part would package.
- Thread E is defining the build description (project file, named configurations) and the
  Visual Studio integration; thread F an IDE ([ide.md](ide.md), not yet written); thread G native
  tests (`kppc --test`). None of their documents is on master yet, so this one states what it
  needs from them ([below](#what-this-needs-from-thread-e)) instead of a syntax.

## Research

### Factorio: the mod portal and the in-game mod manager
The closest model, and the one kovarex named.
- **A mod is a zip** named `{name}_{version}.zip` with one mandatory file, `info.json`: `name`
  (portal: 4 to 49 letters, digits, `-`, `_`), `version` (`a.b.c`, each 0 to 65535), `title`,
  `author`, `factorio_version` (`"2.0"`: the major game version it is for), optional
  `description`, `contact`, `homepage`, `dependencies`, and feature flags such as
  `space_travel_required` [1].
- **Dependencies** are strings `"<prefix> name <op> version"` with `<`, `<=`, `=`, `>=`, `>`:
  no prefix = required (and loaded first), `!` = incompatible, `?` = optional, `(?)` = hidden
  optional, `+` = recommended, `~` = required without affecting load order. A present optional
  dependency whose version does not match disables the mod. The default is `["base"]`, the game
  itself, so the game version is just another dependency [1].
- **The portal API** [2]: read endpoints need no login (`/api/mods`, paginated and filterable by
  game version; `/api/mods/{name}/full` with every release). A release carries `version`,
  `released_at`, `info_json` (its dependencies), `file_name`, `download_url` and a `sha1`.
  Downloads need the player's `username` and `token` (from `player-data.json`, i.e. the game
  account). Publishing is an API too: `init_upload` / `init_publish` with a scoped API key, then
  the zip is posted; the portal validates `info.json` and the zip (`InvalidModRelease`,
  `InvalidModUpload`) [3][4]. Mod pages carry a category, tags, a license from a short list
  (MIT, GPLv3, Apache 2 or custom) and a source URL [5].
- **One-click install and sync.** The in-game manager searches the portal, installs with
  dependencies and updates. *Sync mods with save* (in Load game) enables the mods a save was made
  with, downloading missing ones; by default the latest versions, with Ctrl the exact versions in
  the save; it can also sync startup settings [6][7]. In multiplayer every peer must have exactly
  the same mods, compared by checksum when joining [8], and a joining client downloads the
  server's mods from the portal.
- What it gets right for Kpp: one version of a mod per game (Kpp: one version of a library per
  program), the game version as a dependency (Kpp: the compiler version), the exact mod set
  recorded in the artifact (a save) so it can be restored, and one account system for players and
  authors. What it does not need and Kpp does: content hashes stronger than SHA-1, a lockfile for
  source control, offline and mirrored use.

### cargo and crates.io
- Manifest `Cargo.toml`, versions are semver, `foo = "1.2"` means `>=1.2.0, <2.0.0`; the resolver
  picks the *newest* compatible version, and `Cargo.lock` records exact versions and checksums.
  Several major versions of one crate can coexist in a program (they are distinct crates by
  symbol mangling); Kpp cannot do that ([below](#names-the-one-global-scope)).
- **Yank, not delete**: a yanked version cannot be newly locked, but a project whose lockfile
  already has it still downloads and builds it [9]. Nothing is ever deleted except malware.
- `build.rs` and procedural macros run arbitrary code at build time: the surface used by
  malicious crates. Typosquats are found and removed regularly: `faster_log` and `async_println`
  (published May 2025, 7,181 and 1,243 downloads, stole private keys from log files, removed
  September 2025) [10]; `rands` imitating `rand` [11]; `finch-rst` (December 2025) [12].
- **Trusted publishing** (July 2025): CI publishes with a short-lived token obtained through
  GitHub's OIDC identity, no stored secret; since January 2026 a crate can require it [13][14].
  Login is through GitHub accounts only.

### Go modules
- No central registry: a module is named by its repository path. `proxy.golang.org` caches every
  version it has served, so a deleted repository does not break builds.
- **Minimal version selection** [15][16]: each module states the *minimum* version of each
  dependency; the build uses, for each module, the highest of the minimums stated anywhere in the
  graph. No SAT solver, no "newest available" step, so the result is reproducible without a
  lockfile and changes only when someone edits a requirement. A new release of a dependency never
  changes your build by itself.
- `go.sum` holds hashes of every module version used, checked against `sum.golang.org`, a
  transparency log (a Merkle tree, like certificate transparency): everyone gets the same bytes
  for the same version, or the log shows the lie [15]. `GONOSUMDB` excludes private modules.

### vcpkg and Conan
The two C++ package managers, both built around compiling native code for many configurations.
- **vcpkg**: ports are build recipes (CMake scripts) in a git registry; the manifest pins a
  `builtin-baseline` (a commit of the registry) that fixes the minimum version of every port;
  git or filesystem registries can be added for private ports; built binaries are cached and
  restored by an ABI hash from a file share, NuGet feed or cloud storage [17][18].
- **Conan 2**: recipes in Python (arbitrary code at build time), recipe and package *revisions*
  on top of versions, binaries per settings/options hash, lockfiles that pin versions and
  revisions [19].
- Lesson: most of their complexity is configuration explosion (compiler, runtime, flags, static
  or shared) and running a foreign build system. Kpp avoids both for Kpp code: one compiler,
  whole-program compilation from source, a configuration that is just `config` constants. It
  still meets that problem for C libraries, which is why the proposal below compiles C source
  with kppc's own clang rather than running anyone's CMake.

### Zig's package manager
- `build.zig.zon` lists dependencies by `url` and `hash`; **the hash is the identity** and the
  URL only one place to fetch it from; a URL whose content changes fails the hash. The hash
  covers the file tree after the manifest's `paths` filter, so it is independent of the archive
  format. `zig fetch --save URL` downloads, hashes and writes the entry [20].
- No central registry; anyone hosts archives anywhere. Discovery is left to the community.
- Lesson: content addressing makes mirrors, caches and vendoring trivially safe. Its weakness is
  that without a registry there is no search, no yanking, no names.

### npm and its incidents
- **left-pad (2016)**: an author unpublished 273 packages; an 11-line one broke builds across
  the ecosystem. npm's policy since: unpublish within 72 hours if nothing depends on it, later
  only with no dependents, under 300 downloads a week and one owner; an unpublished version can
  never be reused; otherwise deprecate [21][22].
- **Install scripts**: `preinstall`/`postinstall` run on install. The *Shai-Hulud* worm
  (September 2025, then a second wave in November) ran from them, stole npm and GitHub tokens
  and used them to publish infected versions of the victims' other packages, 500+ packages in
  days; npm answered with mandatory 2FA for local publishing [23][24].
- **Typosquatting and dependency confusion** (a public package with the name of a private one)
  are routine; huge transitive graphs make review impossible.
- Not npm but the same lesson: the **xz backdoor (2024)** hid its activation code in a build
  script that was in the release tarball and not in git, and its payload in "test data" files
  that were in git [25]. A release must be exactly what the repository holds, and nothing in it
  may run at build time.

### Lessons for Kpp

| Lesson | Where from | What Kpp does |
|---|---|---|
| Code that runs at install/build time is the main attack surface | npm scripts, build.rs, Conan recipes, xz's m4 | Nothing runs: libraries are source, compile-time code is interpreted and cannot call C or do I/O ([design.md](design.md) "Compile-time evaluation"). A C part is compiled by kppc, not built by the library's script |
| Never delete a published version; yank | left-pad, crates.io | Releases are immutable; yanking hides from new resolution only; admins remove malware |
| Content hash is identity | Zig, go.sum | SHA-256 of the file tree in the index and the lockfile |
| Reproducible versions without a solver | Go MVS | MVS, plus a lockfile for hashes |
| Everyone gets the same bytes | sum.golang.org | Later phase: a transparency log over the index |
| Typosquats are constant | crates.io 2025, npm | Name similarity check at first publish, `-`/`_` equal, new-account review |
| Stolen tokens publish malware | Shai-Hulud | Scoped tokens, 2FA, trusted publishing from CI; nothing to steal at install time anyway |
| The artifact records what it was built from | Factorio saves | The exe embeds its library list; `kppc sync --from game.exe` restores it |
| The platform/compiler is a dependency | Factorio `base`, `factorio_version` | A library states the minimum kppc version |
| One version per program | Factorio | Forced by the global scope; same as Factorio's one version per mod |

## Design for Kpp

### What a library is
- **A directory of Kpp source**, which becomes a top-level code package when added (its
  `package.kpp` names it in the tree: *JSON*, *Lua*, *Steamworks*). Subdirectories are child
  packages as usual. No prebuilt Kpp objects: the persistent compiler parses a library once and
  keeps it, and the front end handles the whole Factorio benchmark corpus in 0.32 s of wall time
  ([prototype-results.md](prototype-results.md)), so a few libraries are noise for it. Instantiations
  and inlining across the library boundary work as for the program's own code, which binary
  distribution would lose.
- **A manifest**, which is the project description of thread E with a `library` part: registry
  name, version, authors, SPDX license, summary, homepage/source URL, minimum kppc version,
  dependencies, C parts, supported targets, and the list of files that belong to the release
  (Zig's `paths`: tests and examples may be excluded). A library is a project, so it builds and
  tests on its own with the same tools.
- **A release** is an archive of those files; its identity is `name`, `version` and the SHA-256
  of the file tree (paths and contents, normalized line endings undecided: see decisions). The
  registry stores and serves archives; the archive format does not matter to the hash.
- **The registry name** is the top-level directory name in the cache and in the tree: lowercase
  letters, digits and `-`, 2 to 48 characters, `-` and `_` treated as equal for uniqueness
  (crates.io's rule), unique for ever (never reused after a removal).
- **Tests**: a library's tests (thread G's `kppc --test`) are part of its source but outside the
  release's `paths` by default, so they cost the user nothing. The registry can run them at
  publish time.
- **The standard library is not in the registry**: it ships with kppc and its version is kppc's,
  as Factorio's `base` mod is the game's.

### Names: the one global scope
The issue Kpp has and no other system above has: there is no per-library namespace, so two
libraries (or a library and the program) that both declare `Logger` cannot be used together.
Duplicate versions of one library are impossible for the same reason: `json` 1.x and 2.x both
declare `JsonValue`. What to do about it, from cheapest:

1. **Measure before changing anything** (proposed, phase 1). `kppc --exports` lists a release's
   top-level names (types, free functions, global variables, namespaces, C imports excluded); the
   registry stores the list with the release. `kppc add` checks it against the program and the
   other libraries and refuses with the same message the compiler would give, before editing
   anything: `json 1.4.0 declares 'Logger' (json/src/log.kpp:5), which logging 2.1.0 also
   declares (logging/Logger.kpp:3)`. The library page shows the names it claims and the known
   clashes with other libraries ("does not work with: ...").
2. **A convention for authors** (proposed, phase 1): public names are specific (`JsonValue`, not
   `Value`), and helpers are `static` or in an unnamed namespace (file-local, already exempt from
   the clash check) or in a namespace named after the library (`json::detail`). The registry warns
   at publish time about generic names on a short list (`Node`, `Item`, `Data`, `Logger`,
   `Config`, `Error`, `Result`...).
3. **`internal` declarations** (already floated in design.md as a later option): visible only in
   the declaring package and its children, exempt from the global check. This removes most
   clashes, since most of a library's names are helpers. Compiler work (thread A's resolver): a
   package-visibility flag on top-level declarations; lookup from outside skips them. Estimated
   1 to 2 weeks. Proposed for phase 2 if phase 1's measurements show clashes in practice.
4. **Renaming at the point of use** (not proposed): the project description says
   `logging.Logger as LogSink`. It works, but a name would then mean different things in
   different programs, which is the context dependency the code packages section rejected; a
   snippet from the library's documentation would not compile in the program. Listed so that the
   choice is explicit.

The registry also checks **clashes with the standard library** at publish time (a library
declaring `Pair` cannot be published for a kppc that has `Pair`), and the stdlib's new names are
checked against the registry's most-used libraries before a kppc release, so a compiler update
does not break programs silently. It cannot break them silently anyway (the clash is an error),
but it should not break them at all without notice.

### C dependencies
C is the only interop, so many useful libraries are a Kpp face over C code. Three kinds:
1. **System libraries** (`kernel32`, `user32`, `ws2_32`): named in the manifest, no files.
2. **C or C++ source compiled by kppc** (proposed default): the release includes the C/C++ files
   and headers (zlib, Lua, imgui with its C interface, re2 with its C interface: today's
   `programs/factorio/native`); kppc compiles them with its bundled clang, with flags from a short
   allowed list (defines, include directories, language standard, optimization follows the
   program's configuration), and caches the objects by content hash. No CMake, no scripts: the
   library's author writes the list of files, which is what vcpkg ports spend most of their
   effort reproducing. The headers' directory is added to `--cinclude` for the library's imports.
3. **Prebuilt binaries** (`.lib`, `.dll`, for SDKs without source: Steam, Logitech LED, FMOD):
   allowed per target (`windows-x64` only today), each file hashed, `.dll` copied next to the
   exe. The release and every program using it are marked **binary** in the UI and the lockfile;
   first publication of a binary goes through human review.

**C names are global too**: two libraries each bundling zlib would clash (or worse, link one copy
over the other). Rule: a C library is a registry library of its own (`zlib`, `lua`), and the Kpp
faces depend on it, so a program has one zlib. Its version is the upstream version with a
packaging suffix if needed (decision 6).

### Versions, resolution and the lockfile
- **Versions**: `major.minor.patch`, as Factorio and semver. A new major is a new set of names in
  practice (the global scope), so it is published as a new version of the same library and the
  program moves to it as a whole.
- **Requirements**: a dependency states a minimum (`json >= 1.4`), implicitly `< 2.0` (cargo's
  caret); Factorio's `!` (incompatible with) is kept, for clashes and known breakage; `?`
  optional is a later phase, since the program needs a way to know at compile time whether the
  library is there (a generated `config::has_json` constant would do it, as `--config` generates
  constants today). The library states its minimum kppc version, as Factorio's
  `factorio_version`.
- **Resolution: minimal version selection** (Go): each library gets the highest of the minimums
  required anywhere in the graph. One version per library is what Kpp needs anyway; MVS needs no
  solver, is deterministic, and a new release upstream changes nothing until someone runs
  `kppc update`. A conflict (a minimum `>= 2.0` against a `< 2.0`, or a `!`) is an error that
  names the chain of requirements.
- **The lockfile**, `build.lock` (build.md), committed to source control: each library
  with exact version, tree hash and the registry it came from. With MVS it is not needed to pick
  versions; it is needed to detect a changed archive and to pin a yanked version. kppc refuses to
  build when a file in the cache does not match the lockfile.
- **The built program remembers**: kppc embeds the lockfile's content (names, versions, hashes) in
  the exe as a read-only section, and `kppc sync --from program.exe` restores the exact libraries
  a binary was built with: Factorio's *Sync mods with save*, for reproducing a crash report.
- **Yanking**: an author marks a version yanked with a reason; new resolution skips it, existing
  lockfiles still get it, `kppc` prints the reason. Deletion only by registry admins for
  malware or legal reasons, recorded publicly; a removed version's name and number are never
  reused.

### The commands and the project description
All of this is a part of kppc (one tool; the persistent server already holds the program model):
- `kppc add json` (or `json@1.4`): look up the index, resolve with MVS, check name clashes,
  download into the cache, verify hashes, **then** edit the project description (add the
  requirement) and the lockfile. Also `--path DIR` and `--git URL#commit` for unpublished
  libraries.
- `kppc remove json`, `kppc update [json]` (raise minimums to the newest non-yanked within the
  major, show what changed, re-check clashes), `kppc sync` (make the cache match the lockfile;
  what a fresh clone runs, and what the build runs implicitly), `kppc vendor` (copy the libraries
  into the project tree), `kppc publish` (phase 2), `kppc search`.
- **Cache**: `%LOCALAPPDATA%\kpp\libraries\<name>-<version>-<hash8>\`, read-only, shared by all
  projects; the persistent compiler keeps parsed libraries keyed by the hash, so switching
  projects that use the same version does not re-parse it.

#### The project description (thread E, decided 2026-10-31)
[build.md](build.md) settled the form: `project.use("json")` in build.kpp adds a library (the
next build resolves, checks clashes and writes build.lock); `use("json", "1.4")` states a minimum;
`git(...)` and `local(...)` are the other sources; `libraries/<name>` in the project overrides the
registry (forks); a library is a project whose build.kpp says `project.library(...)` and may not
declare build steps. So `kppc add` above is `kpp add`, which only inserts the `use` line.
`project.library(...)` with its version, license, minimum kpp, source and test roots, `use`
from `libraries/` and local paths, and `kpp --test` work since 2026-10-31 (build.md, "Kpp
libraries"). What the registry still needs from E's format: the rest of the manifest fields listed
in [What a library is](#what-a-library-is) (authors, summary, homepage, release paths, C parts),
and build.lock recording, per library, name, version, the tree hash (`sha256-tree:...`, defined by
[kppnet/registry/tree_hash.py](../kppnet/registry/tree_hash.py)) and the registry it came from.

### The service
- **Phase 1 needs no server program**: the index is a git repository (`registry-index.git` on
  kpp.net, mirrored to GitHub; one file per library listing its releases: version, tree hash,
  archive path, dependencies, exported names, yanked), as crates.io's index was a git repository
  for years. Publishing: a library's repository tags a version; a pull request on the index (on
  GitHub) proposes the release; GitHub Actions builds and tests it with the current kpp and checks
  the manifest, name rules and clashes; a maintainer merges locally and runs `kpp publish`, which
  packs it (the tree hash of [kppnet/registry/tree_hash.py](../kppnet/registry/tree_hash.py)),
  writes the index file and the library's page, and uploads them to kpp.net write-once
  ([kppnet/registry/layout.md](../kppnet/registry/layout.md)). kpp reads only kpp.net's static
  files.
- **Phase 2, the service in Kpp** (dogfooding): an HTTP API (index, search, publish, yank,
  tokens), web pages (library page with readme, versions, dependencies, exported names, license,
  "binary" mark, download counts, clash list), a small database (SQLite through C interop), and
  the archive store. It needs from the standard library what it lacks today: sockets, an HTTP
  server, JSON, a SQLite face, time; TLS stays outside (a reverse proxy such as Caddy or
  Cloudflare in front, or Windows' http.sys through C), which keeps the Kpp program small. It
  exercises threads, I/O, strings and containers under real load, which the game does not
  (the game exercises everything else). The index stays a set of static files the service
  writes, so kppc's client code does not change and a CDN serves the reads.
- **Hosting and accounts**: decided, next section. Ownership per library: owners and co-owners
  (the forge's repository permissions in phase 1), transferable; scoped API tokens; 2FA required
  to publish.

### Hosting on kpp.net (decided 2026-10-31, revised the same day)
**The decisions** (kovarex, 2026-10-31):
- Official Kpp libraries, and later kpp itself, are in repositories owned and hosted on kpp.net.
  GitHub may be used only as a mirror: nothing may depend on it existentially, and moving to
  another host must stay possible. His words were "I prefer to build A right away", option A being
  a self-hosted forge.
- **Revised the same day:** kpp.net is his, at NearlyFreeSpeech.NET (NFS), and the site is hosted
  there too. The kpp.net website and the library provider are written in Kpp: "I find it nicely
  poetic, and even practical", and "one of the first libraries we should host on the site, would
  be those needed to run actual web page by kpp."
- **Scope** (2026-11-01): "kpp.net and its repositories will first mainly provide the compiler
  (sources and releases) as well as the libraries."
- **Factorio** (2026-11-01): "I would like to make the factorio sources to be available one day,
  as an example of program in kpp, but it doesn't have to be in the start." So factorio-in-kpp
  stays private and is not on kpp.net. Publishing it later, as an example program, is Wube's
  decision when the time comes. Until then it has no license, and nothing in kpp.net's plan
  depends on it.

The plan that follows from these:
- Canonical bare git repositories live on kpp.net, and pushes go there.
- GitHub is the mirror where pull requests and issues happen. Maintainers merge locally and push
  to kpp.net, which mirrors to GitHub.
- CI stays on GitHub Actions for now.
- A forge on kpp.net comes later, only if NFS can run one (below).

The VM-based kit from earlier that day (Forgejo, Caddy, a Windows runner) is dropped; only the
registry's store layout and its reference hash survive, in [kppnet/registry/](../kppnet/registry).

#### What NFS offers
Read from NFS's own pages on 2026-10-31. "Unconfirmed" means its pages do not say; kovarex can
check those over SSH or by asking NFS support.

| | What NFS says | Source |
|---|---|---|
| OS, hardware | FreeBSD on 64-bit AMD Epyc servers with ECC RAM, files on NVMe; content served mostly from New York. FreeBSD version: unconfirmed (`uname -a` over SSH; design.md's triple assumes 14). | [26][27] |
| Software sets | A site's "realm" is its whole OS, language and application set. New stable realms come about every three months; the recommended "late" setting upgrades about every six months. | [28] |
| Scope | "web sites only": only HTTP(S) services are reachable from outside. Supporting services (databases, caches) are allowed to a limited extent. | [29][30] |
| Own programs | NFS provides C and C++ compilers, as-is, for compiling one's own CGI applications. Which compilers is unconfirmed; FreeBSD's base `cc` is clang. Running an uploaded, statically linked FreeBSD binary is unconfirmed but follows from "compile your own". | [31] |
| CGI | Any executable with a `.cgi` extension, marked executable with a `#!` line, in any directory. CGI processes and single SSH commands get 2 to 5 minutes of CPU, and are killed for running too long or for behaving like daemons. | [32][33] |
| Daemons | A long-running process is added in the site panel (tag, command line, working directory). Web servers run as user `web`, support servers as `me`. NFS starts it within about a minute. Restart after a crash: unconfirmed. The server type must allow daemons ("custom", or the 2023 "Kitchen Sink" type with Apache, PHP, CGI, daemons and proxies). | [34][35] |
| Proxies | A proxy maps a base path of the site to a daemon's TCP port (1024 to 65535). The 2014 posts list HTTP, SCGI and FastCGI. The listen address is unconfirmed, presumably localhost. | [34][36] |
| TLS | Automatic Let's Encrypt certificates for every site, custom processes and proxies included, with no setup or renewal. NFS's front end terminates TLS, so a daemon speaks plain HTTP; this is implied, not stated. HTTP/2 and WebSockets: unconfirmed for today. | [37][38] |
| SSH, SFTP | `ssh <user>_<site>@ssh.<host>.nearlyfreespeech.net` into a shell. Ed25519 keys in the member's keychain. **Automated access must use a key and is allowed only for backups (at most daily) and uploads (under one file per five minutes on average).** | [39][40][41] |
| Git | Not in NFS's docs. Members' how-tos keep a bare repository under `/home/private` and push to it over SSH with a post-receive hook, so git is installed. Unconfirmed officially. | [42] |
| Databases | MariaDB processes (InnoDB) reachable only from NFS sites: the first is free, each extra $0.01 a day. SQLite bindings exist for the supported languages. PostgreSQL and others can run under the "custom" type. | [43] |
| Limits | No overall per-site CPU limit; RAM and CPU are billed. Memory limits per process: unconfirmed. Outbound connections from sites: unconfirmed. | [33] |
| Prices | Membership up to $5 a month. A production site is $0.05 a day with 10 GiB of bandwidth a day included; extra bandwidth is currently not billed. Storage is $1.00 per GiB-month. Resources are $0.01 per 44.64 RAU, one RAU being 1 GB of RAM for one minute or the CPU equivalent. DNS is $0.01 per 9 days when the domain is registered and hosted there. | [44] |

**Cost of the plan**, estimated (assumptions in brackets):
- the site: $0.05 a day, $1.50 a month;
- a Kpp daemon using 30 MB all day: 43 RAU a day, about $0.30 a month [resident memory billed in
  full];
- 2 GiB of archives and site files: $2 a month;
- membership: up to $5 a month.

That is about $5 to $9 a month. Bandwidth is free for now: 10 GiB a day is about 200,000
downloads of a 50 KB library.

#### Git: canonical on kpp.net, pull requests on GitHub
- **The repositories, in this order** (the scope above: the compiler first, then the libraries):
  1. `kpp.git`: the compiler, the standard library, projectlib, the tools and the docs. Its
     releases (CI's packages, `publish.ps1`) go to `registry/kpp/releases/` with `latest.json`
     (see "CI").
  2. The official libraries. **Recommended: they stay in `kpp.git` under `libraries/`** for now,
     and a registry release is cut from that subdirectory (the tree hash covers only the
     library's directory; tags such as `json-v0.1.0`). The reasons:
     - the language and the standard library still change weekly;
     - `run_tests.cmd` builds and tests every library with every compiler change, so a change
       that breaks a library is caught in the same commit;
     - a fix that touches the compiler and a library lands as one commit.

     A library moves to its own repository, `libraries/<name>.git`, once its API settles or it
     gets maintainers of its own. The registry does not care where a release was cut, since it
     records the tree hash and the commit. Third-party libraries are always their own
     repositories, wherever their authors host them.
  3. `registry-index.git`: the index's history, once the first library is published.
  4. The website's sources: `kppsite` and its pages, in `kpp.git` (under `kppnet/site/`) until
     they need a repository of their own.

  Not on kpp.net: factorio-in-kpp (above).
- **Where they live:** bare repositories in the site's private area
  (`/home/private/git/kpp.git` ...). Clone URL:
  `<user>_kpp@ssh.<host>.nearlyfreespeech.net:/home/private/git/kpp.git`. Every push goes there
  first.
- **Mirroring to GitHub**, two ways depending on what NFS allows:
  1. **Preferred:** a `post-receive` hook on kpp.net pushes to GitHub with a deploy key, if NFS
     allows outbound SSH or HTTPS from sites (unconfirmed). Clients then push to kpp.net only, and
     a GitHub ruleset lets only that deploy key update branches. GitHub's merge button cannot be
     turned off directly; the ruleset is what makes it fail, so a pull request is merged only by a
     push that went through kpp.net.
  2. **Otherwise:** every clone's `origin` has two push URLs, kpp.net first and GitHub second
     (`git remote set-url --add --push origin ...`). One `git push` updates both. kpp.net stays the
     original because it is pushed first and holds what GitHub has; a push that fails at GitHub is
     repeated. GitHub's merge button is left alone by convention.
- **The workflow:**
  1. A contributor forks on GitHub and opens a pull request.
  2. Review and issues happen on GitHub.
  3. A maintainer fetches the branch, merges or rebases locally, and pushes to kpp.net.
  4. The mirror updates GitHub, which shows the pull request as merged because its commits are on
     master.

  Nothing on GitHub is needed to build: the code is in every clone and on kpp.net, and pull
  request history is the only thing lost if GitHub goes.
- **The automation rule matters:** the threads push many times a day with keys, and NFS's policy
  allows unattended SSH only for daily backups and occasional uploads. Whether frequent `git push`
  from agent sessions counts is for kovarex to ask NFS (decision 12). If it is not allowed, the
  threads keep pushing to GitHub, and a once-a-day job on kpp.net, or kovarex by hand, pulls
  GitHub into kpp.net. Then kpp.net is a daily copy, not the original, until a forge or another
  host takes the pushes.
- **Backups:** the git data is in every clone, on GitHub and on PC-PRACOVNA. The registry's files
  and the site's database are pulled once a day over SSH, which NFS allows for backups, to
  PC-PRACOVNA and another place.

#### CI
CI stays on GitHub Actions, triggered by the mirror. ci.yml is already portable to Forgejo
Actions (H, [build.md](build.md) "GitHub Actions and Forgejo Actions"), so moving it later
costs little. NFS cannot be a CI runner: it has no Windows, it is web-only, and it kills
long-running non-web processes. If CI ever leaves GitHub, it needs a self-hosted Windows runner
(PC-PRACOVNA or a cloud VM) under a forge somewhere. H's open points (vars in `runs-on`,
`concurrency`, the job token writing releases) matter only then. Two changes are useful now:
- the SDL3 zip ci.yml downloads from github.com moves to kpp.net's mirror directory, checked by
  its SHA-256;
- **Publishing kpp's own packages** (agreed with H through the coordinator):
  `tools/release/publish.ps1` (H, a52cb2a8, kpp.net path 9ba37cc1) keeps its GitHub/Forgejo
  release path and has a kpp.net path. That path copies the files to
  `registry/kpp/releases/<version>/` with OpenSSH's ssh and scp (Windows runners have no rsync),
  keeping any file already there. It writes `registry/kpp/latest.json` (version, commit, zip,
  SHA-256) last, and renames it into place so a reader never sees half a file. With `-Mirror`, it
  puts third-party files under `registry/mirror/<sha256>/`. The key is the CI secret
  `KPP_NET_SSH_KEY`, authorized in NFS's keychain. kpp.net is the copy installers and factorio-in-kpp's CI read; the
  GitHub release stays as a convenience. A green master run uploads three files, so even 22 runs
  a day stay inside NFS's limit of one uploaded file per five minutes on average (288 a day).

**Platforms.** kovarex wants Windows, macOS and Linux as the minimum
([design.md](design.md) "Targets and hosts"). GitHub Actions has hosted runners for all three,
Linux and macOS on both x86-64 and arm64, so staying on it covers the matrix with no machines of
our own. GitHub has no FreeBSD runner: FreeBSD, kpp.net's target, runs in a FreeBSD VM inside a
Linux runner (QEMU, for example the vmactions/freebsd-vm action), or on a FreeBSD VM thread B
sets up (Q3 there). If CI ever moves to a forge of our own, all of these become self-hosted
runners: Windows (VS 2022 with clang), Linux, a Mac, and a FreeBSD VM.

#### A forge on NFS?
Forgejo is a single Go program and could in principle run as an NFS daemon behind a proxy, with
SQLite. The memory cost is small: 250 MB resident is about 360 RAU a day, around $2.50 a month.
The obstacles:
- Forgejo publishes no FreeBSD binary. It needs FreeBSD's port or a build with cgo for SQLite on
  NFS, where Go is not confirmed.
- Git over SSH would go through NFS's own SSH and its automation rule, not Forgejo's SSH server,
  so contributors would use HTTPS.
- Its CI needs runners elsewhere anyway.
- NFS's FAQ wants daemons to be web servers, which Forgejo is, so a trial is allowed.

Verdict: **feasible enough to try later, not needed now.** The plan above works without it.
Trying means building Forgejo on NFS and running it for a week as a non-production site ($0.01 a
day) to watch the RAU bill. The alternative forge is one written in Kpp, a far larger job than
the registry.

#### Where NFS is not enough
The earlier VM kit (Forgejo, Caddy, restic backups, a Windows runner: kppnet/ at commit
bbf5cf61) is kept only as the answer to these cases:
1. **NFS forbids frequent automated pushes** (decision 12, question 1). Then kpp.net cannot take
   the threads' many pushes a day, so it cannot be where they push. Either GitHub stays the push
   target, with kpp.net pulling once a day as a copy, or the repositories move to a forge on a
   rented VM (the kit), with NFS keeping the website and the registry.
2. **Pull requests and issues off GitHub.** NFS can serve bare repositories but has no review UI
   unless Forgejo runs there ("A forge on NFS?"). If GitHub must go entirely, the kit is that
   forge.
3. **CI off GitHub.** NFS can never be a runner. The runners are self-hosted machines whatever
   hosts the forge.

Everything else (the website, the registry, the archive store, the canonical repositories if
pushes are allowed, and backups) fits NFS.

#### The website and the library provider in Kpp
**The blocker:** kppc produces Windows x64 programs only. The FreeBSD target is thread B's
([design.md](design.md) "Targets and hosts": `--target freebsd`, `x86_64-unknown-freebsd14`,
ELF, ld.lld). So the plan has a stage that needs no FreeBSD at all.

**Stage 1: static, built by Kpp on Windows (possible now).** NFS's Apache serves files. The
website and the registry's public part can be all files, written by Kpp programs that run on
PC-PRACOVNA:
- `kppsite`, a static site generator in Kpp. It turns the pages' sources (Markdown, the docs in
  this repository, the generated library pages) into HTML, and uploads by rsync over SSH, which
  is allowed as an upload.
- The registry's index and archives are static files too
  ([kppnet/registry/layout.md](../kppnet/registry/layout.md)). A publish is run by a maintainer
  with a Kpp tool (`kpp publish` in its first form). It packs, hashes, checks names and clashes,
  writes the index file and the library's page, and uploads.
- Search runs in the browser over a JSON list of libraries, which is small for years.

This stage already needs the first libraries in Kpp: JSON, SHA-256, Markdown, HTML templating,
zip with deflate, and file system walking.

**Stage 2: dynamic, as CGI (once the FreeBSD target runs a program).** CGI is the smallest step
on a new target. The program reads the environment and stdin, writes the response to stdout,
and exits: no sockets and no threads. Apache in front handles TLS, HTTP and process limits.
- `registry.cgi` takes publishes over HTTPS from GitHub Actions or `kpp publish`, so uploads stop
  going over SSH and its automation limit. It also handles yanking and API tokens.
- `site.cgi` serves whatever cannot be precomputed: search on the server, download counts,
  per-version pages built on demand.

State lives in files plus SQLite (the SQLite amalgamation is one C file, compiled by kppc).
SQLite is cheaper than a MariaDB process and needs no server. One unconfirmed point: whether
SQLite's file locking is safe on NFS's file storage. If it is not, a MariaDB process is free.

**Stage 3: a daemon (once the target has sockets and threads).** A Kpp HTTP/1.1 server runs as
an NFS daemon (user `web`), and NFS's proxy forwards to its port and terminates TLS. It holds
the index in memory, keeps connections alive and needs no process per request. It is what makes
"a web server in Kpp" true, and the libraries it needs are the ones others will want.

**Libraries, in the order the stages need them.** Each one is a registry library, the first ones
hosted. Each declares specific top-level names, because of the one global scope.
1. `json`: parse and write, typed reflection-driven (de)serialization (section 8's compile-time
   iteration).
2. `sha256`, with HMAC-SHA-256 for tokens and a constant-time compare. Hashes are needed by the
   registry itself (the tree hash, kppnet/registry/tree_hash.py).
3. `zlib`, the C library compiled from source by kppc, and a `zip` reader and writer over it, for
   the archives.
4. `markdown`: CommonMark to HTML, for README and documentation pages.
5. `html`: templates that escape by default. A template is parsed at compile time by the
   interpreter, so a broken template is a compile error.
6. `url`: percent-encoding, query strings and paths.
7. `cgi`: Request and Response over the CGI environment and standard streams.
8. `sqlite`: the C amalgamation and a Kpp face (statements, binding, rows into structs).
9. `http`: an HTTP/1.1 parser and server, a router and a static file handler (stage 3).
10. `base64`, and `time` formatting (RFC 3339, HTTP dates), if the standard library does not have
    them.

TLS is not needed: NFS terminates it. An outbound HTTPS client, for example to sign in with
GitHub, would need TLS from C (BearSSL or mbedTLS) and NFS allowing outbound connections, so
accounts use API tokens until then.

**What the FreeBSD target must provide for this** (sent to thread B through the coordinator):
- **Stage 1:** nothing; it runs on Windows.
- **Stage 2 (CGI):**
  - `main` with UTF-8 `argv`, and `getenv`;
  - stdin and stdout as raw bytes, and the exit status;
  - files: open, read, write, close, fstat, mkdir, rename (atomic, for write-once), unlink, fsync,
    and directory listing;
  - `clock_gettime` (REALTIME and MONOTONIC) and `gmtime_r`;
  - `arc4random_buf` for tokens;
  - errno through `__error()`;
  - the C import against FreeBSD's headers, so the zlib and SQLite C sources compile with the
    target's clang;
  - exceptions unwinding on ELF (section 2 of Targets and hosts);
  - a statically linked executable, so it survives NFS's realm upgrades. Otherwise, dynamic
    linking against the realm's libc, and a rebuild on each upgrade.
- **Stage 3 (daemon):** everything in stage 2, plus:
  - sockets: socket, setsockopt (SO_REUSEADDR), bind and listen on 127.0.0.1 or the address NFS
    expects, accept4, recv, send, shutdown, close;
  - `kqueue` or `poll`;
  - threads: pthread create and join, mutex, condition variable, and the library's atomics;
  - signals: SIGTERM for a clean stop, SIGPIPE ignored.
- **Build and run:**
  - a cross build from Windows to a FreeBSD sysroot (base.txz's headers, crt objects and libc.a),
    linked by VS's `ld.lld`;
  - or, as a stopgap, kppc's IR copied to NFS and compiled with NFS's own clang;
  - and a FreeBSD VM for tests, since NFS is production (design.md's Q3).

### Security and moderation
- **Nothing runs on install or build** (the strongest point; keep it as a rule): a library
  cannot gain a build script, and compile-time evaluation stays without I/O and without C calls.
  If a future feature lets compile-time code read files (embedding assets), it must be limited to
  the library's own directory.
- **What remains**: a malicious library's code runs when the program runs; a prebuilt binary is
  opaque. Mitigations: source-only by default; the "binary" mark everywhere; `kppc update` shows
  the diff of a library between the locked and the new version (it is all source, so the diff is
  readable, unlike npm's minified bundles); releases are built from a tagged commit by the
  registry's CI or trusted publishing, so the archive equals the repository (xz).
- **Names**: first publication of a name close to an existing one (edit distance 1 to 2 after
  normalizing `-`/`_`, digits for letters) waits for review; reserved names (stdlib package names,
  `kpp`, `std`, `factorio`, `wube`); no squatting (a name with no release for a year can be
  reclaimed on request).
- **Integrity**: SHA-256 in the index and the lockfile from phase 1 (trust on first use plus
  HTTPS); phase 3: a transparency log over the index as sum.golang.org, or signed index files
  (TUF-style roles) so a compromised mirror or CDN cannot serve other bytes.
- **Publishing**: by a maintainer's `kpp publish` in phase 1; from stage 2 on, through the
  registry's CGI endpoint with tokens scoped to one library and one action, later short-lived
  per-job tokens as crates.io's trusted publishing; 2FA for maintainers. A security advisory list and `kppc audit` (phase 3).
- **Moderation**: reports from users and automated checks; admins remove malware and ban
  accounts, publicly logged. Phase 1's pull-request review is moderation by construction, which
  is fine while the registry is small.

### Mirrors, offline and private registries
- Content addressing makes any copy trustworthy: the cache, a mirror, a vendored directory, a
  USB stick. `kppc sync --offline` uses only the cache; the build never touches the network
  unless the lockfile names something not in the cache.
- **Mirrors**: the index lists archive URLs, and kppc tries a configured mirror list first; a
  mirror is a static copy of the index and the archives (a `rsync`/`git clone` plus a file
  server).
- **Private registries** (a company, Wube): an index in a private git repository or a directory
  on a share, named in the user's or project's configuration; a library name resolves in the
  first registry that has it, with a project able to pin a library to a registry, so a public
  library with the name of a private one cannot take its place (dependency confusion).

### IDE and Visual Studio
For threads F and E; proposals for their documents, not decisions.
- **IDE (F)**: libraries appear as top-level packages in the package tree with their version,
  license and a "binary" badge; go to definition opens the library's (read-only) source; a
  *Libraries* view searches the registry, shows a library's page (readme, exported names, clashes
  with the current program, dependencies, binary parts), and adds, updates or removes with the
  same kppc commands; an available update or a yanked locked version shows as a notice; a clash
  diagnostic offers "show the other declaration".
- **Visual Studio (E)**: a *Libraries* node under the project listing the locked libraries; menu
  commands for add (a search box), update and remove that run kppc and reload the project
  description. A NuGet-style browser is not worth building twice; the IDE is where the rich view
  lives.

### Licensing (decided 2026-11-01)
**The decision** (kovarex): "Ok, so the apache seems to be most programmer friendly. Basically I
want other programmers/Ai coders to be able to freely use everything as much as they want, and
also be protected from someone denying me from continuing providing the service."

**What is under which license.**
- **Kpp:** the compiler, the standard library, projectlib, the tools, kppnet, the documentation and
  the official libraries in `libraries/` are under **Apache-2.0 WITH LLVM-exception**, as LLVM and
  clang are. The canonical text is [LICENSE](../LICENSE): LLVM's LICENSE.TXT [45], whose Apache
  part is word for word SPDX's Apache-2.0 [46].
  - The exception's first paragraph is why this variant was chosen. A program compiled with Kpp
    contains compiled parts of the standard library, and the exception lets those be distributed
    without Apache's conditions 4(a), 4(b) and 4(d). So users owe no copy of the license, no
    notices and no attribution for the programs they build.
  - Its second paragraph removes Apache 2.0's known incompatibility with GPLv2 for combined
    software [47].
- **No NOTICE file**, as LLVM has none. Under section 4(d) every redistributor would have to carry
  a NOTICE file's text.
- **Contributions** come in under the same license, certified per commit by a `Signed-off-by`
  line (the Developer Certificate of Origin [48], [CONTRIBUTING.md](../CONTRIBUTING.md)). There is
  no copyright assignment and no contributor license agreement. The consequence: contributors keep
  their copyright, so relicensing Kpp later (to anything, proprietary included) would need every
  contributor's consent. Apache 2.0 already allows every use, so this blocks nothing kovarex
  asked for.
- **Third-party code keeps its license and notices:** the JSONTestSuite data in the json
  library's tests (MIT, with its license file next to it), and converted C libraries such as
  zlib, SDL, curl, ogg, vorbis, opus, re2, imgui and stb.
- **factorio-in-kpp** is Factorio's source, Wube's, and gets no open license. It stays private
  and off kpp.net. kovarex would like it available one day as an example program in Kpp, which is
  Wube's decision when the time comes ("Hosting on kpp.net"). In this repository,
  `programs/factorio/` (the overrides derived from the game's source, and the interfaces to its
  libraries) is listed in LICENSE as not covered. Which of its files are Kpp's own work (scripts,
  tests), and so could be Apache, is kovarex's call (decision 13).

**What the registry requires of a hosted library.**
- Every release states an SPDX license expression in its build.kpp (`license`), and its archive
  contains the license texts (`LICENSE`, `COPYING` or a `LICENSES/` directory). Publishing checks
  the expression against the SPDX list and refuses a release without the text.
- **Official libraries** (`libraries/`, maintained by the project) are Apache-2.0 WITH
  LLVM-exception. A Kpp face over a third-party C library is ours. The bundled upstream code keeps
  its license, so such a library's expression names both, e.g.
  `Zlib AND Apache-2.0 WITH LLVM-exception`.
- **Other libraries** may use any OSI-approved license, or one equivalent to the public domain
  (CC0, Unlicense). The library page shows the license, and `kpp add` names it, with a notice when
  it is copyleft (GPL, LGPL, MPL), so nobody adopts one by accident.
- **Converted third-party libraries** carry upstream's license files and every notice verbatim,
  and the release names the upstream version and URL. Their notices, unlike the standard
  library's, must ship with programs that use them (zlib's does not have to, MIT's and BSD's do).
  So every build writes `build/<configuration>/licenses.txt`: the license texts of every library
  in the program with a notice requirement. That is a phase 0 task.

**What the license protects, and what it does not ("denying me from continuing").**
- **It does:**
  - Section 2 grants every recipient a perpetual, irrevocable copyright license. Once Kpp is
    published, nobody can take away kovarex's or anyone's right to use, change and distribute the
    published code, and kovarex keeps his own rights as an author too.
  - Section 3 grants a patent license from every contributor for their contributions. Anyone who
    sues claiming that Kpp infringes a patent loses the patent license they got from Kpp. This
    protects against a contributor, or someone building on Kpp, attacking it with patents.
  - The DCO makes every contribution certified as licensable, so no contributor can later claim
    their code was not licensed.
- **It does not:**
  - Section 6 explicitly grants no rights to trademarks. Anyone may fork Kpp, but the license
    neither stops nor allows them to call their fork "Kpp".
  - The name is protected only by trademark law, which is strongest with a registration.
  - kpp.net is protected by its registration (kovarex's, at NFS, paid until 2030-01-03), and the
    service by his accounts at NFS and GitHub.
  - The license also does not stop someone from registering "Kpp" as a trademark first, in some
    country and class, and contesting his use of the name there.

**Steps kovarex might take** (none taken; listed for him to decide):
1. **A trademark clearance search** for "KPP" and "Kpp" in Nice classes 9 (software) and 42
   (software services, hosting). Search tools are EUIPO's TMview, the USPTO and WIPO's Global
   Brand Database. KPP is a common abbreviation, so earlier marks in those classes are possible.
   A trademark attorney's search is the reliable version.
2. **Register the word mark**, and later a logo. The owner could be him personally, Wube, or a
   foundation if Kpp should outlive any one owner.
   - EU trade mark (EUIPO): €850 online for one class, €50 for the second, €150 for each further
     class [49].
   - US (USPTO): $350 per class since 2025-01-18 [50].
   - Other countries through WIPO's Madrid system.
3. **A short trademark policy** on kpp.net. Forks and products may say "for Kpp", "based on Kpp"
   or "compatible with Kpp", but may not be called Kpp or look like the official site; the stdlib
   and official libraries keep their names in forks. This is what Rust, Python and others
   publish.
4. **Domain hygiene:**
   - kpp.net on auto-renew, with the registrar lock and two-factor login;
   - possibly defensive registrations of the same name under other top-level domains
     (availability not checked);
   - the GitHub organization or user name the mirror lives under kept in his account.
5. **The copyright line** in LICENSE and file headers: "the Kpp authors" (the convention with a
   DCO), or his name for the code he and the threads write. Today no file carries a copyright
   line, which is allowed; the license applies either way.

## Phases and effort
Estimates are in weeks of one thread, assuming thread E's project description exists.

| Phase | Contents | Effort |
|---|---|---|
| 0. Local libraries | `--path`/`--git` dependencies in the project description, cache, lockfile with SHA-256, `kppc add/remove/sync/vendor`, `kppc --exports` and the clash pre-check, C source parts compiled by kppc (moving `programs/factorio/native`'s build into this is the test) | 2 to 3 |
| H. Hosting | kpp.net on NFS: bare repositories, the mirror to GitHub, the static store, daily backups ([kppnet/RUNBOOK.md](../kppnet/RUNBOOK.md)) | half a day of kovarex's time |
| S1. Static site and registry in Kpp | `kppsite` and `kpp publish` running on Windows, with the libraries json, sha256, zlib/zip, markdown, html | 4 to 6 |
| S2. CGI on FreeBSD | `registry.cgi`, `site.cgi`, libraries url, cgi, sqlite; needs thread B's FreeBSD target (CGI subset) | 3 to 4 after the target |
| S3. Kpp HTTP daemon | the `http` library and the site as an NFS daemon; needs sockets and threads on FreeBSD | 3 to 5 after the target |
| 1. Registry without a service | index repository and archives on kpp.net, CI checks on publish pull requests, MVS, `update`, yanking, `search` over the index, embedded library list in the exe, first libraries: zlib, lua, imgui, json, a Kpp face over sockets | 2 to 3, plus porting the first libraries |
| 2. The service in Kpp | stdlib networking/HTTP/JSON/SQLite face first (3 to 5 weeks, useful beyond this), then API, accounts, web pages, publish API, tokens, 2FA, download counts | 6 to 10 |
| 3. Hardening | trusted publishing, transparency log or signed index, advisories and `kppc audit`, typosquat review queue, `internal` declarations if clashes are measured | 4 to 6 |
| 4. Tools | IDE Libraries view, VS commands, generated documentation pages per library from reflection, more targets for binary parts, private registry setup | with threads E and F |

The minimal first version is phases 0, 1 and S1 on the hosting of phase H: about 10 weeks, no
server program of its own,
usable by Wube and early users, and it proves the parts that are Kpp-specific (clash pre-check,
C source parts, MVS with one version) before any server code is written.

## Risks
- **Name clashes become the common failure.** If libraries routinely declare the same names,
  the one global scope is a real cost for users. Phase 1 measures it (the exported-name index
  over all published releases); `internal` is the fix ready to take.
- **Every library is compiled with every program.** Fine for the front end; code generation and
  LLVM dominate builds, so what matters is how much of a library kppc emits for a program that
  uses a little of it; a big C part (Lua, imgui) compiles once per configuration and is cached.
  Measure on the first libraries.
- **C libraries are most of the value and most of the risk.** Compiling their source with fixed
  flags will not cover every C library (configure-time generated headers, assembly); some will
  need a pre-generated config header committed by the packager.
- **The service in Kpp depends on stdlib work** (networking, HTTP, TLS strategy) that does not
  exist yet; doing it before the game runs fully competes with the headline. Phase 1 does not
  depend on it.
- **Moderation load** grows with users; phase 1's pull-request model does not scale past a few
  hundred libraries, which is when phase 2 is needed anyway.
- **Language not stable yet**: libraries published now will break with language changes (struct
  and class, code packages are still proposals). The minimum-kppc-version field and a kppc
  version in the lockfile make the breakage visible; the registry can rebuild all releases with a
  new kppc before it ships (crater-style) to see what breaks.

## Decisions for kovarex
1. **Is a registry wanted now**, or after the language settles? Proposed: phase 0 now (it also
   tidies `programs/factorio/native`), phase 1 when there are three or more libraries worth
   sharing.
2. **Source only, with C source compiled by kppc and prebuilt binaries allowed but flagged?**
   Proposed: yes.
3. **Name clashes**: the clash pre-check and naming convention now, `internal` declarations
   later if measured (proposed), or `internal` now, or renaming at the point of use (not
   proposed)?
4. **Resolution**: minimal version selection (proposed) or newest-compatible as cargo?
5. **No code runs at build or install time, as a permanent rule** (no build scripts, compile-time
   evaluation without I/O)? Proposed: yes.
6. **C libraries as their own registry entries** (one zlib per program), versioned with upstream's
   number? Proposed: yes.
7. **The service written in Kpp** (dogfooding, needs stdlib networking) or a conventional stack
   with Kpp only in the client? Proposed: Kpp, from phase 2; phase 1 needs no service.
8. **Hosting**: decided 2026-10-31 ([Hosting on kpp.net](#hosting-on-kppnet-decided-2026-10-31-revised-the-same-day)): kpp.net at
   NearlyFreeSpeech.NET; canonical bare git repositories there, GitHub as the mirror for pull
   requests and issues; the website and the registry written in Kpp; a forge only later and only
   if NFS runs it.
9. **Accounts**: none of kpp.net's own in stages S1 and S2 (maintainers' SSH keys, API tokens);
   GitHub sign-in or factorio.com accounts are a question for the dynamic stages.
10. **Hash normalization**: hash the files as stored (proposed; the repository's own
    `.gitattributes` fixes line endings) or normalize line endings before hashing?
11. **Who owns it**: the commands touch the driver (thread B), the exported names the resolver
    (A), the project description (E). Proposed: a new thread for phases 0 and 1, with small hunks
    in B's and A's files after telling the coordinator.
12. **Ask NFS** (kovarex, as the account holder): may agent sessions `git push` to kpp.net many
    times a day (their automation policy allows unattended SSH only for daily backups and
    occasional uploads)? May a site's hook or daemon open outbound connections (to push the mirror
    to GitHub)? The FreeBSD version of the current realm, whether `git` and `clang` are on the SSH
    hosts, a daemon's memory limit, and SQLite locking on their file storage.
13. **programs/factorio/ and the license**: LICENSE lists the whole directory as not covered
    (its overrides derive from Factorio's source). Should Kpp's own files there (the converter's
    scripts, tests, harnesses) be Apache too, with only the game-derived files excluded? And the
    trademark steps in [Licensing](#licensing-decided-2026-11-01).

## Sources
1. Factorio Auxiliary Docs, Mod structure (info.json, dependencies):
   https://lua-api.factorio.com/latest/auxiliary/mod-structure.html
2. Factorio wiki, Mod portal API: https://wiki.factorio.com/Mod_portal_API
3. Factorio wiki, Mod upload API: https://wiki.factorio.com/Mod_upload_API
4. Factorio wiki, Mod publish API: https://wiki.factorio.com/Mod_publish_API
5. Factorio wiki, Mod details API: https://wiki.factorio.com/Mod_details_API
6. Factorio wiki, Mods (sync mods with save): https://wiki.factorio.com/Mods
7. Factorio forums, sync dialog with "Sync startup settings": https://forums.factorio.com/103386
8. Factorio wiki, Multiplayer (identical mods, checksums on join):
   https://wiki.factorio.com/Multiplayer
9. cargo yank: https://doc.rust-lang.org/cargo/commands/cargo-yank.html
10. Rust blog, malicious crates faster_log and async_println (2025-09-24):
    https://blog.rust-lang.org/2025/09/24/crates.io-malicious-crates-fasterlog-and-asyncprintln
11. RUSTSEC-2025-0155 (rands): https://osv.dev/vulnerability/RUSTSEC-2025-0155
12. finch-rst removed: https://advisories.gitlab.com/pkg/cargo/finch-rst
13. Alpha-Omega, trusted publishing on crates.io:
    https://alpha-omega.dev/blog/trusted-publishing-secure-rust-package-deployment-without-secrets/
14. Rust blog, crates.io development update (2026-01-21):
    https://blog.rust-lang.org/2026/01/21/crates-io-development-update
15. Go modules reference (MVS, go.sum, checksum database): https://go.dev/ref/mod
16. matklad, Minimal Version Selection Revisited:
    https://matklad.github.io/2024/12/24/minimal-version-selection-revisited.html
17. vcpkg registries: https://learn.microsoft.com/vcpkg/concepts/registries
18. vcpkg, lock package versions (baseline):
    https://learn.microsoft.com/vcpkg/consume/lock-package-versions
19. Conan 2, conan lock: https://docs.conan.io/2/reference/commands/lock.html
20. Zig, build.zig.zon: https://github.com/ziglang/zig/blob/master/doc/build.zig.zon.md
21. npm unpublish policy: https://docs.npmjs.com/policies/unpublish/
22. npm blog, unpublish policy changes (January 2020):
    https://blog.npmjs.org/post/190553543620/changes-to-npmunpublish-policy-january-2020
23. Shai-Hulud, first wave: https://versprite.com/resources/security/npm-supply-chain-attack-shai-hulud-worm-compromises-500-packages/
24. Shai-Hulud 2.0: https://www.hivepro.com/threat-advisory/shai-hulud-2-0-fuels-global-npm-supply-chain-compromise/
25. Russ Cox, The xz attack shell script: https://research.swtch.com/xz-script
26. NFS FAQ, hardware: https://faq.nearlyfreespeech.net/section/technology/hardware
27. NFS blog, "Bigger, better, faster, more" (2023-08-22): https://blog.nearlyfreespeech.net/2023/08/22/bigger-better-faster-more/
28. NFS FAQ, site realms: https://faq.nearlyfreespeech.net/section/programming/siterealm
29. NFS FAQ, servers and services: https://faq.nearlyfreespeech.net/section/technology/servers
30. NFS FAQ, what won't go: https://faq.nearlyfreespeech.net/section/technology/wontgo
31. NFS FAQ, compiling your own CGI applications: https://faq.nearlyfreespeech.net/section/programming/compileapps
32. NFS FAQ, CGI: https://faq.nearlyfreespeech.net/section/programming/cgi
33. NFS FAQ, CPU limits: https://faq.nearlyfreespeech.net/section/policy/cpulimit
34. NFS FAQ, daemon processes and proxies: https://faq.nearlyfreespeech.net/full/daemonprocesses
35. NFS blog, server types (2023-12-26): https://blog.nearlyfreespeech.net/2023/12/26/small-christmas-upgrades/
36. NFS blog, posts on proxies: https://blog.nearlyfreespeech.net/?s=proxy
37. NFS blog, automatic TLS (2024-05-10): https://blog.nearlyfreespeech.net/2024/05/10/automatic-tls-is-now-a-thing/
38. NFS FAQ, TLS: https://faq.nearlyfreespeech.net/section/technology/ssl
39. NFS FAQ, SSH: https://faq.nearlyfreespeech.net/section/uploading/sshprogram
40. NFS FAQ, SSH keys: https://faq.nearlyfreespeech.net/section/uploading/sshkeys
41. NFS FAQ, automated SSH access: https://faq.nearlyfreespeech.net/section/uploading/automatedssh
42. A member's how-to, a bare git repository on NFS: https://www.xanthir.com/b4or0
43. NFS, databases: https://www.nearlyfreespeech.net/services/database
44. NFS, pricing: https://www.nearlyfreespeech.net/services/pricing and https://www.nearlyfreespeech.net/services/hosting
45. LLVM's LICENSE.TXT (Apache 2.0 with LLVM Exceptions): https://github.com/llvm/llvm-project/blob/main/LICENSE.TXT
46. SPDX, Apache-2.0 and LLVM-exception texts: https://spdx.org/licenses/Apache-2.0.html, https://spdx.org/licenses/LLVM-exception.html
47. LLVM relicensing, why the exceptions: https://llvm.org/foundation/relicensing/
48. Developer Certificate of Origin 1.1: https://developercertificate.org/
49. EU trade mark fees per class (third-party summary of EUIPO's schedule): https://tmarkmetric.com/insights/eu-trademark-cost
50. USPTO trademark fees from 2025-01-18: https://www.fr.com/insights/thought-leadership/blogs/new-uspto-trademark-fees-go-into-effect-january-18-2025/
