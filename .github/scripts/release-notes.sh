#!/usr/bin/env bash
# FluxDM — .github/scripts/release-notes.sh
#
# Prints the CHANGELOG.md entry for a release, ready to use as GitHub release notes.
#
#   release-notes.sh v0.2.0        (the leading "v" is optional)
#
# Relative links in the entry are rewritten to absolute ones pinned to the tag, because a release page is not
# inside the repository tree. Fails if the changelog has no entry for the version.
set -euo pipefail

tag="${1:?usage: release-notes.sh <tag-or-version>}"
version="${tag#v}"
repo="${GITHUB_REPOSITORY:-PhillipWanjiru/Fluxdm}"
changelog="$(git rev-parse --show-toplevel)/CHANGELOG.md"

# The entry runs from its heading to the next release heading, or to the link definitions at the foot of the file.
section="$(awk -v heading="## [${version}]" '
    /^## \[/ { if (found) exit; if (index($0, heading) == 1) { found = 1; next } }
    /^\[[^]]+\]: / { if (found) exit }
    found    { print }
' "$changelog" | sed -e '/./,$!d')"

if [[ -z "$section" ]]; then
    echo "release-notes: CHANGELOG.md has no entry for ${version}. Add a '## [${version}] - <date>' section first." >&2
    exit 1
fi

printf '%s\n' "$section" |
    sed -E "s#\]\(([A-Za-z0-9_.][^):]*)\)#](https://github.com/${repo}/blob/v${version}/\1)#g"
