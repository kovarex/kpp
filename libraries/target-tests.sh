#!/bin/sh
# Runs the test programs target-tests.ps1 built (the bundle: this script, NAME.elf, projects.txt) on
# FreeBSD or Linux: each from its project's directory in the checkout ROOT (its tests read files
# there), at most 120 s and 4 GB of address space, its output in NAME.log. The last line says ALL
# PASSED or which failed.
#   sh run.sh ROOT        (HTTPCLIENT_TEST_HTTPS=1 adds httpclient's HTTPS test: EGD over TLS)
root=$1
dir=$(cd "$(dirname "$0")" && pwd)
target=$(uname -s | tr 'A-Z' 'a-z')
passed=0 tests=0 failed=""
while read -r path name; do
  [ -n "$name" ] || continue
  cp "$dir/$name.elf" "/tmp/kpp-target-test-$name" && chmod +x "/tmp/kpp-target-test-$name"
  # 4 GB of address space and 120 s each (H's review): a runaway allocation took a VM down in run
  # 41638108414, and a few hangs must not use up the job's time before its summary.
  (cd "$root/$path" && ulimit -v 4194304 && exec timeout 120 "/tmp/kpp-target-test-$name") > "$dir/$name.log" 2>&1
  code=$?
  rm -f "/tmp/kpp-target-test-$name"
  last=$(grep -E '^[0-9]+ tests?, [0-9]+ failed$' "$dir/$name.log" | tail -n 1)
  count=$(echo "$last" | sed -E 's/^([0-9]+) .*/\1/')
  if [ $code -eq 0 ] && [ -n "$last" ] && echo "$last" | grep -q ', 0 failed$'; then
    echo "ok   $name: $last"
    passed=$((passed + 1)) tests=$((tests + count))
  else
    echo "FAIL $name: exit $code, ${last:-no summary line}"
    sed 's/^/    /' "$dir/$name.log" | tail -n 20
    failed="$failed $name"
  fi
done < "$dir/projects.txt"
if [ -n "$failed" ]; then
  echo "libraries $target: FAILED:$failed ($passed passed, $tests tests)"
  exit 1
fi
echo "libraries $target: ALL PASSED ($passed programs, $tests tests)"
