#!/usr/bin/env bash
# Only the first compiler selection/extraction lives here. All build logic is C.
set -euo pipefail
fail() { printf 'build: %s\n' "$*" >&2; exit 1; }
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
caller=$PWD
compiler=${CROSS_CC_COMPILER:-}
args=()
for arg in "$@"; do
    case "$arg" in
        --compiler=*) compiler=${arg#--compiler=}; [[ -n $compiler ]] || fail '--compiler needs an executable' ;;
        --compiler) fail 'use --compiler=/path/to/cross_cc' ;;
        *) args+=("$arg") ;;
    esac
done
hash_file() {
    local result
    if command -v sha256sum >/dev/null 2>&1; then
        result=$(sha256sum -- "$1") || return
    elif command -v shasum >/dev/null 2>&1; then
        result=$(shasum -a 256 -- "$1") || return
    else
        fail 'SHA-256 verification needs sha256sum or shasum'
    fi
    printf '%s\n' "${result%% *}"
}
resolve_executable() {
    local path=$1 dir link i
    if [[ $path != */* ]]; then
        path=$(command -v "$path") || fail "compiler not found: $1"
    elif [[ $path != /* ]]; then
        path=$caller/$path
    fi
    for ((i=0; i<40; i++)); do
        dir=$(CDPATH= cd -- "$(dirname -- "$path")" && pwd -P) || fail "invalid compiler path: $path"
        path=$dir/$(basename -- "$path")
        [[ -L $path ]] || { printf '%s\n' "$path"; return; }
        link=$(readlink -- "$path")
        if [[ $link == /* ]]; then path=$link; else path=$dir/$link; fi
    done
    fail 'compiler symlink chain is too long'
}
if [[ -z $compiler ]]; then
    compiler=$(command -v cross_cc || true)
fi
if [[ -z $compiler ]]; then
    case $(uname -s) in Linux) os=linux ;; Darwin) os=darwin ;; *) fail 'supply --compiler= on this host' ;; esac
    case $(uname -m) in x86_64|amd64) arch=amd64 ;; arm64|aarch64) arch=arm64 ;; *) fail 'unsupported host architecture' ;; esac
    host=$os-$arch
    name=cross_cc-v1-$host.zip
    archive=$root/backstop/v1/$name
    [[ -f $archive ]] || fail "no v1 backstop for $host; supply --compiler="
    expected=
    while read -r digest filename; do
        if [[ $filename == "$name" ]]; then expected=$digest; fi
    done < "$root/backstop/v1/SHA256SUMS"
    [[ ${#expected} == 64 ]] || fail "missing archive checksum: $name"
    [[ $(hash_file "$archive") == "$expected" ]] || fail "backstop archive checksum mismatch: $name"
    cache=$root/.cache/bootstrap/v1/$host
    compiler=$cache/cross_cc-$host/cross_cc
    mkdir -p -- "$root/.cache/bootstrap/v1"
    if [[ -e $cache ]]; then
        [[ -f $cache/archive.sha256 && $(cat "$cache/archive.sha256") == "$expected" && -f $cache/compiler.sha256 && -x $compiler ]] || fail "incomplete backstop cache; move $cache aside and retry"
        [[ $(hash_file "$compiler") == "$(cat "$cache/compiler.sha256")" ]] || fail "cached compiler changed; move $cache aside and retry"
    else
        lock=$root/.cache/bootstrap/v1/$host.lock
        mkdir -- "$lock" 2>/dev/null || fail "backstop extraction is active or interrupted: $lock"
        stage=
        cleanup() { [[ -z $stage ]] || rm -rf -- "$stage"; rmdir -- "$lock"; }
        trap cleanup EXIT
        stage=$(mktemp -d "$root/.cache/bootstrap/v1/$host.stage.XXXXXX")
        if [[ $os == darwin ]]; then
            tar -xf "$archive" -C "$stage"
        else
            unzip -q "$archive" -d "$stage"
        fi
        extracted=$stage/cross_cc-$host
        [[ -x $extracted/cross_cc && -d $extracted/lib/include && -d $extracted/lib/libc && -f $extracted/upstream.lock.json ]] || fail 'incomplete backstop package'
        printf '%s\n' "$expected" > "$stage/archive.sha256"
        hash_file "$extracted/cross_cc" > "$stage/compiler.sha256"
        [[ ! -e $cache ]] || fail 'another process published the backstop cache'
        mv -- "$stage" "$cache"
        stage=
        rmdir -- "$lock"
        trap - EXIT
    fi
    printf 'build: using v1 backstop %s\n' "$compiler"
fi
compiler=$(resolve_executable "$compiler")
[[ -f $compiler && -x $compiler ]] || fail "compiler is not executable: $compiler"
seed_root=${CROSS_CC_SEED_ROOT:-}
if [[ -z $seed_root ]]; then
    probe=$(dirname -- "$compiler")
    while :; do
        if [[ -f $probe/upstream.lock.json && -d $probe/lib/include && -d $probe/lib/libc ]]; then seed_root=$probe; break; fi
        [[ $probe != / ]] || break
        probe=$(dirname -- "$probe")
    done
fi
[[ -n $seed_root && -d $seed_root/lib/include ]] || fail 'compiler resources not found; keep its package together or set CROSS_CC_SEED_ROOT'
seed_root=$(CDPATH= cd -- "$seed_root" && pwd -P)
export CROSS_CC_SEED_ROOT=$seed_root
mkdir -p -- "$root/build/tools" "$root/.cache/tmp" "$root/.cache/bootstrap/compiler-cache"
printf 'build: seed %s\n' "$compiler"
for tool in build release linux-release-deps macos-x64-llvm; do
    env -u ZIG_LIB_DIR -u ZIG_GLOBAL_CACHE_DIR -u ZIG_LIBC -u CPATH -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
        -u LIBRARY_PATH -u COMPILER_PATH -u GCC_EXEC_PREFIX -u SDKROOT -u MACOSX_DEPLOYMENT_TARGET \
        -u CCC_OVERRIDE_OPTIONS -u CL -u _CL_ \
        CROSS_CC_ROOT="$seed_root" CROSS_CC_LIB_DIR="$seed_root/lib" \
        CROSS_CC_CACHE_DIR="$root/.cache/bootstrap/compiler-cache" \
        TMPDIR="$root/.cache/tmp" \
        "$compiler" cc -std=c11 -O1 -g0 "$root/tools/$tool.c" -o "$root/build/tools/$tool.part"
    mv -- "$root/build/tools/$tool.part" "$root/build/tools/$tool"
done
if [[ ${#args[@]} == 0 ]]; then
    args=(build)
elif [[ ${args[0]} == -* && ${args[0]} != --help && ${args[0]} != -h ]]; then
    args=(build "${args[@]}")
fi
cd -- "$root"
exec "$root/build/tools/build" "${args[@]}" "--compiler=$compiler"
