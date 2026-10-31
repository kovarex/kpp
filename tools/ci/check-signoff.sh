#!/bin/sh
# Every commit of a pull request carries a Developer Certificate of Origin sign-off by its author
# (CONTRIBUTING.md, "Sign-off"): a `Signed-off-by: Name <e-mail>` line whose e-mail is the commit's
# author e-mail. Merge commits are not checked. CI runs it on pull requests (.github/workflows/ci.yml);
# by hand:  sh tools/ci/check-signoff.sh origin/master HEAD
set -eu
base=$1
head=$2
bad=0
count=0
for commit in $(git rev-list --no-merges "$base..$head"); do
  count=$((count + 1))
  email=$(git log -1 --format=%ae "$commit" | tr '[:upper:]' '[:lower:]')
  if git log -1 --format=%B "$commit" | tr '[:upper:]' '[:lower:]' | grep -q "^signed-off-by: .* <$email>[[:space:]]*\$"; then
    continue
  fi
  echo "no sign-off by its author ($email): $(git log -1 --format='%h %s' "$commit")"
  bad=$((bad + 1))
done
if [ "$bad" -ne 0 ]; then
  echo "$bad of $count commits lack a Signed-off-by line with the author's e-mail."
  echo "Add it with: git rebase --signoff $base   (see CONTRIBUTING.md)"
  exit 1
fi
echo "all $count commits are signed off by their authors"
