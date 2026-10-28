#!/usr/bin/env bash
# FluxDM — .github/scripts/test-hooks.sh
#
# Behavioural tests for .githooks/commit-msg and .githooks/pre-commit. Every case runs the real hook, either
# directly or through `git commit` in a throw-away repository, so what is tested is what contributors get.
#
#   bash .github/scripts/test-hooks.sh
set -uo pipefail

# Git sets these while it runs a hook; this script may be started from one.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_PREFIX

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
hooks="${root}/.githooks"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

passed=0
failed=0

# check <accept|reject> <label> <command...>
#   Runs the command; success means "accepted". Prints the command's output when the outcome is not the wanted one.
check() {
    local want="$1" label="$2" got
    shift 2
    if "$@" > "${work}/output" 2>&1; then got=accept; else got=reject; fi
    if [[ "$got" == "$want" ]]; then
        passed=$((passed + 1))
    else
        failed=$((failed + 1))
        echo "FAIL: ${label}: expected ${want}, got ${got}"
        sed 's/^/      /' "${work}/output"
    fi
}

# ── commit-msg, called directly ──────────────────────────────────────────────
# shellcheck disable=SC2329  # called through check()
commit_msg() {                 # commit_msg <message, with \n escapes>
    printf '%b' "$1" > "${work}/message"
    "${hooks}/commit-msg" "${work}/message"
}
msg() { check "$1" "message $(printf '%q' "$2")" commit_msg "$2"; }

