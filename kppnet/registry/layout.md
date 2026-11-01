# The registry's file store on kpp.net

Every file kpp downloads comes from here: library releases, the files CI needs from third parties,
and kpp's own releases. All of it is static files that NFS's Apache serves from the site's
`/home/public/registry/` as https://kpp.net/registry/. There is no server code in stage 1, so a
mirror is just a copy of this directory on any web server. The files are written only by
`kpp publish` (a maintainer's machine, over SSH) until the registry's CGI program takes publishes
(stage 2). Design: [docs/library-registry.md](../../docs/library-registry.md), "Hosting on
kpp.net".

```
https://kpp.net/registry/
  .htaccess                   cache headers (below)
  index/
    config.json               {"format": 1, "hash": "sha256-tree", "archives": "https://kpp.net/registry/archives/"}
    <name>.json               one file per library: every release (below); the only files that change
  archives/
    <hh>/<hex>.zip            a release's files; <hex> is its tree hash, <hh> its first two digits
    <hh>/<hex>.manifest       the manifest the tree hash is the SHA-256 of (tree_hash.py)
    <hh>/<hex>.exports        its top-level names (kppc --exports), for the clash check before download
  mirror/
    <sha256>/<file name>      third-party files CI and builds download (SDL3-devel-3.4.18-VC.zip...),
                              named by the SHA-256 of the file itself
  kpp/
    releases/<version>/       kpp's own packages: kpp-<version>.zip, kpp-<version>-setup.exe, kpp.vsix
    latest.json               {"version", "commit", "zip", "sha256"}: what GitHub's "latest" prerelease is today;
                              "zip" is relative to registry/kpp/ (releases/<version>/kpp-<version>.zip)
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
      "options": [
        {"name": "debug", "type": "bool", "default": false, "description": "Checks every call's arguments"},
        {"name": "max_depth", "type": "int", "required": true, "description": "Deepest nesting the parser accepts"}
      ],
      "published": "2026-11-12T10:31:07Z",
      "commit": "8d0c...",
      "yanked": null
    }
  ]
}
```

**`.htaccess`** (NFS's Apache reads it; archives and mirrored files never change, the index does):

```apache
Options -Indexes
<If "%{REQUEST_URI} =~ m#^/registry/(archives|mirror|kpp/releases)/#">
  Header set Cache-Control "public, max-age=31536000, immutable"
</If>
<If "%{REQUEST_URI} =~ m#^/registry/(index/|kpp/latest\.json)#">
  Header set Cache-Control "public, max-age=60"
</If>
```

**Rules.**
- A library version is `name` + `version` + `tree`, and build.lock records all three. kpp downloads
  `archive` from kpp.net (or a configured mirror), unpacks it, recomputes the tree hash and refuses
  a mismatch. It never downloads from GitHub.
- Write once: `tree_hash.py pack` skips an archive already in the store (the same hash is the same
  content) and never replaces one. Every upload keeps what is there. CI's
  `tools/release/publish.ps1` uses OpenSSH's ssh and scp, since Windows runners have no rsync. It
  skips a file whose path exists (`test -e`), copies to a temporary name and moves it into place
  with `mv -n`. Only `kpp/latest.json` is replaced, by a rename. A maintainer's upload from
  PC-PRACOVNA may use `rsync --ignore-existing`, which follows the same rule. A release's entry in
  the index changes only to set `yanked` (a reason string).
- `options` are the library's build.kpp options for that release (docs/library-registry.md,
  "Library options"): `default` is the option's value as JSON; an option without a default has
  `"required": true` and no `default`.
- Removal (malware, legal) deletes the archive and keeps the index entry with `"removed": "<reason>"`,
  so the version number is never reused.
- `tree_hash.py verify` recomputes every archive's hash; the daily backup pull runs it on its copy.
- `mirror/` holds what CI used to download from github.com (`ci.yml`'s SDL3 headers, which it now
  fetches from here by SHA-256, with github.com as the fallback). CI's
  `publish.ps1 -Mirror -KppNet <user@host> -Files F` puts a file there. `tree_hash.py mirror FILE
  STORE` does the same into a local copy of the store.
