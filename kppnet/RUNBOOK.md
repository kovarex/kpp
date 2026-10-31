# kpp.net runbook

Step by step, for a person at a keyboard. Steps marked **(kovarex)** need his accounts, money or
secrets; nothing here was run, and no step may be run by a Claude thread. Order matters: GitHub
stays the place everyone pushes to until step 10, and becomes a mirror only at step 11.
The design behind it: [docs/library-registry.md](../docs/library-registry.md), "Hosting on
kpp.net". What to buy and how much it costs: [README.md](README.md).

Placeholders: `<VM-IP>` the server's IPv4 (and `<VM-IP6>` IPv6), `<you>` kovarex's e-mail.

## 1. The domain (kovarex)
1. Log in at the registrar that holds kpp.net and confirm the domain is in your account. Public
   registration data (RDAP, read 2026-10-31): registered 2006-01-03, expires 2030-01-03, registrar
   PDR Ltd. (PublicDomainRegistry.com), name servers at NearlyFreeSpeech.NET, last changed
   2026-10-29. The owner is not public; if it is not yours, stop here (README.md, decision 1).
2. Turn on the registrar's transfer lock and two-factor login; set auto-renew.

## 2. The server and DNS (kovarex)
1. Create the VM (README.md, "Server"): Debian 13, 4 vCPU, 8 GB, 160 GB disk, your SSH public key,
   IPv4 and IPv6. Note `<VM-IP>` and `<VM-IP6>`.
2. At the DNS provider for kpp.net (NearlyFreeSpeech today, or move DNS to the server provider):

   | Name | Type | Value | TTL |
   |---|---|---|---|
   | `kpp.net` | A / AAAA | `<VM-IP>` / `<VM-IP6>` | 300 |
   | `www.kpp.net` | CNAME | `kpp.net` | 300 |
   | `git.kpp.net` | A / AAAA | `<VM-IP>` / `<VM-IP6>` | 300 |
   | `registry.kpp.net` | A / AAAA | `<VM-IP>` / `<VM-IP6>` | 300 |
   | `kpp.net` | CAA | `0 issue "letsencrypt.org"` | 3600 |

3. Check from any machine: `nslookup git.kpp.net` gives `<VM-IP>`.

## 3. Prepare the host
```bash
scp -r kppnet/server root@<VM-IP>:/root/kppnet-server
ssh root@<VM-IP> sh /root/kppnet-server/setup-host.sh
```
It installs Docker, the firewall (22 admin SSH, 2222 git SSH, 80, 443), automatic security
updates, restic, and the `registry` upload user. Git over SSH is on port 2222 so that the host's
own SSH keeps 22; clone URLs read `ssh://git@git.kpp.net:2222/kpp/kpp.git`. (Moving the host's SSH
elsewhere to give git port 22 is possible later; it is easy to lock oneself out, so not now.)

## 4. Start Forgejo and Caddy (kovarex fills the secrets)
```bash
ssh root@<VM-IP>
cp /root/kppnet-server/compose.yaml /root/kppnet-server/Caddyfile /srv/kppnet/
cp /root/kppnet-server/env.example /srv/kppnet/.env && chmod 600 /srv/kppnet/.env
cd /srv/kppnet
docker run --rm codeberg.org/forgejo/forgejo:15 forgejo generate secret SECRET_KEY
docker run --rm codeberg.org/forgejo/forgejo:15 forgejo generate secret INTERNAL_TOKEN
docker run --rm codeberg.org/forgejo/forgejo:15 forgejo generate secret JWT_SECRET
nano .env            # paste the three values, ACME_EMAIL=<you>; mail stays off for now
docker compose up -d
docker compose logs -f caddy    # wait for "certificate obtained successfully" for all four names
```
Open https://git.kpp.net: the Forgejo start page, no installer (INSTALL_LOCK is set).

## 5. The admin account (kovarex)
```bash
docker compose exec -u git forgejo forgejo admin user create --admin --username kovarex --email <you> --random-password --must-change-password
```
It prints a password once. Sign in, set your own, then Settings > Security: turn on two-factor
(TOTP or a security key) and save the recovery codes. Settings > SSH keys: add your public key
(the one GitHub has). Then create the organizations: `kpp` (the compiler, factorio-in-kpp) and
`libraries` (official libraries, later).

## 6. Sign-in with GitHub (kovarex)
1. github.com > Settings > Developer settings > OAuth Apps > New OAuth App: name `kpp.net`,
   homepage `https://git.kpp.net`, callback `https://git.kpp.net/user/oauth2/github/callback`.
   Generate a client secret.
