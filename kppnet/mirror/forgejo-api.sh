#!/bin/sh
# Shared by the mirror scripts: a call to git.kpp.net's API with an admin token.
# Needs FORGEJO_TOKEN (git.kpp.net > Settings > Applications > Generate token, scopes
# write:repository, write:organization, write:admin; kovarex makes it and exports it in his shell).
: "${FORGEJO_URL:=https://git.kpp.net}"
: "${FORGEJO_TOKEN:?export FORGEJO_TOKEN first (see the comment at the top of forgejo-api.sh)}"

# api METHOD PATH [JSON]: prints the response; fails on an HTTP error.
api() {
  if [ $# -ge 3 ]; then
    curl -fsS -X "$1" -H "Authorization: token $FORGEJO_TOKEN" -H "Content-Type: application/json" \
      --data "$3" "$FORGEJO_URL/api/v1$2"
  else
    curl -fsS -X "$1" -H "Authorization: token $FORGEJO_TOKEN" "$FORGEJO_URL/api/v1$2"
  fi
}
