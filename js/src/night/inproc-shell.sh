#!/usr/bin/env bash
# jit_test.py shim: runs the wasm AOT shell under wasm-jit-runner as JS_SHELL.
# jit_test.py passes the test file as a trailing "-f path", but the shell's
# --night-inprocess batch compiles only the positional script, so when the last
# file-ish option is a -f it is promoted to a positional argument (execution
# order is preserved: the positional runs after all options).
#
# Env: NIGHT_RUNNER, NIGHT_WASM_SHELL, NIGHT_CACHE_DIR override the defaults;
#      NIGHT_INPROCESS_OFF=1 omits --night-inprocess (baseline lane).
set -u

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
SHELL_WASM=${NIGHT_WASM_SHELL:-$repo/obj-nightmonkey-inprocess/dist/bin/js}
# Prefer the build-installed runner next to the shell (js/src/night/moz.build
# installs it into dist/bin); fall back to the manual in-crate build.
default_runner=$(dirname "$SHELL_WASM")/wasm-jit-runner
if [ ! -x "$default_runner" ]; then
  default_runner=$repo/js/src/night/wasm-jit-runner/target/release/wasm-jit-runner
fi
RUNNER=${NIGHT_RUNNER:-$default_runner}
CACHE=${NIGHT_CACHE_DIR:-$HOME/.cache/wjr}
mkdir -p "$CACHE"

argv=("$@")
n=${#argv[@]}
last_kind=""
last_f=-1
i=0
while [ "$i" -lt "$n" ]; do
  case ${argv[i]} in
    --) last_kind=raw; break ;;
    -f) last_kind=f; last_f=$i; i=$((i + 2)) ;;
    -e | -p | --module) last_kind=${argv[i]}; i=$((i + 2)) ;;
    --module-load-path | --selfhosted-xdr-path | --selfhosted-xdr-mode)
      i=$((i + 2)) ;;
    *) i=$((i + 1)) ;;
  esac
done

out=()
if [ "$last_kind" = f ]; then
  for ((i = 0; i < n; i++)); do
    if [ "$i" -eq "$last_f" ] || [ "$i" -eq $((last_f + 1)) ]; then continue; fi
    out+=("${argv[i]}")
  done
  out+=("${argv[last_f + 1]}")
elif [ "$n" -gt 0 ]; then
  out=("${argv[@]}")
fi

aot=()
if [ "${NIGHT_INPROCESS_OFF:-0}" != 1 ]; then
  aot=(--night-inprocess)
fi

exec "$RUNNER" --dir / --cache-dir "$CACHE" "$SHELL_WASM" -- \
  ${aot[@]+"${aot[@]}"} ${out[@]+"${out[@]}"}
