#!/bin/sh
# kpp-deploy.sh: installs a kpp web app's release on its server, run by hand there (kovarex,
# 2026-11-04: "one simple command on the prod server to pull the new version and download the new
# binary"). POSIX sh with FreeBSD's base tools (fetch, sha256, tar, stat); Linux's (curl,
# sha256sum, GNU stat) when those are missing. docs/build.md, "Deploying a kpp web app".
#
#   kpp-deploy.sh [VERSION]       install VERSION (a release's tag; the latest release without one)
#   kpp-deploy.sh --from FILE     install a package file (FILE.sha256 beside it) instead
#   kpp-deploy.sh --migrate ...   also run the package's database migrations that are not applied
#   kpp-deploy.sh --rollback      put back the program and static files the last install replaced
#   kpp-deploy.sh --mark-applied NAME...   record migrations applied by hand (no SQL is run)
#   kpp-deploy.sh --status        the installed version and the migrations applied
#
# What it does, in order, stopping at the first failure with what to do:
#  1. downloads the release's package and its .sha256 from GitHub, and checks the SHA-256;
#  2. unpacks it into STATE/releases/VERSION and checks that its MANIFEST names this APP;
#  3. compares the package's migrations with the deploy_migration table: pending ones stop the
#     install (shown, never run silently) unless --migrate runs them, in order, recording each;
#  4. checks the configuration's permissions (CONFIG_MODE, group CONFIG_GROUP); never reads or
#     changes its contents;
#  5. copies the static files into PUBLIC, then the program as PROGRAM.new (mode 755), keeps the
#     current one as PROGRAM.previous, and renames PROGRAM.new to PROGRAM (one atomic step);
#  6. runs each SMOKE path through the program as the web server runs it; one that does not
#     answer 200 puts the previous version back and prints what the program said.

# --- The app (gorating's; another app's own values here, or in DEPLOY_CONF) ---
APP=gorating
REPOSITORY=kovarex/gorating
ASSET=gorating-site.tar.gz
PUBLIC=/home/public
PROGRAM=gorating.cgi
CONFIG=/home/protected/gorating-config.json
CONFIG_ENV=GORATING_CONFIG
CONFIG_MODE=-rw-r-----
CONFIG_GROUP=web
STATE=/home/protected/deploy
# The database client for migrations: a defaults file with the user, password, host and database.
DATABASE="mysql --defaults-extra-file=/home/protected/deploy/my.cnf"
SMOKE="/about /players"
KEEP=3 # releases kept in STATE/releases
# ---

[ -n "$DEPLOY_CONF" ] && . "$DEPLOY_CONF"
set -u

fail() { echo "deploy: $*" >&2; exit 1; }
say() { echo "deploy: $*"; }

download() # URL FILE
{
  if command -v fetch > /dev/null 2>&1; then fetch -q -o "$2" "$1"; else curl -fsSL -o "$2" "$1"; fi
}
sha256of() # FILE
{
  if command -v sha256 > /dev/null 2>&1; then sha256 -q "$1"; else sha256sum "$1" | cut -d' ' -f1; fi
}
modeof() # FILE: "-rw-r----- group"
{
  if stat -f '%Sp %Sg' "$1" > /dev/null 2>&1; then stat -f '%Sp %Sg' "$1"; else stat -c '%A %G' "$1"; fi
}
sql() # STATEMENT: its rows, tab separated, no header
{
  echo "$1" | $DATABASE -N -B
}

applied_migrations()
{
  sql "SELECT name FROM deploy_migration ORDER BY name" 2> /dev/null || fail "no deploy_migration table: run $0 --mark-applied with the migrations applied by hand (none: --mark-applied alone)"
}

ensure_table()
{
  sql "CREATE TABLE IF NOT EXISTS deploy_migration (name VARCHAR(255) NOT NULL PRIMARY KEY, applied TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP)" ||
    fail "cannot make the deploy_migration table ($DATABASE)"
}

