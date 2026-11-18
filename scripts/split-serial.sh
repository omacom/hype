#!/usr/bin/env bash
# Replay a prompt pack: one prompt per fresh session, stopping at the first
# failing gate. Run it in the repo the prompts change.
#
#   ./scripts/split-serial.sh                  pack is ./prompt
#   ./scripts/split-serial.sh ../packs/listmonk
#   CHECK_ONLY=1 ./scripts/split-serial.sh     print pack and gate, run nothing
#   CHECK='make test' ./scripts/split-serial.sh
#   COMMIT=0 ./scripts/split-serial.sh         leave commits alone
#   FROM=05-name ./scripts/split-serial.sh     resume at a step
#
# Files run in filename order, so 01-, 02-, 03- sets the order. Logs land in
# .scratch/split/.

set -euo pipefail

usage() {
  awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

if ! [ -d prompt ] && [ "$(basename "$PWD")" = "scripts" ] && [ -d ../prompt ]; then
  cd ..
fi

PACK=${1:-${PACK:-prompt}}
MODEL=${MODEL:-ollama/deepseek-v4.1-flash:1017-cloud}
COMMIT=${COMMIT:-1}
FROM=${FROM:-}
LOGS=.scratch/split

if [ ! -d "$PACK" ]; then
  echo "no pack at $PACK; prompts live in <repo>/$PACK" >&2
  exit 1
fi

shopt -s nullglob
files=("$PACK"/*.md)
shopt -u nullglob

if [ ${#files[@]} -eq 0 ]; then
  echo "no prompts in $PACK" >&2
  exit 1
fi

CHECK=${CHECK:-}
if [ -z "$CHECK" ] && [ -f package.json ] && \
    node -e 'process.exit(require("./package.json").scripts?.check ? 0 : 1)' 2>/dev/null; then
  CHECK="npm run check"
fi

if [ "${CHECK_ONLY:-0}" = "1" ]; then
  printf 'repo:  %s\n' "$PWD"
  printf 'pack:  %s (%d prompts)\n' "$PACK" "${#files[@]}"
  printf 'check: %s\n' "${CHECK:-none}"
  exit 0
fi

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  COMMIT=0
fi

if [ "$COMMIT" = "1" ] && [ -n "$(git status --porcelain --untracked-files=normal -- . ':(exclude).scratch')" ]; then
  echo "tree is dirty; commit it first, or run with COMMIT=0" >&2
  exit 1
fi

mkdir -p "$LOGS"

for file in "${files[@]}"; do
  name=$(basename "$file" .md)
  if [ -n "$FROM" ] && [[ "$name" < "$FROM" ]]; then
    continue
  fi

  echo "== $name"
  if ! opencode run -m "$MODEL" --auto --title "split $name" "$(cat "$file")" 2>&1 \
      | tee "$LOGS/$name.log"; then
    echo "== $name: opencode failed, stopping" >&2
    exit 1
  fi

  if [ -n "$CHECK" ]; then
    if ! eval "$CHECK" >"$LOGS/$name.check" 2>&1; then
      echo "== $name: check failed: $CHECK" >&2
      echo "== see $LOGS/$name.check; fix, then resume with FROM=$name" >&2
      exit 1
    fi
  fi

  if [ "$COMMIT" = "1" ]; then
    git add -A -- .
    if git diff --cached --quiet; then
      echo "== $name: nothing to commit"
    else
      git commit -q -m "split: $name"
    fi
  fi
done

echo "== finished"
