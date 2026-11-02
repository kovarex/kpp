# kpp.net runbook (NearlyFreeSpeech.NET)

These steps are for a person at a keyboard. Steps marked **(kovarex)** need his NFS account,
GitHub account or keys. Nothing here has been run, and no Claude thread may run these steps.

Order matters: GitHub stays the place everyone pushes to until step 5. The one placeholder is
`<host>`: the SSH host NFS's panel shows for the site, as in `ssh.<host>.nearlyfreespeech.net`.
The design is in [docs/library-registry.md](../docs/library-registry.md), "Hosting on kpp.net".

**No thread connects to kpp.net** (SSH, uploads, CI publishing) until kovarex says go.

## Next, for kovarex
1. **The SSH host and the system**:
   - Log in with `ssh kpp_kovarex@ssh.<host>.nearlyfreespeech.net` and tell the coordinator the
     host name.
   - Run step 2's line, which starts with `uname -a` for the FreeBSD version.
   - Check that CGI works on the Generic server type. Put this as `/home/public/cgi-test.cgi`,
     run `chmod 755 /home/public/cgi-test.cgi`, open https://kpp.net/cgi-test.cgi (it should show
     the `uname` line), then delete it:
     ```sh
     #!/bin/sh
     printf 'Content-Type: text/plain\r\n\r\n'
     uname -a
     ```
2. **CI's key for publishing**: step 8 below. It sets the secret `KPP_NET_SSH_KEY` and the
   variables `KPP_NET_SSH` (`kpp_kovarex@ssh.<host>.nearlyfreespeech.net`) and
   `KPP_NET_KNOWN_HOSTS`. CI publishes kpp's packages and the SDL3 mirror from then on.
3. **The first static upload**, once G's site generator produces pages. It goes to
   `/home/public/` over SSH (step 7 creates `registry/` beside it).

Then steps 3 to 6 (repositories, mirror, the switch of pushes, backups), when the coordinator
schedules them.

## 1. The site (kovarex) — done 2026-11-01
- The site `kpp` exists, with server type **Apache 2.4 Generic** (static files, CGI, and daemons
  with proxies; no PHP).
- Its aliases are `kpp.net` and `www.kpp.net`.
- The SSH login is `kpp_kovarex`.
- Still to check: that https://kpp.net serves over TLS, and that your Ed25519 key is in the member
  keychain (Profile > SSH keys).

## 2. What the realm has (kovarex, over SSH)
```sh
uname -a; freebsd-version; which git clang cc rsync sqlite3; git --version; clang --version
```
Write the output into docs/library-registry.md ("What NFS offers"): the FreeBSD version, and
whether git and clang are there. Send NFS support the questions in [README.md](README.md),
item 4, and record the answers in the same place.

## 3. Bare repositories on kpp.net (kovarex, over SSH)
```sh
mkdir -p /home/private/git
cd /home/private/git
git init --bare --initial-branch=master kpp.git
```
`registry-index.git` comes when the first library is published, and `libraries/<name>.git` only
when a library leaves `kpp.git` (docs/library-registry.md, "Git"). factorio-in-kpp is **not** put
on kpp.net: it stays private on GitHub (kovarex, 2026-11-01).

From PC-PRACOVNA, fill it from the current GitHub repository (all branches and tags):
```sh
git -C C:/kpp push --mirror kpp_kovarex@ssh.<host>.nearlyfreespeech.net:/home/private/git/kpp.git
```
`/home/private` is not served on the web. Run `git ls-remote` against it and check that master is
the same commit as on GitHub.

## 4. Mirroring to GitHub (kovarex)
**Option A, if NFS allows outbound SSH from the site** (README.md, question 2):
1. On kpp.net: `ssh-keygen -t ed25519 -f /home/private/.ssh/github-mirror -N ""`. Add the
   `.pub` as a **deploy key with write access** on each GitHub repository (Settings > Deploy keys).
2. Install [git/post-receive](git/post-receive) as `hooks/post-receive` (made executable) in each
   bare repository, and set each repository's mirror URL:
   `git --git-dir=/home/private/git/kpp.git config kpp.mirror git@github.com:kovarex/kpp.git`.
3. Test it: push a throwaway branch to kpp.net, see it appear on GitHub, then delete it on
   kpp.net and see it disappear there.
