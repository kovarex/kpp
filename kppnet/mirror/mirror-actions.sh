#!/bin/sh
# Pull mirrors of the actions kpp's workflows use, in the organization `actions` on git.kpp.net
# (RUNBOOK.md, step 7). With DEFAULT_ACTIONS_URL=self (server/compose.yaml), `uses:
# actions/checkout@v4` in a workflow resolves to git.kpp.net/actions/checkout, so the same
# ci.yml runs on GitHub and here, and CI here keeps working when github.com is down: a mirror keeps
# its last copy. Run once; Forgejo refreshes each mirror every 8 hours.
# Needs FORGEJO_TOKEN.
set -eu
. "$(dirname "$0")/forgejo-api.sh"

api POST /orgs '{"username": "actions", "visibility": "public", "description": "Mirrors of the GitHub Actions kpp workflows use"}' >/dev/null 2>&1 || true

# One per line: GitHub repository. Add a line when a workflow starts using another action.
while read -r src; do
  [ -z "$src" ] && continue
  name=${src#*/}
  owner=${src%/*}
  if [ "$owner" != actions ]; then
    # Actions of other owners need the same owner on git.kpp.net to keep `uses: owner/name@v`.
    api POST /orgs "{\"username\": \"$owner\", \"visibility\": \"public\"}" >/dev/null 2>&1 || true
  fi
  api POST /repos/migrate "{\"clone_addr\": \"https://github.com/$src.git\", \"repo_owner\": \"$owner\", \"repo_name\": \"$name\", \"mirror\": true, \"mirror_interval\": \"8h0m0s\", \"private\": false}" >/dev/null \
    && echo "mirrored $src" || echo "skipped $src (exists already?)"
done <<EOF
actions/checkout
actions/cache
actions/setup-node
EOF
