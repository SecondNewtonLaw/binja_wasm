#!/usr/bin/env bash
# Emits a GitHub Actions build matrix of Binary Ninja API refs to build.
#   plan.sh ci       latest stable + dev head (always)
#   plan.sh release  stable tags and dev head not yet released here
# Env: GH_TOKEN, GITHUB_REPOSITORY, MIN_BN (default 5.0), GITHUB_OUTPUT
set -euo pipefail

mode=${1:-ci}
min=${MIN_BN:-5.0}
api=Vector35/binaryninja-api

stable=$(gh api --paginate "repos/$api/git/matching-refs/tags/stable/" --jq '.[].ref' \
  | sed 's|^refs/tags/stable/||' | sort -V \
  | while read -r v; do
      [ "$(printf '%s\n%s\n' "$min" "$v" | sort -V | head -1)" = "$min" ] && echo "$v"
    done)

released=$(gh release list --repo "$GITHUB_REPOSITORY" --limit 1000 --json tagName --jq '.[].tagName' || true)

entries=()
add() { entries+=("$(jq -nc --arg l "$1" --arg r "$2" --arg t "$3" --arg k "$4" '{label:$l,ref:$r,tag:$t,kind:$k}')"); }

if [ "$mode" = ci ]; then
  v=$(echo "$stable" | tail -1)
  [ -n "$v" ] && add "$v" "stable/$v" "bn-$v" stable
else
  for v in $stable; do
    grep -qx "bn-$v" <<<"$released" || add "$v" "stable/$v" "bn-$v" stable
  done
fi

sha=$(gh api "repos/$api/commits/dev" --jq .sha)
if [ "$mode" = ci ] || ! gh release view bn-dev --repo "$GITHUB_REPOSITORY" --json body --jq .body 2>/dev/null | grep -q "$sha"; then
  add "dev-${sha:0:7}" "$sha" bn-dev dev
fi

matrix=$(printf '%s\n' "${entries[@]:-}" | jq -sc 'map(select(. != null))')
echo "matrix=$matrix" >> "$GITHUB_OUTPUT"
echo "count=$(jq length <<<"$matrix")" >> "$GITHUB_OUTPUT"
echo "$matrix"
