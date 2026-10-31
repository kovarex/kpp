# The registry's file store on registry.kpp.net

Every file kpp downloads comes from here: library releases, the files CI needs from third parties,
and kpp's own releases. All of it is static files served by Caddy (`server/Caddyfile`) from
`/srv/kppnet/data/registry`, written only by the publish job through the `registry` user's rrsync
(write-only, `server/setup-host.sh`). There is no server code: a mirror is a copy of this directory
and any web server. Design: [docs/library-registry.md](../../docs/library-registry.md), "Hosting
on kpp.net".

```
registry.kpp.net/
  index/
    config.json                 {"format": 1, "hash": "sha256-tree", "archives": "https://registry.kpp.net/archives/"}
    <name>.json                 one file per library: every release (below); the only files that change
  archives/
    <hh>/<hex>.zip              a release's files; <hex> is its tree hash, <hh> its first two digits
    <hh>/<hex>.manifest         the manifest the tree hash is the SHA-256 of (tree_hash.py)
    <hh>/<hex>.exports          its top-level names (kppc --exports), for the clash check before download
  mirror/
    <sha256>/<file name>        third-party files CI and builds download (SDL3-devel-3.4.18-VC.zip...),
                                named by the SHA-256 of the file itself
  kpp/
    releases/<version>/         kpp's own packages: kpp-<version>.zip, kpp-<version>-setup.exe, kpp.vsix
    latest.json                 {"version", "commit", "zip", "sha256"}: what GitHub's "latest" prerelease is today
```

**A release in `index/<name>.json`:**

```json
{
  "name": "json",
  "releases": [
    {
      "version": "1.4.2",
      "tree": "sha256-tree:3f9c...",
      "archive": "archives/3f/3f9c....zip",
      "archive_sha256": "9a1b...",
      "size": 48213,
      "kpp": "0.2",
      "dependencies": [{"name": "zlib", "min": "1.3.1"}],
      "published": "2026-11-12T10:31:07Z",
      "commit": "https://git.kpp.net/libraries/json/commit/8d0c...",
      "yanked": null
    }
  ]
}
```

**Rules.**
- A library version is `name` + `version` + `tree`; build.lock records all three. kpp downloads
  `archive` from registry.kpp.net (or a configured mirror), unpacks it, recomputes the tree hash and
  refuses a mismatch. It never downloads from a forge or from GitHub.
- Write once: `tree_hash.py pack` skips an archive already in the store (the same hash is the same
  content) and never replaces one; the publish job uploads with `rsync --ignore-existing`. A release's entry in
  the index changes only to set `yanked` (a reason string).
- Removal (malware, legal) deletes the archive and keeps the index entry with `"removed": "<reason>"`,
  so the version number is never reused.
- `tree_hash.py verify` recomputes every archive's hash; the nightly job runs it.
- `mirror/` holds what used to come from github.com in CI (`ci.yml`'s SDL3 headers, Node.js for
  `setup-node` if wanted): `tree_hash.py mirror FILE STORE` prints the URL to put in the workflow,
  with the SHA-256 to check.