lock()
{
  mkdir -p "$STATE" || fail "cannot make $STATE"
  mkdir "$STATE/lock" 2> /dev/null || fail "another deploy runs, or one stopped half way: remove $STATE/lock when none runs"
  trap 'rmdir "$STATE/lock" 2> /dev/null' EXIT INT TERM
}

# The static files of SITE (a release's unpacked public/), all but the program, into PUBLIC.
copy_static() # SITE
{
  (cd "$1" && find . -type f ! -path "./$PROGRAM") | while read -r f; do
    mkdir -p "$PUBLIC/$(dirname "$f")" && cp "$1/$f" "$PUBLIC/$f" || exit 1
  done || fail "copying the static files into $PUBLIC failed"
}

install_program() # FILE
{
  cp "$1" "$PUBLIC/$PROGRAM.new" && chmod 755 "$PUBLIC/$PROGRAM.new" || fail "cannot write $PUBLIC/$PROGRAM.new"
  if [ -f "$PUBLIC/$PROGRAM" ]; then
    cp -p "$PUBLIC/$PROGRAM" "$PUBLIC/$PROGRAM.previous" || fail "cannot keep $PUBLIC/$PROGRAM.previous"
  fi
  mv -f "$PUBLIC/$PROGRAM.new" "$PUBLIC/$PROGRAM" || fail "cannot rename $PROGRAM.new to $PROGRAM"
}

# Each SMOKE path through the program as Apache runs it (CGI); prints the failing one's output.
smoke()
{
  for path in $SMOKE; do
    out=$(cd "$PUBLIC" && env REQUEST_METHOD=GET REQUEST_URI="$path" "$CONFIG_ENV=$CONFIG" "./$PROGRAM" 2>&1)
    case "$out" in
      "Status: 200"*) say "smoke test $path: 200" ;;
      *) echo "$out" | head -n 30 >&2; return 1 ;;
    esac
  done
}

# The release before the current one back: its program (PROGRAM.previous) and static files.
rollback()
{
  [ -f "$PUBLIC/$PROGRAM.previous" ] || fail "no $PUBLIC/$PROGRAM.previous to go back to"
  previous=$(cat "$STATE/previous" 2> /dev/null || true)
  [ -n "$previous" ] && [ -d "$STATE/releases/$previous/site/public" ] && copy_static "$STATE/releases/$previous/site/public"
  current=$(cat "$STATE/current" 2> /dev/null || true)
  cp -p "$PUBLIC/$PROGRAM" "$PUBLIC/$PROGRAM.failed" 2> /dev/null || true
  mv -f "$PUBLIC/$PROGRAM.previous" "$PUBLIC/$PROGRAM" || fail "cannot put $PROGRAM.previous back"
  [ -n "$previous" ] && echo "$previous" > "$STATE/current"
  [ -n "$current" ] && echo "$current" > "$STATE/previous"
  say "rolled back to ${previous:-the previous program} (the replaced one is $PROGRAM.failed; the database is as it was)"
}

migrate=no
from=""
version=""
while [ $# -gt 0 ]; do
  case "$1" in
    --migrate) migrate=yes ;;
    --from) shift; from=$1 ;;
    --rollback) lock; rollback; exit 0 ;;
    --status)
      echo "installed: $(cat "$STATE/current" 2> /dev/null || echo none) (before it: $(cat "$STATE/previous" 2> /dev/null || echo none))"
      echo "migrations applied:"; applied_migrations | sed 's/^/  /'
      exit 0 ;;
    --mark-applied)
      shift; ensure_table
      for name in "$@"; do sql "INSERT IGNORE INTO deploy_migration(name) VALUES ('$(echo "$name" | sed "s/'/''/g")')" || fail "cannot record $name"; say "recorded $name as applied"; done
      exit 0 ;;
    -*) fail "unknown option $1 (see the head of $0)" ;;
    *) version=$1 ;;
  esac
  shift
done

lock
mkdir -p "$STATE/releases" || fail "cannot make $STATE/releases"