2. On the server:
   ```bash
   docker compose exec -u git forgejo forgejo admin auth add-oauth --name github --provider github --key <client id> --secret <client secret>
   ```
   (Or Site administration > Authentication sources > Add, type OAuth2, provider GitHub.)
3. In a private browser window, "Sign in with GitHub" with another GitHub account: it creates a
   Forgejo user with the GitHub name. Contributors only ever use this; nobody gets a password
   account except admins.

## 7. The CI runner (kovarex provides the machine)
1. Mirrors of the actions the workflows use (on any machine with curl and sh; a token from
   git.kpp.net > Settings > Applications with write:repository, write:organization, write:admin):
   ```bash
   export FORGEJO_TOKEN=<admin token>
   sh kppnet/mirror/mirror-actions.sh
   ```
2. The Windows machine (README.md, "CI runner"): Windows Server 2022/2025 or 10/11 Pro, updated,
   nothing else running on it. Copy `kppnet/runner/windows` there.
3. git.kpp.net > `kpp` organization > Settings > Actions > Runners > Create new runner: copy the
   registration token. In an elevated PowerShell on the Windows machine:
   ```powershell
   .\install-runner.ps1 -Token <registration token> -Name kpp-win-1
   ```
   It asks for a password for the local user `kpprunner` (make one up, keep it). Takes about 30
   minutes (Visual Studio Build Tools).
