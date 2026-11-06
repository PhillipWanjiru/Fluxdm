const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("fs");
const path = require("path");
const { APP_ORIGIN, APP_SCHEME, DAEMON_ORIGIN, DEV_ORIGIN } = require("./config");

test("the page's own origin is the scheme with the host the daemon is told", () => {
  assert.equal(APP_ORIGIN, `${APP_SCHEME}://-`);
});

/* The daemon's own settings are the truth about where it listens and whom it lets in. The checkout may hold only the
 * UI, in which case there is nothing to compare with. */
const properties = path.join(__dirname, "..", "..", "daemon", "src", "main", "resources", "application.properties");
const present = fs.existsSync(properties);

test("the daemon listens where the shell says, and allows the origins the shell uses", { skip: !present && "the daemon's sources are not here" }, () => {
  const text = fs.readFileSync(properties, "utf8");
  const setting = (key) => {
    const line = text.split(/\r?\n/).find((l) => l.startsWith(`${key}=`));
    assert.ok(line, `${key} is not set in application.properties`);
    return line.slice(key.length + 1).trim();
  };
  assert.equal(`http://${setting("server.address")}:${setting("server.port")}`, DAEMON_ORIGIN);
  const allowed = setting("fluxdm.api.allowed-origins").split(",").map((origin) => origin.trim());
  assert.ok(allowed.includes(APP_ORIGIN), `${APP_ORIGIN} is not among ${allowed}`);
  assert.ok(allowed.includes(DEV_ORIGIN), `${DEV_ORIGIN} is not among ${allowed}`);
});
