const test = require("node:test");
const assert = require("node:assert/strict");
const path = require("path");
const { contentSecurityPolicy, isTrustedUrl, mimeType, resolveAppFile } = require("./security");

const directives = (policy) =>
  Object.fromEntries(
    policy.split("; ").map((directive) => {
      const [name, ...sources] = directive.split(" ");
      return [name, sources];
    })
  );

test("the policy lets the page reach the daemon over HTTP and WebSocket, and nothing else on the network", () => {
  const policy = directives(contentSecurityPolicy());
  assert.deepEqual(policy["connect-src"], ["'self'", "http://127.0.0.1:7171", "ws://127.0.0.1:7171"]);
});

test("the policy refuses by default and allows scripts and styles only from the app itself", () => {
  const policy = directives(contentSecurityPolicy());
  assert.deepEqual(policy["default-src"], ["'none'"]);
  assert.deepEqual(policy["script-src"], ["'self'"]);
  assert.deepEqual(policy["style-src"], ["'self'"]);
  assert.deepEqual(policy["object-src"], ["'none'"]);
  assert.deepEqual(policy["base-uri"], ["'none'"]);
  assert.deepEqual(policy["form-action"], ["'none'"]);
  assert.deepEqual(policy["frame-ancestors"], ["'none'"]);
});

test("the policy never allows inline code, eval or a wildcard", () => {
  assert.doesNotMatch(contentSecurityPolicy(), /unsafe-inline|unsafe-eval|unsafe-hashes|\*/);
});

test("the policy follows the daemon's address, and uses wss for an https daemon", () => {
  assert.equal(
    directives(contentSecurityPolicy("http://127.0.0.1:9000"))["connect-src"].join(" "),
    "'self' http://127.0.0.1:9000 ws://127.0.0.1:9000"
  );
  assert.equal(
    directives(contentSecurityPolicy("https://daemon.example:8443"))["connect-src"].join(" "),
    "'self' https://daemon.example:8443 wss://daemon.example:8443"
  );
});

test("the packaged page is trusted, and nothing that merely looks like it", () => {
  for (const url of ["app://-/", "app://-/index.html", "app://-/static/js/main.js?x=1#y"]) {
    assert.equal(isTrustedUrl(url), true, url);
  }
  for (const url of [
    "app://evil/",
    "app://-:81/",
    "app://user@-/",
    "app://-@evil/",
    "http://localhost:3000/",
    "https://example.com/",
    "file:///home/u/fluxdm/ui/build/index.html",
    // eslint-disable-next-line no-script-url -- a javascript: address is exactly what must not be trusted
    "javascript:alert(1)",
    "data:text/html,<b>x</b>",
    "about:blank",
    "",
    "not a url",
  ]) {
    assert.equal(isTrustedUrl(url), false, url);
  }
});

test("the development server is trusted only in development, and only at its own origin", () => {
  assert.equal(isTrustedUrl("http://localhost:3000/", { dev: true }), true);
  assert.equal(isTrustedUrl("http://localhost:3000/static/js/bundle.js", { dev: true }), true);
  assert.equal(isTrustedUrl("app://-/", { dev: true }), true);
  for (const url of ["http://localhost:3000.evil.test/", "http://localhost:3001/", "https://localhost:3000/", "http://127.0.0.1:3000/"]) {
    assert.equal(isTrustedUrl(url, { dev: true }), false, url);
  }
  assert.equal(isTrustedUrl("http://localhost:3000/"), false);
  assert.equal(isTrustedUrl("http://localhost:3000/", { dev: false }), false);
});

const root = path.join(path.sep, "srv", "fluxdm", "build");
const inside = (file) => file === null || file.startsWith(root + path.sep);

test("an address names a file under the root; / is the page", () => {
  assert.equal(resolveAppFile(root, "app://-/"), path.join(root, "index.html"));
  assert.equal(resolveAppFile(root, "app://-/index.html"), path.join(root, "index.html"));
  assert.equal(resolveAppFile(root, "app://-/static/js/main.abc.js"), path.join(root, "static", "js", "main.abc.js"));
  assert.equal(resolveAppFile(root, "app://-/static//js///main.js?v=1#top"), path.join(root, "static", "js", "main.js"));
  assert.equal(resolveAppFile(root, "app://-/my%20file.txt"), path.join(root, "my file.txt"));
});

test("an address in another scheme or on another host names nothing", () => {
  for (const url of ["file:///etc/passwd", "http://-/index.html", "https://example.com/index.html", "app://evil/index.html", "app://-:81/index.html", "", "not a url"]) {
    assert.equal(resolveAppFile(root, url), null, url);
  }
});

test("no spelling of a path leaves the root", () => {
  for (const url of [
    "app://-/../etc/passwd",
    "app://-/%2e%2e/%2e%2e/etc/passwd",
    "app://-/%2E%2E%2f%2E%2E%2fetc/passwd",
    "app://-/static/../../etc/passwd",
    "app://-/..%2f..%2fetc/passwd",
    "app://-/static%2f..%2f..%2fetc/passwd",
    "app://-/%5c..%5c..%5cetc/passwd",
    "app://-/..%5cetc",
    "app://-/%2e/%2e%2e/etc",
    "app://-/%00",
    "app://-/index.html%00.png",
    "app://-/%E0%A4%A",
  ]) {
    assert.ok(inside(resolveAppFile(root, url)), url);
  }
});

test("the spellings that URL parsing does not tidy away are refused outright", () => {
  for (const url of [
    "app://-/..%2f..%2fetc/passwd",
    "app://-/static%2f..%2f..%2fetc/passwd",
    "app://-/static%2f..%2findex.html", /* back inside the root, but still a dot-segment in disguise */
    "app://-/static%2f.%2fjs%2fmain.js", /* a single dot in disguise: harmless in itself, and never a path the build has */
    "app://-/.%2findex.html",
    "app://-/%5c..%5cetc",
    "app://-/%00",
    "app://-/%E0%A4%A",
  ]) {
    assert.equal(resolveAppFile(root, url), null, url);
  }
});

test("a file's type comes from its extension, any case, and unknown files are opaque", () => {
  assert.equal(mimeType("/b/index.html"), "text/html; charset=utf-8");
  assert.equal(mimeType("/b/static/js/main.abc.js"), "text/javascript; charset=utf-8");
  assert.equal(mimeType("/b/static/css/main.css"), "text/css; charset=utf-8");
  assert.equal(mimeType("/b/asset-manifest.json"), "application/json; charset=utf-8");
  assert.equal(mimeType("/b/static/js/main.abc.js.map"), "application/json; charset=utf-8");
  assert.equal(mimeType("/b/logo.SVG"), "image/svg+xml");
  assert.equal(mimeType("/b/f.woff2"), "font/woff2");
  assert.equal(mimeType("/b/favicon.ico"), "image/x-icon");
  assert.equal(mimeType("/b/run.sh"), "application/octet-stream");
  assert.equal(mimeType("/b/LICENSE"), "application/octet-stream");
});
