#!/usr/bin/env bash
# FluxDM — .github/scripts/check-commits.sh
#
# Checks the commit messages in a range against the same rules as the commit-msg hook
# (Conventional Commits), so the rules still hold when a hook was bypassed or never installed.
#
#   check-commits.sh <base> <head>   the commits reachable from <head> but not from <base>
#   check-commits.sh ""     <head>   only <head>: used when there is no usable base
#                                    (the first push of a branch, a force-push, a manual run)
#
# Merge commits are skipped. Exit status: 0 all good, 1 at least one message rejected, 2 usage or setup error.
set -euo pipefail

base="${1:-}"
head="${2:?usage: check-commits.sh <base> <head>}"

root="$(git rev-parse --show-toplevel)"
hook="${root}/.githooks/commit-msg"
if [[ ! -x "$hook" ]]; then
    echo "check-commits: ${hook} is missing or not executable" >&2
    exit 2
fi

null_sha='0000000000000000000000000000000000000000'
if [[ -n "$base" && "$base" != "$null_sha" ]] && git cat-file -e "${base}^{commit}" 2>/dev/null; then
    mapfile -t commits < <(git rev-list --no-merges --reverse "${base}..${head}")
else
    mapfile -t commits < <(git rev-list --no-merges -n 1 "${head}")
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

rejected=0
for sha in "${commits[@]}"; do
    git log -n 1 --format=%B "$sha" > "${work}/message"
    subject="$(git log -n 1 --format=%s "$sha")"
    if "$hook" "${work}/message" 2> "${work}/why"; then
        printf '  ok        %s  %s\n' "${sha:0:9}" "$subject"
    else
        rejected=$((rejected + 1))
        printf '  REJECTED  %s  %s\n' "${sha:0:9}" "$subject"
        sed 's/^/            /' "${work}/why"
        if [[ -n "${GITHUB_ACTIONS:-}" ]]; then
            echo "::error title=Commit message::${sha:0:9} does not follow Conventional Commits: ${subject}"
        fi
    fi
done

echo
if ((rejected > 0)); then
    echo "${rejected} of ${#commits[@]} commit message(s) rejected. See CONTRIBUTING.md#commit-messages."
    exit 1
fi
echo "${#commits[@]} commit message(s) checked."
