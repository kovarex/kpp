#!/bin/sh
# Moves a repository from GitHub to git.kpp.net with its issues, pull requests, labels, milestones
# and releases (RUNBOOK.md, step 10). The copy on git.kpp.net is an ordinary repository, not a
# mirror: from then on it is the original.
#   sh migrate-from-github.sh kovarex/kpp kpp kpp public
#   sh migrate-from-github.sh kovarex/factorio-in-kpp kpp factorio-in-kpp private
# Needs FORGEJO_TOKEN and GITHUB_MIGRATE_TOKEN (a GitHub token that can read the repository, its
# issues and pull requests; for a private repository a classic token with `repo`).
set -eu
. "$(dirname "$0")/forgejo-api.sh"
: "${GITHUB_MIGRATE_TOKEN:?export GITHUB_MIGRATE_TOKEN first}"
src=$1 owner=$2 name=$3 visibility=$4
private=false
[ "$visibility" = private ] && private=true

api POST /repos/migrate "$(cat <<EOF
{"clone_addr": "https://github.com/$src.git", "service": "github", "auth_token": "$GITHUB_MIGRATE_TOKEN",
 "repo_owner": "$owner", "repo_name": "$name", "private": $private, "mirror": false,
 "issues": true, "pull_requests": true, "labels": true, "milestones": true, "releases": true,
 "wiki": true, "lfs": true}
EOF
)" >/dev/null
echo "migrated github.com/$src to $FORGEJO_URL/$owner/$name ($visibility)"
