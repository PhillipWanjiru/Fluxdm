#!/usr/bin/env python3
"""Mutation check for the rules of the Electron shell (ui/electron).

Each rule is broken on purpose, one at a time, in a throw-away copy, and the shell's tests (node --test) are run
against the broken copy. A mutation that no test notices is a rule that nothing pins: the tests would stay green if
someone weakened it. The working tree is never touched.

    python3 tests/mutation_electron.py              every mutation (about three minutes)
    python3 tests/mutation_electron.py -k token     only those whose description contains "token"
    python3 tests/mutation_electron.py --list       the descriptions, without running anything

Exit status 0 only when every mutation is caught. It is not part of CI (it takes minutes, and each entry quotes the
source it changes, so a refactor of the shell means updating the list: the script says which entries no longer
apply). Run it when you change a rule, and add the missing test when something survives.
"""
import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

# What the shell's tests compare with when the daemon's sources are in the checkout (config.test.js, launch.test.js).
DAEMON_FILES = [
    "daemon/src/main/resources/application.properties",
    "daemon/src/main/java/com/fluxdm/security/ApiGuard.java",
]

# (file in ui/electron, text to replace, replacement, what the change breaks). The text must occur exactly once.
MUTATIONS = [
    # --- the Content-Security-Policy
    ("security.js", "\"script-src 'self'\",", "\"script-src 'self' 'unsafe-inline'\",", "the policy allows inline script"),
    ("security.js", "\"default-src 'none'\",", "\"default-src 'self'\",", "the policy's default-src opens up"),
    ("security.js", "`connect-src 'self' ${daemonOrigin} ${socket}`,", "`connect-src *`,", "the policy lets the page connect anywhere"),
    ("security.js", "\"object-src 'none'\",", "\"object-src 'self'\",", "the policy allows plug-ins"),
    ("security.js", "\"frame-ancestors 'none'\",", "\"frame-ancestors 'self'\",", "the policy allows framing"),
    ("security.js", "\"base-uri 'none'\",", "\"base-uri 'self'\",", "the policy allows <base>"),
    ("security.js", "\"form-action 'none'\",", "\"form-action 'self'\",", "the policy allows form posts"),
    ("security.js", "\"style-src 'self'\",", "\"style-src 'self' 'unsafe-inline'\",", "the policy allows inline style"),
    ("security.js", "\"img-src 'self' data:\",", "\"img-src *\",", "the policy allows images from anywhere"),
    ("security.js", "const socket = `${protocol === \"https:\" ? \"wss\" : \"ws\"}://${host}`;", "const socket = `ws://${host}`;", "the policy forgets wss for an https daemon"),
    # --- who is the app's own page
    ("security.js", "url.host === \"-\" && url.username === \"\" && url.password === \"\"", "url.host !== \"\" && url.username === \"\" && url.password === \"\"", "any app:// host is trusted"),
    ("security.js", "url.host === \"-\" && url.username === \"\" && url.password === \"\"", "url.host === \"-\"", "credentials in the trusted URL are ignored"),
    ("security.js", "return dev && url.origin === DEV_ORIGIN;", "return url.origin === DEV_ORIGIN;", "the development server is trusted in production"),
    ("security.js", "return dev && url.origin === DEV_ORIGIN;", "return dev;", "any URL is trusted in development"),
    # --- which file an address names
    ("security.js", "if (url.protocol !== `${APP_SCHEME}:` || url.host !== \"-\") return null;", "if (url.protocol !== `${APP_SCHEME}:`) return null;", "app://other-host/ is served"),
    ("security.js", "if (url.protocol !== `${APP_SCHEME}:` || url.host !== \"-\") return null;", "if (url.host !== \"-\") return null;", "any scheme with the host - is served"),
    ("security.js", "if (pathname.includes(\"\\0\") || pathname.includes(\"\\\\\")) return null;", "if (pathname.includes(\"\\0\")) return null;", "a backslash is allowed in a path"),
    ("security.js", "if (pathname.includes(\"\\0\") || pathname.includes(\"\\\\\")) return null;", "if (pathname.includes(\"\\\\\")) return null;", "a NUL byte is allowed in a path"),
    ("security.js", "pathname = decodeURIComponent(url.pathname);", "pathname = url.pathname;", "percent-escapes are not decoded before the checks"),
    ("security.js", "if (parts.some((part) => part === \".\" || part === \"..\")) return null;", "if (parts.some((part) => part === \".\")) return null;", "'..' parts are allowed (only the second check is left)"),
    ("security.js", "if (parts.some((part) => part === \".\" || part === \"..\")) return null;", "if (parts.some((part) => part === \"..\")) return null;", "'.' parts are allowed"),
    ("security.js", "parts.length === 0 ? [\"index.html\"] : parts", "parts.length === 0 ? [] : parts", "/ no longer serves the page"),
    ("security.js", "if (inside === \"\" || inside.startsWith(\"..\") || path.isAbsolute(inside)) return null;", "if (path.isAbsolute(inside)) return null;", "the second check on the resolved path is gone"),
    ("security.js", "\".js\": \"text/javascript; charset=utf-8\",", "\".js\": \"text/plain; charset=utf-8\",", "scripts are served as text"),
    ("security.js", "\".html\": \"text/html; charset=utf-8\",", "\".html\": \"text/plain; charset=utf-8\",", "the page is served as text"),
    ("security.js", "|| \"application/octet-stream\"", "|| \"text/html; charset=utf-8\"", "unknown file types are served as HTML"),
    # --- the answer to a request
    ("appProtocol.js", "if (method !== \"GET\" && method !== \"HEAD\")", "if (method !== \"GET\")", "HEAD is refused"),
    ("appProtocol.js", "if (method !== \"GET\" && method !== \"HEAD\")", "if (method !== \"GET\" && method !== \"HEAD\" && method !== \"POST\")", "POST is answered"),
    ("appProtocol.js", "if (file === null) return refuse(404, \"Not found\");", "", "an address that names nothing is read from disk anyway"),
    ("appProtocol.js", "\"content-security-policy\": csp,", "\"content-security-policy-report-only\": csp,", "the policy is only reported, not enforced"),
    ("appProtocol.js", "\"x-content-type-options\": \"nosniff\",\n      \"cache-control\"", "\"cache-control\"", "nosniff is dropped from a file response"),
    ("appProtocol.js", "\"cache-control\": \"no-store\",", "\"cache-control\": \"max-age=3600\",", "files are cached"),
    ("appProtocol.js", "body: method === \"HEAD\" ? null : body,", "body,", "HEAD is answered with a body"),
    ("appProtocol.js", "{ allow: \"GET, HEAD\" }", "{}", "405 comes without Allow"),
    # --- the window
    ("shell.js", "sandbox: true,", "sandbox: false,", "the renderer is not sandboxed"),
    ("shell.js", "contextIsolation: true,", "contextIsolation: false,", "context isolation is off"),
    ("shell.js", "nodeIntegration: false,", "nodeIntegration: true,", "the page has Node"),
    ("shell.js", "webSecurity: true,", "webSecurity: false,", "web security is off"),
    ("shell.js", "webviewTag: false,", "webviewTag: true,", "the webview tag is allowed"),
    ("shell.js", "allowRunningInsecureContent: false,", "allowRunningInsecureContent: true,", "insecure content is allowed"),
    ("shell.js", "if (!trusted(event.url ?? url)) event.preventDefault();", "if (false) event.preventDefault();", "navigation anywhere is allowed"),
    ("shell.js", "contents.on(\"will-navigate\", stay);", "", "will-navigate is not guarded"),
    ("shell.js", "contents.on(\"will-redirect\", stay);", "", "will-redirect is not guarded"),
    ("shell.js", "contents.on(\"will-attach-webview\", (event) => event.preventDefault());", "contents.on(\"will-attach-webview\", () => {});", "webviews may attach"),
    ("shell.js", "contents.setWindowOpenHandler(() => ({ action: \"deny\" }));", "contents.setWindowOpenHandler(() => ({ action: \"allow\" }));", "new windows are allowed"),
    ("shell.js", "ses.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));", "ses.setPermissionRequestHandler((_contents, _permission, callback) => callback(true));", "permission requests are granted"),
    ("shell.js", "ses.setPermissionCheckHandler(() => false);", "ses.setPermissionCheckHandler(() => true);", "permission checks are granted"),
    ("shell.js", "if (!app.requestSingleInstanceLock()) {", "if (false) {", "there is no single-instance lock"),
    ("shell.js", "{ scheme: APP_SCHEME, privileges: { standard: true, secure: true, supportFetchAPI: true } },", "{ scheme: APP_SCHEME, privileges: { standard: true, supportFetchAPI: true } },", "app:// is not registered as secure"),
    ("shell.js", "{ scheme: APP_SCHEME, privileges: { standard: true, secure: true, supportFetchAPI: true } },", "{ scheme: APP_SCHEME, privileges: { secure: true, supportFetchAPI: true } },", "app:// is not registered as standard"),
    ("shell.js", "{ scheme: APP_SCHEME, privileges: { standard: true, secure: true, supportFetchAPI: true } },", "{ scheme: APP_SCHEME, privileges: { standard: true, secure: true } },", "app:// may not fetch"),
    ("shell.js", "win.loadURL(dev ? DEV_ORIGIN : `${APP_ORIGIN}/`);", "win.loadURL(dev ? DEV_ORIGIN : `file://${buildDir}/index.html`);", "the page is opened from file:// again"),
    ("shell.js", "if (!dev && !fs.existsSync(path.join(buildDir, \"index.html\"))) {", "if (false) {", "a missing build is not reported"),
    # --- who is given the token
    ("shell.js", "if (!frame || frame.parent || !trusted(frame.url)) return \"\";", "if (!frame || !trusted(frame.url)) return \"\";", "a frame inside the page is given the token"),
    ("shell.js", "if (!frame || frame.parent || !trusted(frame.url)) return \"\";", "if (!frame || frame.parent) return \"\";", "any page is given the token"),
    ("shell.js", "if (!frame || frame.parent || !trusted(frame.url)) return \"\";", "if (frame && (frame.parent || !trusted(frame.url))) return \"\";", "a request with no frame is given the token"),
    ("shell.js", "return (await fs.promises.readFile(tokenFile, \"utf8\")).trim();", "return (await fs.promises.readFile(tokenFile, \"utf8\"));", "the token's newline is kept"),
    ("shell.js", "} catch {\n      return \"\";\n    }\n  });", "} catch (error) {\n      throw error;\n    }\n  });", "a missing token file throws into the page"),
    # --- what the page can call
    ("preload.js", "apiToken: () => ipcRenderer.invoke(\"fluxdm:api-token\"),", "apiToken: () => ipcRenderer.invoke(\"fluxdm:api-token\"),\n  send: (channel, ...args) => ipcRenderer.send(channel, ...args),", "a generic send() is exposed again"),
    ("preload.js", "apiToken: () => ipcRenderer.invoke(\"fluxdm:api-token\"),", "apiToken: (channel = \"fluxdm:api-token\") => ipcRenderer.invoke(channel),", "the page chooses the channel"),
    ("preload.js", "apiToken: () => ipcRenderer.invoke(\"fluxdm:api-token\"),", "apiToken: (...args) => ipcRenderer.invoke(\"fluxdm:api-token\", ...args),", "the page's arguments are forwarded"),
    ("preload.js", "ipcRenderer.invoke(\"fluxdm:api-token\")", "ipcRenderer.invoke(\"fluxdm:token\")", "the preload asks on another channel than the shell answers on"),
    # --- where things are, and what the environment decides
    ("config.js", "DAEMON_ORIGIN: \"http://127.0.0.1:7171\",", "DAEMON_ORIGIN: \"http://127.0.0.1:7172\",", "the daemon's port"),
    ("config.js", "APP_ORIGIN: \"app://-\",", "APP_ORIGIN: \"app://app\",", "the page's origin"),
    ("config.js", "DEV_ORIGIN: \"http://localhost:3000\",", "DEV_ORIGIN: \"http://localhost:3001\",", "the development origin"),
    ("launch.js", "path.join(homedir, \".fluxdm\", \"api-token\")", "path.join(homedir, \".fluxdm\", \"token\")", "the token file's name"),
    ("launch.js", "path.join(homedir, \".fluxdm\", \"api-token\")", "path.join(homedir, \"api-token\")", "the token file's folder"),
    ("launch.js", "env.FLUXDM_TOKEN_FILE || path.join", "path.join", "FLUXDM_TOKEN_FILE is ignored"),
    ("launch.js", "dev: env.NODE_ENV === \"development\",", "dev: Boolean(env.NODE_ENV),", "any NODE_ENV means development"),
    ("launch.js", "dev: env.NODE_ENV === \"development\",", "dev: env.NODE_ENV !== \"production\",", "development is the default"),
    ("launch.js", "buildDir: path.join(dirname, \"..\", \"build\"),", "buildDir: path.join(dirname, \"build\"),", "the build folder's place"),
    ("launch.js", "preload: path.join(dirname, \"preload.js\"),", "preload: path.join(dirname, \"..\", \"preload.js\"),", "the preload's place"),
]

