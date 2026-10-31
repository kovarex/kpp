#!/bin/sh
# Makes a GitHub repository a read-only copy of one on git.kpp.net (RUNBOOK.md, step 11): Forgejo
# pushes every branch and tag there on each push and every 8 hours.
#   sh push-mirror.sh kpp/kpp kovarex/kpp
# Needs FORGEJO_TOKEN, GITHUB_USER and GITHUB_MIRROR_TOKEN (a fine-grained GitHub token for that one
# repository with Contents: read and write and Workflows: read and write).
#
# The push is a force push: anything pushed to GitHub directly is overwritten. So before this runs,
# every clone pushes to git.kpp.net (RUNBOOK.md step 10), and GitHub's Issues, Pull requests and
# Actions are switched off for the repository (step 11), with its description saying where the
# original is.
set -eu
. "$(dirname "$0")/forgejo-api.sh"
: "${GITHUB_USER:?export GITHUB_USER first}"
: "${GITHUB_MIRROR_TOKEN:?export GITHUB_MIRROR_TOKEN first}"
repo=$1 github=$2

api POST "/repos/$repo/push_mirrors" "$(cat <<EOF
{"remote_address": "https://github.com/$github.git", "remote_username": "$GITHUB_USER",
 "remote_password": "$GITHUB_MIRROR_TOKEN", "interval": "8h0m0s", "sync_on_commit": true}
EOF
)" >/dev/null
api POST "/repos/$repo/push_mirrors-sync" >/dev/null
echo "$FORGEJO_URL/$repo now pushes to github.com/$github; first sync started"
