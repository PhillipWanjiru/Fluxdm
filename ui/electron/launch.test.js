const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("fs");
const path = require("path");
const { launchOptions } = require("./launch");

const where = { dirname: "/opt/fluxdm/ui/electron", homedir: "/home/user" };

test("the built page is next to electron/, the preload beside it, and the token where the daemon puts it", () => {
  assert.deepEqual(launchOptions({}, where), {
    dev: false,
    buildDir: "/opt/fluxdm/ui/build",
    tokenFile: "/home/user/.fluxdm/api-token",
    preload: "/opt/fluxdm/ui/electron/preload.js",
  });
});

test("NODE_ENV=development shows the development server, and no other value does", () => {
  assert.equal(launchOptions({ NODE_ENV: "development" }, where).dev, true);
  for (const value of ["production", "test", "", "Development", undefined]) {
    assert.equal(launchOptions({ NODE_ENV: value }, where).dev, false, String(value));
  }
});

test("FLUXDM_TOKEN_FILE names the token file; an empty one names nothing", () => {
  assert.equal(launchOptions({ FLUXDM_TOKEN_FILE: "/run/secret/token" }, where).tokenFile, "/run/secret/token");
  assert.equal(launchOptions({ FLUXDM_TOKEN_FILE: "" }, where).tokenFile, "/home/user/.fluxdm/api-token");
});

/* The daemon writes the token and the shell reads it, so they have to mean the same file. The checkout may hold only
 * the UI, in which case there is nothing to compare with. */
const guard = path.join(__dirname, "..", "..", "daemon", "src", "main", "java", "com", "fluxdm", "security", "ApiGuard.java");

test("the shell reads the token where the daemon writes it", { skip: !fs.existsSync(guard) && "the daemon's sources are not here" }, () => {
  assert.match(fs.readFileSync(guard, "utf8"), /"\.fluxdm",\s*"api-token"/);
  assert.equal(path.relative(where.homedir, launchOptions({}, where).tokenFile), path.join(".fluxdm", "api-token"));
});
