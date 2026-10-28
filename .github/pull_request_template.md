## What and why

<!-- What does this change, and why is it needed? Link the issue: "Fixes #123". -->

## How it works

<!-- The approach, and the alternatives you rejected. The diff shows what; say why. -->

## How it was tested

<!-- Name the tests that cover it. A bug fix needs a test that fails without the change. -->

- [ ] `bash tests/run_all.sh`
- [ ] `bash tests/run_daemon_tests.sh`
- [ ] `bash tests/run_engine_tests.sh`
- [ ] Sanitizer run (ASan + UBSan, and TSan if threads are involved) for changes to the core's threading, memory or I/O

## Risk

<!-- What could this break? Which behaviour, protocol field or limit changes, and for whom? -->

## Checklist

- [ ] Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/)
- [ ] Tests added or updated
- [ ] Documentation updated in the same change (`docs/`, README, comments next to constants)
- [ ] `CHANGELOG.md` updated under **Unreleased** if a user or a client can notice the change
- [ ] A protocol change touches `ipc/`, the core, the daemon's client, the tests **and** `docs/IPC_PROTOCOL.md`
