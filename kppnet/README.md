# kppnet: the deployment kit for kpp.net

kovarex's decision (2026-10-31): official Kpp libraries, and later kpp itself, live in git
repositories owned and hosted on kpp.net, with a forge there for pull requests, issues, review and
CI; GitHub only as a read-only mirror that nothing depends on. In his words: "I prefer to build A
right away." The design is in [docs/library-registry.md](../docs/library-registry.md), "Hosting on
kpp.net"; this directory is what deploys it. **Nothing here has been deployed or run against a
server**; [RUNBOOK.md](RUNBOOK.md) is the procedure, step by step.

| Path | What |
|---|---|
| [RUNBOOK.md](RUNBOOK.md) | the steps, in order, with who does each |
| [server/compose.yaml](server/compose.yaml) | Forgejo 15 LTS (SQLite) and Caddy, the whole server configuration |
| [server/Caddyfile](server/Caddyfile) | TLS for kpp.net, git.kpp.net, registry.kpp.net; the registry's static files |
| [server/env.example](server/env.example) | the secrets the server needs, to fill in as `.env` |
| [server/setup-host.sh](server/setup-host.sh) | prepares a Debian 13 VM: Docker, firewall, updates, the upload user |
| [server/backup/](server/backup) | nightly Forgejo dump plus registry files to restic, systemd timer |
| [runner/windows/](runner/windows) | the Windows CI runner: tools ci.yml needs, runner, registration, service |
| [ci/runner-smoke.yml](ci/runner-smoke.yml) | the runner's acceptance workflow |
| [mirror/](mirror) | moving repositories from GitHub, push mirrors to GitHub, mirrors of the actions CI uses |
| [registry/layout.md](registry/layout.md), [registry/tree_hash.py](registry/tree_hash.py) | the hash-addressed store and the reference tree hash build.lock records |

## What kovarex must provide or do by hand
No thread can do these: they need his accounts, payment or secrets. Prices are list prices found
2026-10-31 on third-party price trackers after Hetzner's 2026 price changes; check them on the
provider's page before ordering.

1. **The domain kpp.net.** Public registration data shows it registered (since 2006-01-03, expires
   2030-01-03, registrar PublicDomainRegistry, DNS at NearlyFreeSpeech.NET, last changed
   2026-10-29); whose account it is in is not public and was not checked. Confirm it is yours,
   lock it, enable the registrar's 2FA, create the DNS records (RUNBOOK.md step 2).
2. **Server**: one Linux VM. Proposed: Hetzner Cloud (Germany or Finland, EU, close to Prague),
   4 vCPU / 8 GB / 160 GB SSD class (CPX32-like), about €11 to €16 a month. Sizing: Forgejo with
   SQLite for tens of users and a few repositories needs 2 GB; the rest is headroom for
   factorio-in-kpp (4.5 GiB of packed history on PC-PRACOVNA, so clones and repacks need memory
   and disk) and for the registry's files. Alternatives: any VPS with Debian 13 (OVH, Scaleway,
   DigitalOcean at roughly twice the price), or a Wube-hosted machine.
3. **Backup storage**: a Hetzner Storage Box BX11 (1 TB) about €3.20 a month plus VAT, or any
   restic target (Backblaze B2, S3, a Wube NAS). A different provider than the server's is safer.
4. **CI runner**: a Windows machine with 8+ cores, 32 GB, 200 GB SSD, doing nothing else (jobs run
   on it without isolation). Options, cheapest first:
   - a machine Wube or kovarex already owns, always on: €0 plus power. Recommended to start.
   - a rented dedicated server (Hetzner AX line, Ryzen 8 to 16 cores) with a Windows Server
     licence: roughly €50 to €110 a month plus the licence; price both when ordering.
   - cloud Windows VMs (Azure, AWS) of that size cost several hundred dollars a month on demand:
     not proposed.
   It needs a Windows Server or Windows 10/11 Pro licence, and Visual Studio Build Tools, whose
   licence terms for building a private repository on a CI machine he should check (Wube's Visual
   Studio subscriptions are the fallback).
5. **Accounts and secrets**, each kept in his password manager:
   - the Forgejo admin account (RUNBOOK.md step 5) with 2FA;
   - a GitHub OAuth app for "Sign in with GitHub" (client id and secret, step 6);
   - the three Forgejo server secrets and the Let's Encrypt e-mail (`.env`, step 4);
   - the restic password (step 8; without it the backups are unreadable);
   - the runner's registration token and the `kpprunner` user's password (step 7);
   - the publish key for the registry store (step 9);
   - GitHub tokens: one to migrate the repositories (step 10), one per mirrored repository
     (step 11); they expire and need renewing;
   - an SMTP account for notification mail (optional at first; `FORGEJO_MAIL_*` in `.env`).
6. **Decisions** (also in docs/library-registry.md):
   1. kpp.net is his and is the name (if not, another domain changes four DNS names here).
   2. Server provider and region (Hetzner proposed).
   3. Runner machine (an owned machine proposed).
   4. Who gets admin rights on the forge besides him.
   5. Whether kpp itself stays private on kpp.net (it is private on GitHub today).
   6. factorio-in-kpp is Factorio's source converted: hosting it on a server outside Wube's
      infrastructure is his call as Wube's owner; it stays private either way.

**Monthly total, as proposed**: about €15 to €20 (server and backups), plus the runner: €0 on an
owned machine, about €60 to €150 rented.
