const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { respond } = require("./appProtocol");
const { contentSecurityPolicy } = require("./security");

const PAGE = "<!doctype html><title>FluxDM</title><div id=root></div>";

function makeRoot(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "fluxdm-app-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  fs.writeFileSync(path.join(root, "index.html"), PAGE);
  fs.mkdirSync(path.join(root, "static", "js"), { recursive: true });
  fs.writeFileSync(path.join(root, "static", "js", "main.js"), "console.log(1);");
  return root;
}

const get = (url) => ({ url, method: "GET" });

test("the page is served with its type and the policy", async (t) => {
  const answer = await respond(makeRoot(t), get("app://-/"));
  assert.equal(answer.status, 200);
  assert.equal(answer.body.toString(), PAGE);
  assert.equal(answer.headers["content-type"], "text/html; charset=utf-8");
  assert.equal(answer.headers["content-security-policy"], contentSecurityPolicy());
  assert.equal(answer.headers["x-content-type-options"], "nosniff");
  assert.equal(answer.headers["cache-control"], "no-store"); /* a new build is never answered from an old copy */
});

test("a script is served from its folder as a script", async (t) => {
  const answer = await respond(makeRoot(t), get("app://-/static/js/main.js"));
  assert.equal(answer.status, 200);
  assert.equal(answer.body.toString(), "console.log(1);");
  assert.equal(answer.headers["content-type"], "text/javascript; charset=utf-8");
});

test("the policy can be replaced, and the files are read through what is passed in", async () => {
  const reads = [];
  const answer = await respond("/srv/build", get("app://-/"), {
    csp: "default-src 'none'",
    readFile: async (file) => {
      reads.push(file);
      return Buffer.from("x");
    },
  });
  assert.deepEqual(reads, [path.join("/srv/build", "index.html")]);
  assert.equal(answer.headers["content-security-policy"], "default-src 'none'");
});

test("a file that is not there, and a folder, are not found", async (t) => {
  const root = makeRoot(t);
  for (const url of ["app://-/nothing.js", "app://-/static", "app://-/static/js/"]) {
    const answer = await respond(root, get(url));
    assert.equal(answer.status, 404, url);
    assert.equal(answer.headers["content-type"], "text/plain; charset=utf-8");
    assert.equal(answer.headers["x-content-type-options"], "nosniff");
    assert.equal(answer.headers["content-security-policy"], undefined);
  }
});

test("an address that names nothing in the build is refused without touching the disk", async () => {
  const reads = [];
  const readFile = async (file) => {
    reads.push(file);
    return Buffer.from("secret");
  };
  for (const url of ["file:///etc/passwd", "app://evil/index.html", "app://-/..%2f..%2fetc/passwd", "app://-/%00", "nonsense"]) {
    const answer = await respond("/srv/build", get(url), { readFile });
    assert.equal(answer.status, 404, url);
  }
  assert.deepEqual(reads, []);
});

test("anything but GET and HEAD is refused", async (t) => {
  const root = makeRoot(t);
  for (const method of ["POST", "PUT", "DELETE", "PATCH", "OPTIONS"]) {
    const answer = await respond(root, { url: "app://-/", method });
    assert.equal(answer.status, 405, method);
    assert.equal(answer.headers.allow, "GET, HEAD");
  }
});

test("HEAD gets the headers of GET and no body, and the method's case does not matter", async (t) => {
  const root = makeRoot(t);
  const head = await respond(root, { url: "app://-/", method: "head" });
  const full = await respond(root, { url: "app://-/", method: "get" });
  assert.equal(head.status, 200);
  assert.equal(head.body, null);
  assert.deepEqual(head.headers, full.headers);
  assert.equal(full.body.toString(), PAGE);
});

test("a request without a method is a GET", async (t) => {
  const answer = await respond(makeRoot(t), { url: "app://-/" });
  assert.equal(answer.status, 200);
});
