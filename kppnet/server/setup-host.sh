#!/bin/sh
# Prepares a fresh Debian 13 VM for kpp.net (RUNBOOK.md, step 3). Run once as root:
#   sh setup-host.sh
# It installs Docker from Docker's own repository, a firewall, automatic security updates, restic
# and sqlite3 for backups, and the `registry` user that the publish job uploads archives as
# (write-only into /srv/kppnet/data/registry through rrsync). It does not start anything: that is
# `docker compose up -d` in /srv/kppnet, after .env is filled in.
set -eu

apt-get update
apt-get -y upgrade
apt-get -y install ca-certificates curl gnupg ufw unattended-upgrades restic sqlite3 rsync

# Docker Engine and the compose plugin (https://docs.docker.com/engine/install/debian/).
install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/debian/gpg -o /etc/apt/keyrings/docker.asc
chmod a+r /etc/apt/keyrings/docker.asc
. /etc/os-release
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/debian $VERSION_CODENAME stable" \
  > /etc/apt/sources.list.d/docker.list
apt-get update
apt-get -y install docker-ce docker-ce-cli containerd.io docker-compose-plugin

# Firewall: admin SSH 22, git SSH 2222, HTTP and HTTPS (443/udp for HTTP/3).
ufw default deny incoming
ufw default allow outgoing
ufw allow 22/tcp
ufw allow 2222/tcp
ufw allow 80/tcp
ufw allow 443/tcp
ufw allow 443/udp
ufw --force enable

dpkg-reconfigure -f noninteractive unattended-upgrades

# The data directories. Forgejo runs as uid 1000 inside its container.
mkdir -p /srv/kppnet/data/forgejo /srv/kppnet/data/caddy /srv/kppnet/data/registry /srv/kppnet/backup
chown -R 1000:1000 /srv/kppnet/data/forgejo

# The upload user: its authorized key (the publish job's, RUNBOOK.md step 9) is restricted to
# rrsync into the registry directory, so it can add files there and do nothing else.
id registry >/dev/null 2>&1 || useradd --create-home --shell /bin/sh registry
chown -R registry:registry /srv/kppnet/data/registry
install -d -m 700 -o registry -g registry /home/registry/.ssh
touch /home/registry/.ssh/authorized_keys
chown registry:registry /home/registry/.ssh/authorized_keys
chmod 600 /home/registry/.ssh/authorized_keys
RRSYNC=$(command -v rrsync || echo /usr/share/doc/rsync/scripts/rrsync)
echo "rrsync is at $RRSYNC; authorized_keys lines for the registry user must start with:"
echo "  command=\"$RRSYNC -wo /srv/kppnet/data/registry\",restrict ssh-ed25519 AAAA... publish@kpp.net"
echo "Done. Next: copy compose.yaml, Caddyfile and .env to /srv/kppnet (RUNBOOK.md, step 4)."
