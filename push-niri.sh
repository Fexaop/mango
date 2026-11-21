#!/usr/bin/env bash
# Run manually to publish the topic branches and their integration branch.
# Atomic push: either all four refs update or none do. Never force-push.
set -euo pipefail

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd -- "$repo_dir"

expected_remote='git@github.com:Fexaop/mango.git'
if [[ $(git remote get-url --push --all origin) != "$expected_remote" ]]; then
    printf 'Refusing to push: origin must point only to %s\n' "$expected_remote" >&2
    exit 1
fi

branches=(
    wip/niri-overview
    wip/animation-fix
    wip/niri-gestures
    wip/niri
)
refspecs=()
for branch in "${branches[@]}"; do
    refspecs+=("refs/heads/$branch:refs/heads/$branch")
done

git push --atomic --set-upstream origin "${refspecs[@]}"
