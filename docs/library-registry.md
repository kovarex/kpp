# A library registry for Kpp (proposed 2026-10-30; hosting decided 2026-10-31)

**The ask** (kovarex): a package installer for Kpp libraries, like Factorio's mods: a service
written in Kpp where libraries are registered, from which a library is added to a project in one
step. This document is research and a proposal. Decided so far: where it lives ([Hosting on
kpp.net](#hosting-on-kppnet-decided-2026-10-31), kovarex 2026-10-31) and, in
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
hosted on kpp.net (decided): a Forgejo forge at git.kpp.net holds the libraries' repositories,
reviews and CI, registry.kpp.net serves the index and the hash-addressed archives as static
files, and GitHub is only a read-only mirror. The first version needs no registry service of its
own beyond that; the service in Kpp (accounts, web pages, publishing API) comes
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
  [the service](#the-service), [hosting on kpp.net](#hosting-on-kppnet-decided-2026-10-31),
  [security](#security-and-moderation),
  [mirrors and offline](#mirrors-offline-and-private-registries), [IDE and VS](#ide-and-visual-studio)
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
declare build steps. So `kppc add` above is `kpp add`, which only inserts the `use` line. What
the registry still needs from E's format: the library's manifest fields listed in
[What a library is](#what-a-library-is) (version, license, minimum kpp, release paths, C parts),
and build.lock recording, per library, name, version, the tree hash (`sha256-tree:...`, defined by
[kppnet/registry/tree_hash.py](../kppnet/registry/tree_hash.py)) and the registry it came from.

### The service
- **Phase 1 needs no registry program**: the index is a git repository on the forge
  (`git.kpp.net/kpp/registry-index`, one file per library listing its releases: version, tree
  hash, archive path, dependencies, exported names, yanked), as crates.io's index was a git
  repository for years. Publishing: a library's repository (official ones in the `libraries`
  organization on git.kpp.net) tags a version; its workflow builds and tests it with the current
  kpp, packs it (`tree_hash.py pack`), computes `--exports` and opens a pull request on the index
  that adds the release; CI there checks the manifest, the hash, name rules and clashes; kovarex or
  a maintainer merges; the merge uploads the archive and the index file to registry.kpp.net
  (write-once, [kppnet/registry/layout.md](../kppnet/registry/layout.md)). kpp reads only
  registry.kpp.net's static files.
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

### Hosting on kpp.net (decided 2026-10-31)
**The decision** (kovarex, the coordinator's option A): official Kpp
libraries, and later kpp itself, live in git repositories owned and hosted on kpp.net, with a
forge there for pull requests, issues, review and CI. GitHub may be used, but only as a mirror:
nothing may depend on it existentially, and moving to another host must stay possible. In his
words: "I prefer to build A right away." The deployment kit is [kppnet/](../kppnet/README.md);
nothing is deployed yet.

**The names.**
- `git.kpp.net`: the forge. Organizations `kpp` (the compiler; factorio-in-kpp, private) and
  `libraries` (official libraries); `actions` holds pull mirrors of the few GitHub Actions the
  workflows use.
- `registry.kpp.net`: static files only: the registry index, the archives by tree hash, kpp's own
  releases, and mirrored third-party files CI needs
  ([kppnet/registry/layout.md](../kppnet/registry/layout.md)).
- `kpp.net`: redirects to the compiler's repository until there is a site.

**The forge: Forgejo** (15.x, the LTS line, supported until 2027-07-15 [26]), with SQLite, behind
Caddy for TLS, in Docker on one Debian VM. Why it and not:
- *Gitea*: the same code base before the fork; Forgejo is run by a non-profit (Codeberg e.V.),
  is free software throughout, and is what Codeberg runs. Either would do; Forgejo has no
  company that can change the terms.
- *GitLab CE*: more mature CI, and its runner officially supports Windows, which is Forgejo's weak
  point here (below). But it wants 8 GB or more for itself, many services to run and upgrade,
  and a heavier backup and restore. It is the fallback if no runner works on Windows.
- *SourceHut, Gerrit*: patches by mail, or review only; kovarex asked for pull requests, issues,
  review and CI in one place.

**Not depending on GitHub**, item by item:
- Code: the repositories on git.kpp.net are the originals; GitHub gets a force-pushed copy on
  every push (a push mirror [27]), with its issues, pull requests and Actions turned off.
  Dropping GitHub is deleting the mirror.
- Downloads: kpp fetches libraries only from registry.kpp.net (or a configured mirror of it), by
  name, version and tree hash, and checks the hash; build.lock records all three. It never
  downloads from GitHub or from a forge URL.
- CI: workflows run on Forgejo Actions on our own runner. `uses: actions/checkout@v4` resolves on
  git.kpp.net (`DEFAULT_ACTIONS_URL = self` and the mirrored `actions/*` repositories [28]), and
  files CI downloads from github.com today (ci.yml's SDL3 headers) move to registry.kpp.net/mirror.
- Sign-in: contributors use "Sign in with GitHub" (OAuth) for convenience; it creates a forge
  account, which can get a password by mail if GitHub sign-in ever goes away. Admins have local
  accounts with 2FA. No password sign-up for others, which keeps out spam accounts.

**CI.** ci.yml and release.yml are kept portable between GitHub Actions and Forgejo Actions
(thread H, through the coordinator): Forgejo reads `.github/workflows` when there is no
`.forgejo/workflows` [29], its `github` context is its own `forgejo` context under that name, and
`concurrency`, `schedule` and `workflow_dispatch` work as on GitHub [30]. The runner registers the
label `windows-2022:host`, so `runs-on: windows-2022` is unchanged; jobs run directly on a
dedicated Windows machine with Visual Studio Build Tools and clang, as GitHub's image has them.
What is not portable is publishing: "Publish latest" (`gh release`) and release.yml's
`softprops/action-gh-release` become uploads to registry.kpp.net (`kpp/releases/`,
`kpp/latest.json`), which factorio-in-kpp's CI then reads.
**Open risk:** Forgejo Runner's releases have Linux binaries only (v13.2.1, 2026-10-29), and
running it on Windows in host mode is not documented [31]. The kit builds it from source for
Windows, with Gitea's act_runner (which ships a Windows binary, v5.2.4 [32]) as the fallback; the
runbook's smoke test decides which works, and the result is recorded here. If neither does, the
options are a Linux runner driving a Windows VM, or GitLab.

**Backups and leaving.** A nightly `forgejo dump` and the registry's files go to restic on storage
at another provider, kept 14 days, 8 weeks, 12 months, with a tenth read back each night and a
yearly restore drill (kppnet/RUNBOOK.md step 8). Moving host is restoring the backup elsewhere and
changing DNS; leaving Forgejo is taking the plain git repositories (GitHub's mirror has them too)
and exporting issues through the API; the registry is static files any server can serve.

**Cost**: about €15 to €20 a month for the server and backups, plus the Windows runner (a machine
Wube or kovarex owns: nothing; rented: roughly €60 to €150 a month). What kovarex must provide or
do by hand (domain, server, runner machine, accounts, OAuth app, secrets) is listed in
[kppnet/README.md](../kppnet/README.md).

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
- **Publishing**: from CI on git.kpp.net only (the upload key is an Actions secret of the forge,
  never a person's); later, short-lived per-job tokens as crates.io's trusted publishing; tokens
  scoped to one library and one action; 2FA. A security advisory list and `kppc audit` (phase 3).
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

## Phases and effort
Estimates are in weeks of one thread, assuming thread E's project description exists.

| Phase | Contents | Effort |
|---|---|---|
| 0. Local libraries | `--path`/`--git` dependencies in the project description, cache, lockfile with SHA-256, `kppc add/remove/sync/vendor`, `kppc --exports` and the clash pre-check, C source parts compiled by kppc (moving `programs/factorio/native`'s build into this is the test) | 2 to 3 |
| H. Hosting | the forge, CI runner, backups, mirrors, registry store on kpp.net: the kit is [kppnet/](../kppnet/README.md) (done); deploying it is kovarex's steps in its RUNBOOK.md plus checking the Windows runner | 1 to 2 days of his time, plus the runner machine |
| 1. Registry without a service | index repository on git.kpp.net, archives on registry.kpp.net, CI checks on publish pull requests, MVS, `update`, yanking, `search` over the index, embedded library list in the exe, first libraries: zlib, lua, imgui, json, a Kpp face over sockets | 2 to 3, plus porting the first libraries |
| 2. The service in Kpp | stdlib networking/HTTP/JSON/SQLite face first (3 to 5 weeks, useful beyond this), then API, accounts, web pages, publish API, tokens, 2FA, download counts | 6 to 10 |
| 3. Hardening | trusted publishing, transparency log or signed index, advisories and `kppc audit`, typosquat review queue, `internal` declarations if clashes are measured | 4 to 6 |
| 4. Tools | IDE Libraries view, VS commands, generated documentation pages per library from reflection, more targets for binary parts, private registry setup | with threads E and F |

The minimal first version is phases 0 and 1 on the hosting of phase H: about 5 weeks, no
registry program of its own,
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
8. **Hosting**: decided 2026-10-31, kovarex: "I prefer to build A right away": repositories,
   reviews and CI on a self-hosted forge on kpp.net, GitHub only as a read-only mirror
   ([Hosting on kpp.net](#hosting-on-kppnet-decided-2026-10-31)). Still his: confirming kpp.net is
   his, the server provider, the runner machine ([kppnet/README.md](../kppnet/README.md)).
9. **Accounts**: decided with 8: contributors sign in to the forge with GitHub (OAuth); admins
   have local accounts with 2FA. factorio.com accounts remain possible later for the registry's
   own service (phase 2).
10. **Hash normalization**: hash the files as stored (proposed; the repository's own
    `.gitattributes` fixes line endings) or normalize line endings before hashing?
11. **Who owns it**: the commands touch the driver (thread B), the exported names the resolver
    (A), the project description (E). Proposed: a new thread for phases 0 and 1, with small hunks
    in B's and A's files after telling the coordinator.

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
26. Forgejo releases and support periods: https://forgejo.org/releases/
27. Forgejo, repository mirrors (push mirrors to GitHub): https://forgejo.org/docs/latest/user/repo-mirror/
28. Forgejo Actions, using actions (DEFAULT_ACTIONS_URL): https://forgejo.org/docs/latest/user/actions/actions/
29. Forgejo Actions user guide (.github/workflows when .forgejo/workflows is absent):
    https://forgejo.org/docs/latest/user/actions/overview/
30. Forgejo Actions reference (contexts, concurrency, schedule, workflow_dispatch):
    https://forgejo.org/docs/latest/user/actions/reference/
31. Forgejo Runner releases: https://code.forgejo.org/forgejo/runner/releases; installation:
    https://forgejo.org/docs/latest/admin/actions/installation/binary/
32. Gitea act_runner releases (windows-amd64): https://gitea.com/gitea/act_runner/releases
