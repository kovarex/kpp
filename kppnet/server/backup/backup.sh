#!/bin/sh
# Nightly backup of kpp.net (RUNBOOK.md, step 8): a Forgejo dump (database, repositories, LFS,
# attachments, configuration) and the registry's files, into a restic repository off the machine
# (a Hetzner Storage Box over SFTP, or any restic backend). Run by kppnet-backup.timer as root.
# Needs /srv/kppnet/backup/restic.env with RESTIC_REPOSITORY and RESTIC_PASSWORD (chmod 600).
set -eu
cd /srv/kppnet
. /srv/kppnet/backup/restic.env
export RESTIC_REPOSITORY RESTIC_PASSWORD

stamp=$(date -u +%Y%m%dT%H%M%SZ)
dump=/srv/kppnet/backup/forgejo-dump.tar.zst
rm -f "$dump"
# forgejo dump writes inside the container's /data, which is ./data/forgejo here.
docker compose exec -T -u git forgejo mkdir -p /data/tmp
docker compose exec -T -u git forgejo forgejo dump --type tar.zst --file /data/forgejo-dump.tar.zst --tempdir /data/tmp
mv /srv/kppnet/data/forgejo/forgejo-dump.tar.zst "$dump"

# The registry's files are content-addressed and written once, so restic stores each only once.
restic backup --tag "kppnet-$stamp" "$dump" /srv/kppnet/data/registry /srv/kppnet/compose.yaml /srv/kppnet/Caddyfile
restic forget --prune --keep-daily 14 --keep-weekly 8 --keep-monthly 12
# Read back a tenth of the data each night, so a broken backup is found within ten days.
restic check --read-data-subset 10%
rm -f "$dump"
echo "backup $stamp done"
