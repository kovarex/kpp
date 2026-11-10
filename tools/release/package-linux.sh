#!/bin/sh
# Packages a kpp built on Linux as kpp's Linux release (docs/build.md, "Installing"):
# build/release/kpp-<version>-linux-x86_64/ with kpp (compiler/build/kpp under the name users type),
# LICENSE.txt, stdlib/, projectlib/, libraries/ (the official libraries: each one's tracked files) and
# README.txt; then build/release/kpp-<version>-linux-x86_64.tar.gz. The version and commit are kpp's
# own (kpp --version), never given by hand. No toolchain inside: a Linux system's clang 19 and lld
# build and link (KPP_CLANG, else the clang on PATH; docs/design.md, "Stage 5, kpp on a Linux host").
# Its smoke test unpacks the tarball outside the repository and runs, there, kpp new and kpp --run
# (`Hello from kpp`), a project that uses kpp's own json library, and kpp --version (the package's
# stdlib and libraries named).
#   sh tools/release/package-linux.sh
set -eu
repository=$(cd "$(dirname "$0")/../.." && pwd)
compiler="$repository/compiler/build/kpp"
printed=$("$compiler" --version | head -n 1)
version=$(echo "$printed" | sed -n 's/^kpp \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\(+dirty\)\{0,1\}\), commit \([0-9a-f]\{40\}\)$/\1/p')
commit=$(echo "$printed" | sed -n 's/^kpp .*, commit \([0-9a-f]\{40\}\)$/\1/p')
[ -n "$version" ] && [ -n "$commit" ] || { echo "compiler/build/kpp has no version to package (kpp --version: $printed)"; exit 1; }
name="kpp-$version-linux-x86_64"
out="$repository/build/release"
kpp="$out/$name"
echo "packaging kpp $version, commit $commit"
rm -rf "$kpp" "$out/$name.tar.gz"
mkdir -p "$kpp"
cp "$compiler" "$kpp/kpp"
strip "$kpp/kpp" 2>/dev/null || true
# Apache 2.0's section 4(a): every copy carries the license.
cp "$repository/LICENSE" "$kpp/LICENSE.txt"
cp -r "$repository/stdlib" "$kpp/stdlib"
cp -r "$repository/projectlib" "$kpp/projectlib"
# The official libraries: each one's tracked files (its tests and licence texts with it), no build/.
libraries=""
for dir in "$repository"/libraries/*/; do
  library=$(basename "$dir")
  [ -f "$dir/build.kpp" ] || continue
  git -C "$repository" ls-files -- "libraries/$library" | while read -r file; do
    mkdir -p "$kpp/$(dirname "$file")"
    cp "$repository/$file" "$kpp/$file"
  done
  libraries="$libraries${libraries:+, }$library"
done
cp "$repository/libraries/README.md" "$kpp/libraries/"
cat > "$kpp/README.txt" <<EOF
kpp $version for Linux x86-64 (commit $commit)

kpp builds kpp programs: \`kpp\` in a project's directory (its build.kpp), \`kpp new\` makes one,
\`kpp --run\` builds and runs it, \`kpp --test\` runs its tests. kpp needs clang 19 or later and lld
(Debian and Ubuntu: apt install clang-19 lld-19; then KPP_CLANG=/usr/bin/clang-19, or clang 19 as
the clang on PATH). Put this directory on PATH, or run ./kpp from it.
Documentation: https://github.com/kovarex/kpp/blob/master/docs/build.md

The official libraries are in libraries/ ($libraries; libraries/README.md): use("json") finds them
there when a project has no copy of its own.

Not yet on Linux: the compile server (each build checks everything again) and the language server.

kpp and its libraries are licensed under the Apache License 2.0 with LLVM Exceptions: LICENSE.txt.
Third-party code keeps its own licence, its text beside it.
EOF
tar -C "$out" -czf "$out/$name.tar.gz" "$name"
echo "built $out/$name.tar.gz ($(du -h "$out/$name.tar.gz" | cut -f1))"

# The smoke test, outside the repository, from the tarball.
smoke=$(mktemp -d)
trap 'rm -rf "$smoke"' EXIT
tar -C "$smoke" -xzf "$out/$name.tar.gz"
installed="$smoke/$name/kpp"
mkdir "$smoke/hello" && cd "$smoke/hello"
"$installed" new > /dev/null
run=$("$installed" --run | tail -n 1)
[ "$run" = "Hello from kpp" ] || { echo "the packaged kpp did not build a new project: '$run'"; exit 1; }
echo "smoke test: kpp new and kpp --run: $run"
mkdir -p "$smoke/json/src" && cd "$smoke/json"
printf 'void build(Project& project)\n{\n  project.executable("app", {"src"});\n  project.use("json");\n}\n' > build.kpp
printf 'int main()\n{\n  std::println(json::parseJson("[1, 2]").size());\n  return 0;\n}\n' > src/main.kpp
run=$("$installed" --run | tail -n 1)
[ "$run" = "2" ] || { echo "the packaged kpp did not build a project using its json library: '$run'"; exit 1; }
echo "smoke test: a project using json: $run"
printed=$("$installed" --version)
echo "$printed"
echo "$printed" | grep -q "^kpp $version" && echo "$printed" | grep -q "^stdlib: $smoke/$name/stdlib\$" &&
  echo "$printed" | grep -q "^libraries: $smoke/$name/libraries ($libraries)\$" || { echo "kpp --version names another stdlib or libraries"; exit 1; }
echo "package-linux: ALL PASSED ($name.tar.gz)"
