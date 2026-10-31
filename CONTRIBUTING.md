# Contributing to FluxDM

Thanks for taking an interest. FluxDM is a systems project: a small C core, a JVM daemon and a
binary protocol between them. The bar is "correct under hostile conditions, and provably so", so the
rules below are mostly about keeping changes small, tested and explainable.

- [Set up](#set-up)
- [Workflow](#workflow)
- [Commit messages](#commit-messages)
- [Tests](#tests)
- [Code style](#code-style)
- [Changing the protocol](#changing-the-protocol)
- [Pull requests](#pull-requests)
- [Releases and versioning](#releases-and-versioning)

## Set up

```bash
sudo apt install build-essential libssl-dev python3 openjdk-21-jdk maven

git clone https://github.com/PhillipWanjiru/Fluxdm.git
cd Fluxdm

# local guard rails: commit-message format, no secrets or huge files, a commit template
git config core.hooksPath .githooks
git config commit.template .gitmessage

make -C core                      # build the core
bash tests/run_all.sh --quick     # prove your checkout works
```

The hooks are small bash scripts in [`.githooks/`](.githooks); they need nothing beyond bash and git, and
they are tested by [`.github/scripts/test-hooks.sh`](.github/scripts/test-hooks.sh). Bypass them for a
single commit with `git commit --no-verify`: CI re-checks the commit messages of every push and pull
request, so a bypassed rule still shows up there.

## Workflow

1. Open an issue first for anything bigger than a small fix, so the approach can be agreed before the
   work is done.
2. Branch from `main`: `feat/<short-name>`, `fix/<short-name>`, `docs/<short-name>`, and so on.
3. Make small, focused commits. Each commit should build and pass the tests on its own, so that
   `git bisect` stays useful.
4. Keep `main` linear: rebase onto it rather than merging it into your branch, and merge your pull
   request with squash or rebase.
5. Never force-push `main`. Force-pushing your own branch is fine.

## Commit messages

FluxDM uses [Conventional Commits](https://www.conventionalcommits.org/). The history is the project's
changelog and its audit trail, and a hook checks the format.

```
<type>(<scope>): <subject>

<body: why the change is needed, and what was considered and rejected>

<footers: Fixes #12, Refs #34, BREAKING CHANGE: ...>
```

**Types**

| Type | Use for |
|---|---|
| `feat` | New behaviour a user or client can observe |
| `fix` | A bug fix |
| `perf` | A change whose purpose is speed or memory, with no behaviour change |
| `refactor` | Restructuring without a behaviour change |
| `test` | Adding or correcting tests only |
| `docs` | Documentation only |
| `style` | Formatting only (whitespace, comments); no code change |
| `build` | Makefiles, `pom.xml`, dependencies, scripts that build or package |
| `ci` | GitHub Actions and other automation |
| `chore` | Anything else that does not touch the product (repository housekeeping, archives) |
| `revert` | Reverting an earlier commit |

**Scopes** are optional; prefer one of: `core`, `ipc`, `daemon`, `engine`, `grabber`, `extension`,
`ui`, `tests`, `db`, `config`, `build`, `ci`, `docs`.

**Rules**

- The subject is imperative ("add", not "added" or "adds"), at most 72 characters, with no trailing
  period.
- The body explains **why**: what was wrong or missing, how the change fixes it, and what alternatives
  were rejected. The diff already shows *what*. Wrap at 72 columns.
- Mark an incompatible change with `!` after the type or scope and a `BREAKING CHANGE:` footer that says
  what breaks and how to migrate (`feat(ipc)!: ...`).
- One logical change per commit. A fix and an unrelated refactor are two commits.

A good message (illustrative):

```
fix(core): count an adopted probe connection in the host gate

A server that ignores Range is served by one segment that adopts the
probe's connection. If the gate is not told about it, it under-counts
the connections open to the host, and when the host refuses one it
learns an allowance that is too low and throttles every later download.
Register the adopted connection with flux_gate_hold() when it is adopted.

Test: test_rate_limit_queue, part 4 (fails without this change).
```

Bad: `fixed stuff`, `Update downloader.c`, `WIP`, `feat: lots of changes`.

## Tests

A change is not finished until its tests are. See [docs/TESTING.md](docs/TESTING.md) for the suites.

- **A bug fix starts with a test that fails**, at the lowest layer that can express the bug.
- **New behaviour comes with tests** for the behaviour and for how it fails.
- **Run what you touched**, then everything before you push:

  ```bash
  bash tests/run_all.sh              # C unit + integration + behaviour
  bash tests/run_daemon_tests.sh     # Java (add --no-maven if you have no network)
  bash tests/smoke_api.sh            # the built daemon's API defences; needs `make -C core` and `mvn -f daemon/pom.xml -DskipTests package`
  bash tests/run_engine_tests.sh     # C++ engine layer
  ```

- **Changes to the core's threading, memory or I/O** also need a sanitizer run (ASan + UBSan, and TSan
  if threads are involved); the commands are in [docs/TESTING.md](docs/TESTING.md#sanitizer-builds).
- Tests must be deterministic. Script the server instead of racing it, and inject the clock where the
  code allows it.

## Code style

[`.editorconfig`](.editorconfig) sets indentation and line endings; most editors honour it.

**C (core)**
- C11, warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes
  -Wmissing-prototypes` (the Makefile sets them).
- Segment code is single-threaded by design. Do not add locks there. State shared between threads goes
  through atomics with acquire/release ordering, or behind the one mutex that owns it.
- Report failures with `flux_set_error()` and a message a user can act on; never print from library
  code.
- Comments say *why* (the invariant, the failure that motivated the code), not *what*.
- No new global state without a reason written next to it.

**Java (daemon)**
- Java 21, constructor injection, no field injection.
- Anything that touches the database or the core's connection follows the rules at the top of
  `DownloadService`: one state lock, never held across IPC; guarded status transitions; idempotent slot
  release.
- If daemon code starts using a new Spring, Jakarta or SLF4J type, add a minimal stand-in under
  `tests/java/stubs/` so the harness still compiles it.

**Python (tests, grabber)**
- Tests use the standard library only. Target Python 3.10 or newer.

**Everything**
- No dead code, commented-out code, or TODOs without an issue number.
- Keep the docs in step with the code in the same commit (a table of limits that disagrees with the
  constant is a bug).

## Changing the protocol

The IPC protocol is a contract between two programs written in two languages. A change touches, in the
**same** pull request:

1. `ipc/protocol.h` and `ipc/messages.h`, with `_Static_assert`s on any size that changed;
2. the core's encoder, decoder and server;
3. the daemon's `CoreIPCClient`;
4. the tests on both sides, including a behaviour test that drives the new message;
5. [docs/IPC_PROTOCOL.md](docs/IPC_PROTOCOL.md).

Adding a field or message that old readers can ignore is compatible. Changing the meaning of an
existing field, or its size or position, is a new protocol version.

## Pull requests

- Fill in the template. Say what changed and why, how it was tested, and what could go wrong.
- Keep it reviewable: a few hundred lines is a good size; a refactor goes in its own pull request.
- CI must be green. If a timing-sensitive test fails, run it again locally with
  `python3 tests/test_core.py -k <name>` before concluding it is flaky, and say so in the pull request.
- Expect review comments about naming, invariants and tests more than about style.

## Releases and versioning

[Semantic Versioning](https://semver.org/). Releases are annotated tags (`v0.2.0`) on `main`, and every
release has an entry in [CHANGELOG.md](CHANGELOG.md), which follows
[Keep a Changelog](https://keepachangelog.com/). Pre-1.0, a minor version may include incompatible
changes; they are called out in the changelog.

## Reporting problems

- **Bugs and ideas:** open an issue and fill in the template.
- **Security issues:** do not open a public issue; follow [SECURITY.md](SECURITY.md).
