#!/usr/bin/env bash
# Launcher selection/argument/environment regressions, with a fake compiler.
# Real bootstrap and staged compiler builds are separate integration tests.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
mkdir -p "$root/.cache"
work=$(mktemp -d "$root/.cache/bootstrap-test.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
fixture="$work/checkout with spaces"
mkdir -p "$fixture/tools" "$fixture/backstop/v1" "$work/seed one/lib/include" "$work/seed one/lib/libc"
cp "$root/build.sh" "$fixture/build.sh"
printf '{}\n' > "$work/seed one/upstream.lock.json"
for tool in build release linux-release-deps macos-x64-llvm; do : > "$fixture/tools/$tool.c"; done
cat > "$work/seed one/cross_cc" <<'MOCK'
#!/bin/sh
set -eu
[ "$1" = cc ]
[ "$CROSS_CC_ROOT" = "$EXPECT_ROOT" ]
[ "$CROSS_CC_LIB_DIR" = "$EXPECT_ROOT/lib" ]
[ -z "${CPATH:-}" ] && [ -z "${CPLUS_INCLUDE_PATH:-}" ]
out=
while [ "$#" -gt 0 ]; do
    if [ "$1" = -o ]; then shift; out=$1; fi
    shift
done
[ -n "$out" ]
cat > "$out" <<'TOOL'
#!/bin/sh
printf 'mock tool:'
printf ' <%s>' "$@"
printf '\n'
TOOL
chmod +x "$out"
MOCK
chmod +x "$work/seed one/cross_cc"
export EXPECT_ROOT="$work/seed one"
export CPATH=must-not-leak CPLUS_INCLUDE_PATH=must-not-leak
seed="$work/seed one/cross_cc"
export CROSS_CC_COMPILER="$seed"
out=$("$fixture/build.sh" test '--profile=debug')
[[ $out == *"mock tool: <test> <--profile=debug> <--compiler=$seed>"* ]]
out=$("$fixture/build.sh")
[[ $out == *"mock tool: <build> <--compiler=$seed>"* ]]
# Explicit selection beats a broken environment selection; symlinks resolve.
export CROSS_CC_COMPILER="$work/does-not-exist"
ln -s "$seed" "$work/selected"
out=$("$fixture/build.sh" --compiler="$work/selected" '--profile=release')
[[ $out == *"mock tool: <build> <--profile=release> <--compiler=$seed>"* ]]
# A relative explicit compiler is relative to the caller, not the checkout.
out=$(cd "$work" && "$fixture/build.sh" '--compiler=seed one/cross_cc' help)
[[ $out == *"mock tool: <help> <--compiler=$seed>"* ]]
unset CROSS_CC_COMPILER
out=$(PATH="$work/seed one:$PATH" "$fixture/build.sh" tools)
[[ $out == *"mock tool: <tools> <--compiler=$seed>"* ]]
# A bad explicit compiler must fail, not silently fall back to v1 or PATH.
if "$fixture/build.sh" --compiler="$work/missing" > "$work/failure" 2>&1; then
    echo 'bad explicit compiler unexpectedly succeeded' >&2; exit 1
fi
grep -q 'compiler is not executable' "$work/failure"
# Hash verification happens before extracting or compiling a fallback archive.
if [[ $(uname -s) == Linux && $(uname -m) == x86_64 ]]; then
    printf 'not a zip\n' > "$fixture/backstop/v1/cross_cc-v1-linux-amd64.zip"
    printf '%064d  cross_cc-v1-linux-amd64.zip\n' 0 > "$fixture/backstop/v1/SHA256SUMS"
    # The ordinary development PATH must not already include cross_cc here.
    if ! command -v cross_cc >/dev/null 2>&1; then
        if "$fixture/build.sh" > "$work/failure" 2>&1; then
            echo 'corrupt backstop unexpectedly succeeded' >&2; exit 1
        fi
        grep -q 'backstop archive checksum mismatch' "$work/failure"
        [[ ! -e $fixture/.cache/bootstrap/v1/linux-amd64 ]]
    fi
fi
printf 'bootstrap launcher tests: PASS\n'