# No input can tell these from the original: an earlier check already catches everything they would catch. They stay
# as defence in depth, and are named here so that a survivor is a surprise only when it is not one of them.
REDUNDANT = {"the second check on the resolved path is gone"}


def run_tests(tree: pathlib.Path, scratch: pathlib.Path) -> tuple[bool, int]:
    """Run the shell's tests in `tree`/ui; return (all passed, number of failing tests)."""
    tests = sorted(str(path) for path in (tree / "ui" / "electron").glob("*.test.js"))
    done = subprocess.run(
        ["node", "--test", *tests],
        cwd=tree / "ui",
        env={**os.environ, "TMPDIR": str(scratch)},
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    failing = sum(1 for line in done.stdout.splitlines() if line.startswith("not ok"))
    return done.returncode == 0, failing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-k", metavar="TEXT", help="only the mutations whose description contains TEXT")
    parser.add_argument("--list", action="store_true", help="print the descriptions and stop")
    args = parser.parse_args()

    chosen = [m for m in MUTATIONS if not args.k or args.k.lower() in m[3].lower()]
    if args.list:
        for number, (target, _old, _new, label) in enumerate(chosen, 1):
            print(f"{number:2d}. [{target}] {label}")
        return 0
    if not chosen:
        print(f"no mutation mentions {args.k!r}", file=sys.stderr)
        return 2
    if shutil.which("node") is None:
        print("node is needed (https://nodejs.org)", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="fluxdm-mutation-") as work:
        tree = pathlib.Path(work) / "tree"
        scratch = pathlib.Path(work) / "tmp"
        scratch.mkdir()
        shutil.copytree(ROOT / "ui" / "electron", tree / "ui" / "electron")
        for name in DAEMON_FILES:
            if (ROOT / name).is_file():
                (tree / name).parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(ROOT / name, tree / name)

        passed, _ = run_tests(tree, scratch)
        if not passed:
            print("the shell's tests fail without any mutation; fix that first (npm run test:electron in ui/)", file=sys.stderr)
            return 2

        survived, inapplicable, redundant = [], [], []
        for number, (target, old, new, label) in enumerate(chosen, 1):
            file = tree / "ui" / "electron" / target
            original = file.read_text(encoding="utf-8")
            if original.count(old) != 1:
                print(f"{number:2d}. [{target}] {label}: DOES NOT APPLY (the text occurs {original.count(old)} times; update the list)")
                inapplicable.append(label)
                continue
            try:
                file.write_text(original.replace(old, new), encoding="utf-8")
                passed, failing = run_tests(tree, scratch)
            finally:
                file.write_text(original, encoding="utf-8")
            if not passed:
                verdict = f"caught ({failing} failing)"
            elif label in REDUNDANT:
                verdict = "survived (redundant by design)"
                redundant.append(label)
            else:
                verdict = "SURVIVED"
                survived.append(label)
            print(f"{number:2d}. [{target}] {label}: {verdict}")

    caught = len(chosen) - len(survived) - len(inapplicable) - len(redundant)
    print(f"\n{len(chosen)} mutations: {caught} caught, {len(redundant)} redundant by design, "
          f"{len(survived)} survived, {len(inapplicable)} did not apply")
    for label in survived:
        print(f"  SURVIVED: {label}  (no test pins this rule: add one)")
    for label in inapplicable:
        print(f"  DOES NOT APPLY: {label}")
    return 1 if survived or inapplicable else 0


if __name__ == "__main__":
    sys.exit(main())