4. On GitHub, each repository > Settings > Rules > New branch ruleset, targeting all branches:
   "Restrict updates" and "Restrict deletions", with the deploy key as the only bypass actor. From
   then on, nobody can push or merge on GitHub; everything goes through kpp.net.

**Option B, otherwise:** each clone gets two push URLs, kpp.net first:
```sh
git -C C:/kpp remote set-url --push origin kpp_kovarex@ssh.<host>.nearlyfreespeech.net:/home/private/git/kpp.git
git -C C:/kpp remote set-url --add --push origin git@github.com:kovarex/kpp.git
```
Then `git push` updates both. The fetch URL stays on GitHub, where pull requests arrive. There is no
ruleset; the merge button is left alone by convention.

## 5. Switch the pushes (kovarex and the coordinator)
1. The coordinator pauses all pushes for the switch, which takes about ten minutes.
2. Option A: in every clone of kpp (C:\kpp; factorio-in-kpp's clones stay as they are), run
   `git remote set-url --push origin <kpp.net URL>`. Worktrees share their clone's remote.
   Option B: the two commands in step 4 for each clone.
3. If the threads push with their own key, which needs NFS's answer to question 1, add that key to
   the keychain too.
4. One test push from a thread. Check that it lands on kpp.net and on GitHub.
5. The coordinator lifts the pause. CI keeps running on GitHub Actions, triggered by the mirror's
   pushes.

## 6. Backups (kovarex)
1. On PC-PRACOVNA: run `ssh-keygen -t ed25519 -f %USERPROFILE%\.ssh\kppnet-backup` and add the
   `.pub` to the keychain.
2. Create a scheduled task, daily at 05:00:
   `powershell -NoProfile -ExecutionPolicy Bypass -File C:\kpp\kppnet\backup\pull-backup.ps1 -SshTarget kpp_kovarex@ssh.<host>.nearlyfreespeech.net -Destination D:\backup\kpp.net`
   Point the destination at a disk that is itself backed up. A second copy elsewhere (Wube's NAS,
   cloud storage) is cheap insurance.
3. Run it once by hand and check that it ends with "backup ... done".

## 7. The registry directory (kovarex, once there is a first library)
```sh
mkdir -p /home/public/registry/index /home/public/registry/archives /home/public/registry/mirror /home/public/registry/kpp
```
Put the `.htaccess` from [registry/layout.md](registry/layout.md) into `/home/public/registry/`.
Libraries (stage 1) are published by a maintainer's `kpp publish`.

## 8. CI's access to kpp.net (kovarex)
CI writes kpp's packages to `registry/kpp/` and mirrored files to `registry/mirror/` with
`tools/release/publish.ps1`, over OpenSSH (ci.yml; docs/build.md "Continuous integration").
Without these settings it skips kpp.net and says so.
1. On PC-PRACOVNA, make a key for CI only, without a passphrase:
   `ssh-keygen -t ed25519 -f kppnet-ci -N "" -C ci@kpp.net`.
2. Add `kppnet-ci.pub` to NFS's member keychain.
3. Read the server's host key line:
   `ssh-keyscan -t ed25519 ssh.<host>.nearlyfreespeech.net`.
   Check its fingerprint (`ssh-keygen -lf`) against what your own first `ssh` login showed.
4. On GitHub, kovarex/kpp > Settings > Secrets and variables > Actions, set:
   - the **secret** `KPP_NET_SSH_KEY`: the whole text of `kppnet-ci` (the private key);
   - the **variable** `KPP_NET_SSH`: `kpp_kovarex@ssh.<host>.nearlyfreespeech.net`;
   - the **variable** `KPP_NET_KNOWN_HOSTS`: the line from step 3.

   Then delete the local copy of `kppnet-ci`.
5. Check: run ci on master by hand (Actions > ci > Run workflow). Its log says it published to
   kpp.net, and https://kpp.net/registry/kpp/latest.json names the run's version. The first such
   run also mirrors the SDL3 zip to `registry/mirror/<sha256>/`, and later runs fetch it from
   there.

## Running it
- **Realm upgrades:** NFS's "late" setting upgrades about every six months. Before switching, run
  the site's kpp programs (stage 2 on) against the upcoming realm, unless they are statically
  linked.
- **Leaving NFS:** the repositories are plain bare git repositories (and every clone has them), and
  the registry and site are files. Copy them to the new host with rsync, then move the DNS.
