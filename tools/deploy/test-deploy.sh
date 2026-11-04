#!/bin/sh
# test-deploy.sh PACKAGE DATABASE: kpp-deploy.sh against a package (PACKAGE and PACKAGE.sha256) in a
# made-up server: PUBLIC, STATE and the configuration under a new directory, DATABASE the client
# command of a scratch database (its migrations' table is dropped first). Checks: an install refused
# without the migrations' table and with pending migrations, then --migrate; the program installed
# with mode 755 and its smoke tests; a broken program put back by itself; --rollback. The last line
# says ALL PASSED or what failed. gorating's CI runs it in its FreeBSD VM; it runs on Linux too.
#   sh test-deploy.sh gorating-site.tar.gz "mysql -h 127.0.0.1 -u gorating -pX gorating_kpp_tests"
# The package's configuration comes from GORATING_TEST_CONFIG (a file), copied in.
package=$1
database=$2
here=$(cd "$(dirname "$0")" && pwd)
root=$(mktemp -d)
failed=0
check() # WHAT COMMAND...: the command must succeed
{
  what=$1; shift
  if "$@" > "$root/out.txt" 2>&1; then echo "ok   $what"; else echo "FAIL $what:"; sed 's/^/    /' "$root/out.txt"; failed=$((failed + 1)); fi
}
refused() # WHAT TEXT COMMAND...: the command must fail saying TEXT
{
  what=$1; text=$2; shift 2
  if "$@" > "$root/out.txt" 2>&1; then echo "FAIL $what: it succeeded"; sed 's/^/    /' "$root/out.txt"; failed=$((failed + 1))
  elif grep -q "$text" "$root/out.txt"; then echo "ok   $what"
  else echo "FAIL $what: no \"$text\" in:"; sed 's/^/    /' "$root/out.txt"; failed=$((failed + 1)); fi
}

mkdir -p "$root/public" "$root/state"
cp "${GORATING_TEST_CONFIG:?the site configuration to test with}" "$root/config.json"
chmod 640 "$root/config.json"
group=$(id -gn)
cat > "$root/deploy.conf" <<EOF
PUBLIC=$root/public
STATE=$root/state
CONFIG=$root/config.json
CONFIG_GROUP=$group
DATABASE="$database"
EOF
export DEPLOY_CONF="$root/deploy.conf"
echo "DROP TABLE IF EXISTS deploy_migration" | $database || { echo "FAIL the scratch database ($database)"; exit 1; }
deploy="sh $here/kpp-deploy.sh"

refused "no migrations' table" "no deploy_migration table" $deploy --from "$package" v1
check "--mark-applied, none" $deploy --mark-applied
refused "pending migrations" "needs these migrations" $deploy --from "$package" v1
[ ! -f "$root/public/gorating.cgi" ] && echo "ok   nothing installed while refused" || { echo "FAIL a refused install wrote the program"; failed=$((failed + 1)); }
check "--migrate and install v1" $deploy --migrate --from "$package" v1
mode=$(ls -l "$root/public/gorating.cgi" | cut -c1-10)
[ "$mode" = "-rwxr-xr-x" ] && echo "ok   the program's mode" || { echo "FAIL the program is $mode"; failed=$((failed + 1)); }
[ -f "$root/public/.htaccess" ] && [ -f "$root/public/style2.css" ] && echo "ok   the static files" || { echo "FAIL static files missing"; failed=$((failed + 1)); }
check "a second install, nothing pending" $deploy --from "$package" v2
check "--status" $deploy --status

# --rollback: v1's program back; then v2 again.
check "--rollback" $deploy --rollback
[ -f "$root/public/gorating.cgi.failed" ] && [ "$(cat "$root/state/current")" = "v1" ] && echo "ok   v1 current, the replaced program kept as .failed" ||
  { echo "FAIL after --rollback: current $(cat "$root/state/current")"; failed=$((failed + 1)); }
check "v2 again" $deploy --from "$package" v2

# A broken program: its smoke test fails and v2 comes back.
mkdir "$root/broken" && tar -xzf "$package" -C "$root/broken"
printf '#!/bin/sh\necho "Status: 500 Internal Server Error"\necho\n' > "$root/broken/gorating-site/public/gorating.cgi"
(cd "$root/broken" && tar -czf broken.tar.gz gorating-site)
if command -v sha256 > /dev/null 2>&1; then sha256 -q "$root/broken/broken.tar.gz"; else sha256sum "$root/broken/broken.tar.gz" | cut -d' ' -f1; fi > "$root/broken/broken.tar.gz.sha256"
refused "a broken program is rolled back" "failed its smoke test" $deploy --from "$root/broken/broken.tar.gz" v3
cmp -s "$root/public/gorating.cgi" "$root/state/releases/v2/site/public/gorating.cgi" && echo "ok   v2's program back after v3 failed" || { echo "FAIL the program is not v2's"; failed=$((failed + 1)); }
[ "$(cat "$root/state/current")" = "v2" ] && echo "ok   v2 current" || { echo "FAIL current is $(cat "$root/state/current")"; failed=$((failed + 1)); }

# A wrong SHA-256 installs nothing.
cp "$package" "$root/bad.tar.gz" && echo 0000 > "$root/bad.tar.gz.sha256"
refused "a wrong SHA-256" "not the" $deploy --from "$root/bad.tar.gz" v4

rm -rf "$root"
if [ $failed -ne 0 ]; then echo "test-deploy: $failed FAILED"; exit 1; fi
echo "test-deploy: ALL PASSED"
