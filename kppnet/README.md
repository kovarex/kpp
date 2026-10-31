# kppnet: kpp.net on NearlyFreeSpeech.NET

kovarex's decisions (2026-10-31):
- kpp.net is his, at NearlyFreeSpeech.NET (NFS), and it hosts the site.
- The canonical git repositories live on kpp.net, and GitHub is the mirror where pull requests and
  issues happen.
- The website and the library registry are written in Kpp.

The design, with what NFS offers and its sources, is in
[docs/library-registry.md](../docs/library-registry.md), "Hosting on kpp.net". This directory holds
the operational part. **Nothing here has been run against kpp.net.**

| Path | What |
|---|---|
| [RUNBOOK.md](RUNBOOK.md) | the steps, in order, with who does each |
| [git/post-receive](git/post-receive) | the hook on kpp.net that pushes every update to GitHub (if NFS allows outbound connections) |
| [backup/pull-backup.ps1](backup/pull-backup.ps1) | the daily pull of kpp.net's repositories and registry files to PC-PRACOVNA (NFS allows automated SSH for one backup a day) |
| [site/](site/README.md) | kppsite, the site generator in Kpp: docs and library pages as static files (stage 1) |
| [registry/layout.md](registry/layout.md), [registry/tree_hash.py](registry/tree_hash.py) | the hash-addressed store under https://kpp.net/registry/ and the reference tree hash that build.lock records |

An earlier version of this directory held a self-hosted Forgejo kit for a rented VM. It was
dropped when the hosting moved to NFS (git history: bbf5cf61).

## What kovarex provides or does by hand
No thread can do these: they need his NFS account, his keys or his answers.
1. **The NFS site** for kpp.net: **created 2026-11-01**.
   - Name `kpp`, server type Apache 2.4 Generic (static files, CGI, daemons and proxies; no PHP).
   - Aliases `kpp.net` and `www.kpp.net`.
   - SSH login `kpp_kovarex@ssh.<host>.nearlyfreespeech.net`; the host is the one his panel shows.

   What he does next is the checklist at the top of [RUNBOOK.md](RUNBOOK.md), "Next, for kovarex".
2. **SSH keys**: his Ed25519 public key in the NFS member keychain. A separate key for the threads'
   pushes, if NFS allows those (question 1 below), one for the daily backup pull, and CI's key with
   the GitHub settings `KPP_NET_SSH_KEY` (secret), `KPP_NET_SSH` and `KPP_NET_KNOWN_HOSTS`
   (variables) (RUNBOOK.md step 8).
3. **The GitHub side**:
   - the mirror's deploy key with write access, for the hook;
   - a ruleset on each mirrored repository that lets only that deploy key update branches;
   - GitHub's wiki and projects turned off, with a description pointing to kpp.net.
4. **Questions for NFS support**, which only the account holder can ask:
   1. Agent sessions push to git many times a day with SSH keys. Is that allowed under the
      automated-access policy (backups at most daily, uploads under one file per five minutes)?
   2. May a site's git hook, or a daemon, open outbound SSH or HTTPS connections, to push the
      mirror to GitHub?
   3. What is the FreeBSD version of the current realm? Are `git` and `clang` on the SSH hosts?
   4. Is there a memory limit for a daemon?
   5. Is SQLite's file locking safe on the site's storage?
5. **Monthly cost**: about $5 to $9: membership up to $5, the site $1.50, and storage at $1 a GiB.
   Bandwidth is not billed at present.