# 1. The package and its SHA-256.
if [ -n "$from" ]; then
  [ -f "$from" ] && [ -f "$from.sha256" ] || fail "--from needs $from and $from.sha256"
  version=${version:-local-$(sha256of "$from" | cut -c1-12)}
  dir=$STATE/releases/$version
  rm -rf "$dir" && mkdir -p "$dir" && cp "$from" "$dir/$ASSET" && cp "$from.sha256" "$dir/$ASSET.sha256" || fail "cannot copy $from"
else
  if [ -z "$version" ]; then
    download "https://api.github.com/repos/$REPOSITORY/releases/latest" "$STATE/latest.json" || fail "cannot ask GitHub for $REPOSITORY's latest release"
    version=$(sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' "$STATE/latest.json" | head -n 1)
    [ -n "$version" ] || fail "GitHub names no latest release of $REPOSITORY"
  fi
  dir=$STATE/releases/$version
  rm -rf "$dir" && mkdir -p "$dir" || fail "cannot make $dir"
  base="https://github.com/$REPOSITORY/releases/download/$version"
  download "$base/$ASSET" "$dir/$ASSET" && download "$base/$ASSET.sha256" "$dir/$ASSET.sha256" || fail "cannot download $version's $ASSET"
fi
expected=$(cut -d' ' -f1 < "$dir/$ASSET.sha256")
got=$(sha256of "$dir/$ASSET")
[ "$got" = "$expected" ] || fail "$ASSET has SHA-256 $got, not the $expected its .sha256 says: nothing installed"
say "$version: $ASSET, SHA-256 $got"

# 2. Unpacked, and its manifest.
mkdir -p "$dir/site" && tar -xzf "$dir/$ASSET" -C "$dir/site" --strip-components 1 || fail "cannot unpack $ASSET"
grep -q "^app $APP\$" "$dir/site/MANIFEST" 2> /dev/null || fail "$ASSET's MANIFEST does not name the app $APP: nothing installed"
[ -f "$dir/site/public/$PROGRAM" ] || fail "$ASSET has no public/$PROGRAM"

# 3. Migrations.
pending=""
applied=$(applied_migrations) || exit 1
for name in $(sed -n 's/^migration //p' "$dir/site/MANIFEST"); do
  echo "$applied" | grep -qx "$name" || pending="$pending $name"
done
if [ -n "$pending" ]; then
  if [ "$migrate" != yes ]; then
    echo "deploy: $version needs these migrations, not applied yet:" >&2
    for name in $pending; do echo "  $dir/site/migrations/$name" >&2; done
    fail "back up the database, then run again with --migrate (or apply them by hand and --mark-applied them): nothing installed"
  fi
  for name in $pending; do
    $DATABASE < "$dir/site/migrations/$name" || fail "the migration $name failed: the database may be half changed; the site is as it was"
    sql "INSERT INTO deploy_migration(name) VALUES ('$name')" || fail "$name ran but could not be recorded: --mark-applied $name"
    say "migration $name applied"
  done
fi

# 4. The configuration's permissions.
[ -f "$CONFIG" ] || fail "no $CONFIG: the site's configuration (DEPLOY.md)"
mode=$(modeof "$CONFIG")
[ "$mode" = "$CONFIG_MODE $CONFIG_GROUP" ] || fail "$CONFIG is $mode, not $CONFIG_MODE $CONFIG_GROUP: chgrp $CONFIG_GROUP $CONFIG; chmod 640 $CONFIG"

# 5. Installed.
copy_static "$dir/site/public"
install_program "$dir/site/public/$PROGRAM"
[ -f "$STATE/current" ] && cp "$STATE/current" "$STATE/previous"
echo "$version" > "$STATE/current"
say "installed $version"

# 6. Smoke tests; a failure goes back.
if ! smoke; then
  rollback
  fail "$version failed its smoke test (above): rolled back"
fi

# Old releases go, the newest KEEP and the current and previous stay.
keep_previous=$(cat "$STATE/previous" 2> /dev/null || true)
ls -t "$STATE/releases" | tail -n +$((KEEP + 1)) | while read -r old; do
  [ "$old" = "$version" ] || [ "$old" = "$keep_previous" ] || rm -rf "$STATE/releases/$old"
done
say "done: $version"