subject_72="feat: $(printf 'x%.0s' {1..66})"
subject_73="feat: $(printf 'x%.0s' {1..67})"
if ((${#subject_72} != 72 || ${#subject_73} != 73)); then
    echo "FAIL: test setup builds subjects of the wrong length"
    failed=$((failed + 1))
fi

msg accept 'feat: add thing\n'
msg accept 'feat(core): add thing\n'
msg accept 'fix(core)!: break thing\n'
msg accept 'docs(readme-x.y/z): tweak\n'
msg accept 'perf(ipc): faster\n\nbody line one\nbody line two\n\nCo-Authored-By: A <a@example.test>\n'
msg accept 'revert: undo it\n'
msg accept "Merge branch 'x' into main\n"
msg accept 'Revert "feat: x"\n\nThis reverts commit abc.\n'
msg accept 'fixup! feat: x\n'
msg accept 'squash! feat: x\n'
msg accept 'amend! feat: x\n'
msg accept '# leading comment\nfeat: x\n\n# trailing comment\n'
msg accept '\n\nfeat: after blank lines\n'
msg accept "${subject_72}\n"
msg accept 'chore(archive): snapshot fluxdm_backup_2026-10-24\n\nBody.\n'
msg accept 'ci: add workflow\n'
msg accept 'build(deps): bump x\n'
msg accept 'style: whitespace\n'
msg accept 'test(core): cover retries\n'
msg accept 'refactor(daemon)!: rename\n\nBREAKING CHANGE: renamed.\n'

msg reject ''
msg reject '\n'
msg reject '# only a comment\n'
msg reject 'Update stuff\n'
msg reject 'wip\n'
msg reject 'feat:nospace\n'
msg reject 'feat: \n'
msg reject 'feat:  two spaces\n'
msg reject 'Feat: upper\n'
msg reject 'feat(Core): upper scope\n'
msg reject 'feat(): empty scope\n'
msg reject 'feat : space before colon\n'
msg reject 'feature: unknown type\n'
msg reject 'feat: ends with period.\n'
msg reject "${subject_73}\n"
msg reject 'feat: subject\nbody without blank line\n'
msg reject 'fix(core) missing colon\n'
msg reject ': no type\n'

# ── through git, in a throw-away repository ──────────────────────────────────
repo="${work}/repo"
git init -q "$repo"
cd "$repo" || exit 1
git config user.name "Hook Test"
git config user.email "hooks@example.test"
git config commit.gpgsign false
cp -r "$hooks" .githooks
git config core.hooksPath .githooks
git add .githooks
git commit -q --no-verify -m 'chore: hooks'

echo a > a.txt
git add a.txt
check reject 'git commit, bad message' git commit -q -m 'Bad message'
check accept 'git commit, good message' git commit -q -m 'feat: good message'

echo b > b.txt
git add b.txt
printf 'fix: edited in an editor\n\n# Please enter the commit message for your changes.\n# Lines starting with # are ignored.\n' > "${work}/edited"
check accept 'git commit -F, message with comment lines' git commit -q -F "${work}/edited"

echo c > c.txt
git add c.txt
check reject 'a template with nothing but comments' env GIT_EDITOR=true git commit -q -t "${root}/.gitmessage"
check accept 'git commit after the aborted one' git commit -q -m 'chore: c'

# staged <accept|reject> <label> [VAR=value...]: commit what is staged, then return to a clean tree
staged() {
    local want="$1" label="$2"
    shift 2
    check "$want" "pre-commit: ${label}" env "$@" git commit -q -m "chore: ${label}"
    git reset -q --hard HEAD
    git clean -qfd
}
big() { head -c "$1" /dev/zero > "$2"; }
mib=$((1024 * 1024))

echo 'plain text' > ok.txt
git add ok.txt
staged accept 'a plain file'

big $((5 * mib + 1)) big.bin
git add big.bin
staged reject 'a file over 5 MiB'

big $((5 * mib + 1)) big.bin
git add big.bin
staged accept 'a file over 5 MiB with FLUXDM_ALLOW_LARGE=1' FLUXDM_ALLOW_LARGE=1

big $((5 * mib)) exact.bin
git add exact.bin
staged accept 'a file of exactly 5 MiB'

big $((5 * mib + 1)) 'my big file.bin'
git add 'my big file.bin'
staged reject 'a large file with spaces in its name'

odd_name=$'odd\nname.bin'
big $((5 * mib + 1)) "$odd_name"
git add -- "$odd_name"
staged reject 'a large file with a newline in its name'

printf 'a\n<<<<<<< HEAD\nb\n=======\nc\n>>>>>>> other\n' > conflict.c
git add conflict.c
staged reject 'conflict markers in C source'

printf 'Title\n=======\nA setext heading uses equals signs\n<<<<<<< HEAD\n' > doc.md
git add doc.md
staged accept 'conflict-like lines in Markdown'

printf 'x = "<<<<<<<"\n' > notmarker.py
git add notmarker.py
staged accept 'seven "<" inside a string is not a marker line'

printf -- '%s RSA PRIVATE KEY-----\nabc\n' '-----BEGIN' > key.pem
git add key.pem
staged reject 'a private key'

printf -- '%s OPENSSH PRIVATE KEY-----\n' '-----BEGIN' > id.txt
git add id.txt
staged reject 'an OpenSSH private key'

printf 'token=ghp_%s\n' "$(printf 'a%.0s' {1..36})" > classic.env
git add classic.env
staged reject 'a GitHub classic token'

printf 'token=github_pat_%s\n' "$(printf 'A1b2%.0s' {1..12})" > fine.env
git add fine.env
staged reject 'a GitHub fine-grained token'

printf 'AWS_KEY=%s%s\n' AKIA IOSFODNN7EXAMPLE > aws.env
git add aws.env
staged reject 'an AWS access key id'

printf 'hook=%s%s\n' xoxb -1234567890-abcdefghij > slack.env
git add slack.env
staged reject 'a Slack token'

printf 'ghp_short and AKIA123 are not tokens\n' > short.txt
git add short.txt
staged accept 'near misses'

# An old line must not block an unrelated change: commit a marker with --no-verify, then edit another line.
printf 'one\n<<<<<<< old\ntwo\n' > legacy.c
git add legacy.c
git commit -q --no-verify -m 'chore: legacy'
printf 'one\n<<<<<<< old\ntwo\nthree\n' > legacy.c
git add legacy.c
staged accept 'a pre-existing marker line is not flagged again'

# A rename that also adds a secret must be caught (git reports it as R, not A).
echo 'payload one two three four five six seven eight nine ten' > r1.txt
git add r1.txt
git commit -q -m 'chore: r1'
git mv r1.txt r2.txt
printf 'payload one two three four five six seven eight nine ten\n%s%s\n' AKIA IOSFODNN7EXAMPLE > r2.txt
git add r2.txt
staged reject 'a secret added while renaming a file'

git rm -q a.txt
staged accept 'a deletion'

# `git commit -- <path>` commits only that path, and the hook must judge only that path.
printf '%s%s\n' AKIA IOSFODNN7EXAMPLE > secret.txt
echo 'harmless' > other.txt
git add secret.txt other.txt
check accept 'partial commit of a clean file' git commit -q -m 'chore: partial' -- other.txt
check reject 'the secret that is still staged' git commit -q -m 'chore: rest'
git reset -q --hard HEAD
git clean -qfd

# The repository must pass its own hooks: everything tracked or not ignored, staged in a fresh repository.
if git -C "$root" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    cd "$work" || exit 1
    mkdir self
    (cd "$root" && git ls-files -z --cached --others --exclude-standard | tar --null --ignore-failed-read -T - -cf -) | tar -xf - -C self
    cd self || exit 1
    git init -q .
    git config user.name "Hook Test"
    git config user.email "hooks@example.test"
    git config commit.gpgsign false
    git config core.hooksPath .githooks
    git add -A
    check accept "the repository's own files pass the pre-commit hook" .githooks/pre-commit
else
    echo "note: ${root} is not a git checkout; skipping the check of the repository's own files"
fi

echo
echo "hook tests: ${passed} passed, ${failed} failed"
exit $((failed > 0))