4. The acceptance test: create the repository `kpp/runner-smoke`, add
   `kppnet/ci/runner-smoke.yml` as `.forgejo/workflows/smoke.yml`, push, open its Actions tab.
   Every step green means ci.yml can run here.
   - If the runner never picks the job up or a step fails because of the runner itself, run
     `install-runner.ps1 -Kind gitea` (Gitea's act_runner, which has a Windows build) instead and
     repeat. Forgejo Runner publishes Linux binaries only, so `-Kind forgejo` builds it from
     source; neither combination with Forgejo 15 on Windows has been tried yet.
   - Write which one passed into docs/library-registry.md ("Hosting on kpp.net", CI) and tell the
     coordinator.
5. Pull requests from forks run code on this machine. Organization `kpp` > Settings > Actions:
   require approval for runs from forks/first-time contributors.

## 8. Backups (kovarex buys the storage)
1. Order a Storage Box (README.md), enable SSH access, and add the server's root key
   (`ssh-keygen -t ed25519` on the server, then the Storage Box's key upload).
2. On the server:
   ```bash
   cat > /srv/kppnet/backup/restic.env <<'EOF'
   RESTIC_REPOSITORY=sftp:<user>@<user>.your-storagebox.de:23/kppnet
   RESTIC_PASSWORD=<a long random password; store it in your password manager too>
   EOF
   chmod 600 /srv/kppnet/backup/restic.env
   cp /root/kppnet-server/backup/* /srv/kppnet/backup/
   . /srv/kppnet/backup/restic.env && export RESTIC_REPOSITORY RESTIC_PASSWORD && restic init
   cp /srv/kppnet/backup/kppnet-backup.service /srv/kppnet/backup/kppnet-backup.timer /etc/systemd/system/
   systemctl daemon-reload && systemctl enable --now kppnet-backup.timer
   systemctl start kppnet-backup.service && journalctl -u kppnet-backup.service -n 20
   ```
3. Restore drill, once now and once a year: on a scratch VM, `restic restore latest --target
   /restore`, unpack `forgejo-dump.tar.zst` into a fresh `/srv/kppnet/data/forgejo` as Forgejo's
   backup documentation describes (data/, repositories, the database file), start the same
   compose.yaml, sign in. Delete the scratch VM afterwards.
   Without the restic password the backups are unreadable: it belongs in the password manager,
   not only on the server.

## 9. The registry store
1. On your machine: `ssh-keygen -t ed25519 -f kpp-publish -C publish@kpp.net` (no passphrase; it is
   a CI key).
2. On the server, put the public key into `/home/registry/.ssh/authorized_keys` behind the
   `command="... rrsync -wo /srv/kppnet/data/registry",restrict` prefix setup-host.sh printed.
3. git.kpp.net > `kpp` organization > Settings > Actions > Secrets: `REGISTRY_SSH_KEY` = the private
   key's text. Delete the local copy of the private key.
4. The files CI downloads from github.com today: `python -I kppnet/registry/tree_hash.py mirror
   SDL3-devel-3.4.18-VC.zip <store>` and upload with
   `rsync --ignore-existing -e "ssh -i kpp-publish" -r <store>/ registry@<VM-IP>:` (thread H
   switches ci.yml to the printed URL).

## 10. Move the repositories (kovarex, with the coordinator)
1. The coordinator stops all pushes for the move (about 15 minutes).
2. A GitHub token that can read both repositories with issues and pull requests (classic, `repo`):
   ```bash
   export FORGEJO_TOKEN=<admin token> GITHUB_MIGRATE_TOKEN=<github token>
   sh kppnet/mirror/migrate-from-github.sh kovarex/kpp kpp kpp private
   sh kppnet/mirror/migrate-from-github.sh kovarex/factorio-in-kpp kpp factorio-in-kpp private
   ```
   factorio-in-kpp is converted game source: it stays private, readable by kovarex and the
   threads' account only (README.md, decision 6).
3. Check: `git ls-remote ssh://git@git.kpp.net:2222/kpp/kpp.git master` shows the same commit as
   GitHub's master; issues and pull requests are there.
4. Point the clones at kpp.net (worktrees share their clone's remote, so once per clone):
   ```bash
   git -C C:/kpp remote set-url origin ssh://git@git.kpp.net:2222/kpp/kpp.git
   git -C C:/factorio-in-kpp remote set-url origin ssh://git@git.kpp.net:2222/kpp/factorio-in-kpp.git
   ```
   plus `C:\kpp-sandbox\fik-convert` and any other clone. `git fetch` in each to check.
5. The coordinator lifts the stop.

## 11. GitHub becomes the mirror (kovarex)
1. On GitHub, for each repository: Settings > General: description "Mirror of
   https://git.kpp.net/kpp/kpp; issues and pull requests there"; uncheck Issues, Projects, Wiki, and
   Pull requests if GitHub offers the switch (otherwise close new ones with that pointer).
2. A fine-grained GitHub token per repository (Contents and Workflows: read and write), then:
   ```bash
   export FORGEJO_TOKEN=<admin token> GITHUB_USER=kovarex GITHUB_MIRROR_TOKEN=<github token>
   sh kppnet/mirror/push-mirror.sh kpp/kpp kovarex/kpp
   sh kppnet/mirror/push-mirror.sh kpp/factorio-in-kpp kovarex/factorio-in-kpp
   ```
   The mirror force-pushes: after this, nothing may be pushed to GitHub directly.
3. Once CI on git.kpp.net is green for master (step 12): GitHub > Settings > Actions > Disable
   actions, so the mirror does not run (and bill) CI twice.

## 12. CI on kpp.net (thread H, through the coordinator)
- ci.yml runs unchanged where it can (same `runs-on`, `uses:` resolved through the mirrored
  actions, `github.*` is Forgejo's context too, `.github/workflows` is read when there is no
  `.forgejo/workflows`). What changes: the "Publish latest" step and release.yml publish to
  registry.kpp.net (`kpp/releases/`, `kpp/latest.json`) instead of a GitHub release, and the SDL3
  download comes from registry.kpp.net/mirror. factorio-in-kpp's CI downloads `kpp/latest.json`.
- Green run on master on git.kpp.net, then step 11.3.

## Running it
- **Upgrades**: Forgejo 15.x patches: `docker compose pull && docker compose up -d` (monthly, or on
  a security release; subscribe to Forgejo's release feed). The next LTS (v19, due around April
  2027, before 15's support ends on 2027-07-15): read its release notes, take a backup, then change
  the image tag. Caddy: same pull.
  Debian: unattended-upgrades does security updates; reboot when it asks. The runner: Windows
  Update, Visual Studio Build Tools updates through the Visual Studio Installer, the runner binary
  by re-running install-runner.ps1 with a new version.
- **Monitoring**: an external uptime check on https://git.kpp.net and
  https://registry.kpp.net/index/config.json (any free uptime service, mail to kovarex); the
  backup's journal (`journalctl -u kppnet-backup`).
- **Tokens**: the GitHub mirror and migrate tokens expire; when a push mirror shows an error in the
  repository's settings, make a new token and re-run push-mirror.sh.
- **Leaving**: to another server, restore the backup there (step 8.3) and change DNS. Away from
  Forgejo: every repository is a plain git repository (and the GitHub mirror has them all);
  issues and pull requests export through the API. The registry is static files.
