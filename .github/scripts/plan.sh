#!/usr/bin/env bash
# Emits a GitHub Actions build matrix of Binary Ninja API refs to build.
#   plan.sh release  stable tags and dev head not yet released here
#   plan.sh minors   latest stable tag of each major.minor (marketplace)
# Env: GH_TOKEN, GITHUB_REPOSITORY, MIN_BN (default 5.2), GITHUB_OUTPUT
set -euo pipefail

mode=${1:-release}
min=${MIN_BN:-5.2}
api=Vector35/binaryninja-api

stable=$(gh api --paginate "repos/$api/git/matching-refs/tags/stable/" --jq '.[].ref' \
  | sed 's|^refs/tags/stable/||' | sort -V \
  | while read -r v; do
      [ "$(printf '%s\n%s\n' "$min" "$v" | sort -V | head -1)" = "$min" ] && echo "$v"
    done)

released=$(gh release list --repo "$GITHUB_REPOSITORY" --limit 1000 --json tagName --jq '.[].tagName' || true)

entries=()
add() { entries+=("$(jq -nc --arg l "$1" --arg r "$2" --arg t "$3" --arg k "$4" '{label:$l,ref:$r,tag:$t,kind:$k,mm:($l|split(".")[0:2]|join("."))}')"); }

if [ "$mode" = minors ]; then
  for v in $(echo "$stable" | awk -F. '{last[$1"."$2]=$0} END{for(k in last) print last[k]}' | sort -V); do
    add "$v" "stable/$v" "bn-$v" stable
  done
  matrix=$(printf '%s\n' "${entries[@]:-}" | jq -sc 'map(select(. != null))')
  echo "matrix=$matrix" >> "$GITHUB_OUTPUT"
  echo "count=$(jq length <<<"$matrix")" >> "$GITHUB_OUTPUT"
  echo "$matrix"
  exit 0
fi

for v in $stable; do
  grep -qx "bn-$v" <<<"$released" || add "$v" "stable/$v" "bn-$v" stable
done

sha=$(gh api "repos/$api/commits/dev" --jq .sha)
if ! gh release view bn-dev --repo "$GITHUB_REPOSITORY" --json body --jq .body 2>/dev/null | grep -q "$sha"; then
  add "dev-${sha:0:7}" "$sha" bn-dev dev
fi

matrix=$(printf '%s\n' "${entries[@]:-}" | jq -sc 'map(select(. != null))')
echo "matrix=$matrix" >> "$GITHUB_OUTPUT"
echo "count=$(jq length <<<"$matrix")" >> "$GITHUB_OUTPUT"
echo "$matrix"
