# kpp.net runbook (NearlyFreeSpeech.NET)

These steps are for a person at a keyboard. Steps marked **(kovarex)** need his NFS account,
GitHub account or keys. Nothing here has been run, and no Claude thread may run these steps.

Order matters: GitHub stays the place everyone pushes to until step 5. Placeholders:
- `<user>`: his NFS member name;
- `<host>`: the SSH host NFS shows for the site.

The design is in [docs/library-registry.md](../docs/library-registry.md), "Hosting on kpp.net".

## 1. The site (kovarex)
1. In NFS's member panel, create the site `kpp` as a **production** site. For the server type, take
   Apache with CGI for now. A type that also allows daemons and proxies is needed only for stage 3,
   and the type can be changed later.
2. Add the aliases `kpp.net` and `www.kpp.net`, and let NFS set the DNS (the domain is registered
   there).
3. Wait for automatic TLS, then check that https://kpp.net shows the empty site.
4. Add your Ed25519 public key to the member keychain (Profile > SSH keys), and check:
   `ssh <user>_kpp@ssh.<host>.nearlyfreespeech.net`.

## 2. What the realm has (kovarex, over SSH)
```sh
uname -a; freebsd-version; which git clang cc rsync sqlite3; git --version; clang --version
```
Write the output into docs/library-registry.md ("What NFS offers"): the FreeBSD version, and
whether git and clang are there. Send NFS support the questions in [README.md](README.md),
item 4, and record the answers in the same place.

## 3. Bare repositories on kpp.net (kovarex, over SSH)
```sh
mkdir -p /home/private/git/libraries
cd /home/private/git
git init --bare --initial-branch=master kpp.git
git init --bare --initial-branch=master factorio-in-kpp.git
git init --bare --initial-branch=master registry-index.git
```
From PC-PRACOVNA, fill them from the current GitHub repositories (all branches and tags):
```sh
git -C C:/kpp push --mirror <user>_kpp@ssh.<host>.nearlyfreespeech.net:/home/private/git/kpp.git
git -C C:/factorio-in-kpp push --mirror <user>_kpp@ssh.<host>.nearlyfreespeech.net:/home/private/git/factorio-in-kpp.git
```
`/home/private` is not served on the web, and factorio-in-kpp, which is converted game source,
stays there. Run `git ls-remote` against both and check that master is the same commit as on GitHub.

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
git -C C:/kpp remote set-url --push origin <user>_kpp@ssh.<host>.nearlyfreespeech.net:/home/private/git/kpp.git
git -C C:/kpp remote set-url --add --push origin git@github.com:kovarex/kpp.git
```
Then `git push` updates both. The fetch URL stays on GitHub, where pull requests arrive. There is no
ruleset; the merge button is left alone by convention.

## 5. Switch the pushes (kovarex and the coordinator)
1. The coordinator pauses all pushes for the switch, which takes about ten minutes.
2. Option A: in every clone (C:\kpp, C:\factorio-in-kpp, C:\kpp-sandbox\fik-convert), run
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
   `powershell -File C:\kpp\kppnet\backup\pull-backup.ps1 -SshTarget <user>_kpp@ssh.<host>.nearlyfreespeech.net -Destination D:\backup\kpp.net`
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
   - the **variable** `KPP_NET_SSH`: `<user>_kpp@ssh.<host>.nearlyfreespeech.net`;
   - the **variable** `KPP_NET_KNOWN_HOSTS`: the line from step 3.

   Then delete the local copy of `kppnet-ci`.
5. Check: run ci on master by hand (Actions > ci > Run workflow). Its log says it published to
   kpp.net, and https://kpp.net/registry/kpp/latest.json names the run's version. The first such
   run also mirrors the SDL3 zip to `registry/mirror/<sha256>/`, and later runs fetch it from
   there.

## Running it
- **Realm upgrades:** NFS's "late" setting upgrades about every six months. Before switching, run
  the site's Kpp programs (stage 2 on) against the upcoming realm, unless they are statically
  linked.
- **Leaving NFS:** the repositories are plain bare git repositories (and every clone has them), and
  the registry and site are files. Copy them to the new host with rsync, then move the DNS.
