const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("fs");
const os = require("os");
const path = require("path");
const { startShell, TOKEN_CHANNEL } = require("./shell");
const { contentSecurityPolicy } = require("./security");

/* A stand-in for the parts of Electron the shell uses. It does nothing but record what it is asked to do, and hands
 * the callbacks the shell registers back to the test, which calls them as Electron would. */
function standIn({ lock = true } = {}) {
  const seen = {
    calls: [],
    privileged: null,
    ipc: new Map(),
    appEvents: new Map(),
    errorBoxes: [],
    quits: 0,
    protocolHandler: null,
    permissionRequest: null,
    permissionCheck: null,
  };
  const windows = [];

  class BrowserWindow {
    constructor(options) {
      this.options = options;
      this.url = null;
      this.listeners = new Map();
      this.openHandler = null;
      this.minimized = false;
      this.restored = 0;
      this.focused = 0;
      this.webContents = {
        on: (name, listener) => this.listeners.set(name, listener),
        setWindowOpenHandler: (handler) => {
          this.openHandler = handler;
        },
      };
      windows.push(this);
    }
    static getAllWindows() {
      return windows;
    }
    loadURL(url) {
      this.url = url;
    }
    isMinimized() {
      return this.minimized;
    }
    restore() {
      this.restored += 1;
    }
    focus() {
      this.focused += 1;
    }
  }

  const electron = {
    app: {
      requestSingleInstanceLock: () => {
        seen.calls.push("lock");
        return lock;
      },
      quit: () => {
        seen.quits += 1;
      },
      on: (name, listener) => seen.appEvents.set(name, listener),
      whenReady: () => {
        seen.calls.push("whenReady");
        return Promise.resolve();
      },
    },
    BrowserWindow,
    dialog: { showErrorBox: (title, text) => seen.errorBoxes.push({ title, text }) },
    ipcMain: { handle: (channel, handler) => seen.ipc.set(channel, handler) },
    protocol: {
      registerSchemesAsPrivileged: (schemes) => {
        seen.calls.push("privileged");
        seen.privileged = schemes;
      },
    },
    session: {
      defaultSession: {
        protocol: { handle: (scheme, handler) => (seen.protocolHandler = { scheme, handler }) },
        setPermissionRequestHandler: (handler) => (seen.permissionRequest = handler),
        setPermissionCheckHandler: (handler) => (seen.permissionCheck = handler),
      },
    },
  };
  return { electron, seen, windows };
}

const settle = () => new Promise((resolve) => setImmediate(resolve));

async function start(t, { dev = false, built = true, token = " s3cret\n", lock = true, platform = "linux" } = {}) {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "fluxdm-shell-"));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  const buildDir = path.join(dir, "build");
  fs.mkdirSync(buildDir);
  if (built) fs.writeFileSync(path.join(buildDir, "index.html"), "<title>FluxDM</title>");
  const tokenFile = path.join(dir, "api-token");
  if (token !== null) fs.writeFileSync(tokenFile, token);

  const fake = standIn({ lock });
  startShell({ electron: fake.electron, dev, buildDir, tokenFile, preload: "/app/preload.js", platform });
  await settle();
  return { ...fake, buildDir, tokenFile };
}

/* An Electron navigation event, and whether the listener refused it. */
function navigate(listener, url, { shape = "event.url" } = {}) {
  const event = { prevented: false, preventDefault() { this.prevented = true; } };
  if (shape === "event.url") event.url = url;
  listener(event, url);
  return event.prevented;
}

const fromPage = (url, parent = null) => ({ senderFrame: { url, parent } });

test("the app:// scheme is registered as standard and secure, before the app is ready", async (t) => {
  const { seen } = await start(t);
  assert.deepEqual(seen.privileged, [{ scheme: "app", privileges: { standard: true, secure: true, supportFetchAPI: true } }]);
  assert.ok(seen.calls.indexOf("privileged") < seen.calls.indexOf("whenReady"));
});

test("one window opens on the packaged page", async (t) => {
  const { windows } = await start(t);
  assert.equal(windows.length, 1);
  assert.equal(windows[0].url, "app://-/");
});

test("in development the window shows the development server", async (t) => {
  const { windows } = await start(t, { dev: true, built: false });
  assert.equal(windows.length, 1);
  assert.equal(windows[0].url, "http://localhost:3000");
});

test("the page runs sandboxed, without Node, with the one-function preload", async (t) => {
  const { windows } = await start(t);
  assert.deepEqual(windows[0].options.webPreferences, {
    preload: "/app/preload.js",
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: true,
    webSecurity: true,
    allowRunningInsecureContent: false,
    webviewTag: false,
  });
});

test("without a build there is a message saying what to do, not a window that says Not found", async (t) => {
  const { windows, seen } = await start(t, { built: false });
  assert.equal(windows.length, 0);
  assert.equal(seen.errorBoxes.length, 1);
  assert.match(seen.errorBoxes[0].text, /npm run build/);
  assert.equal(seen.quits, 1);
});

test("a second copy quits and leaves the first one alone", async (t) => {
  const { windows, seen } = await start(t, { lock: false });
  assert.equal(seen.quits, 1);
  assert.equal(windows.length, 0);
  assert.equal(seen.calls.includes("whenReady"), false);
  assert.equal(seen.ipc.size, 0);
});

test("the first copy raises its window when a second one is started", async (t) => {
  const { windows, seen } = await start(t);
  windows[0].minimized = true;
  seen.appEvents.get("second-instance")();
  assert.equal(windows[0].restored, 1);
  assert.equal(windows[0].focused, 1);
  windows[0].minimized = false;
  seen.appEvents.get("second-instance")();
  assert.equal(windows[0].restored, 1);
  assert.equal(windows[0].focused, 2);
});

test("a second copy finding no window does nothing", async (t) => {
  const { windows, seen } = await start(t);
  windows.length = 0;
  assert.doesNotThrow(() => seen.appEvents.get("second-instance")());
});

test("the page cannot be navigated or redirected to anywhere but itself", async (t) => {
  const { windows } = await start(t);
  const { listeners } = windows[0];
  for (const name of ["will-navigate", "will-redirect"]) {
    assert.equal(navigate(listeners.get(name), "app://-/index.html"), false, name);
    // eslint-disable-next-line no-script-url -- a javascript: address is exactly what must not be followed
    for (const url of ["https://example.com/", "http://localhost:3000/", "file:///etc/passwd", "app://evil/", "javascript:alert(1)"]) {
      assert.equal(navigate(listeners.get(name), url), true, `${name} ${url}`);
    }
  }
});

test("the address is read from the event, or from the argument older Electron passes", async (t) => {
  const { windows } = await start(t);
  const listener = windows[0].listeners.get("will-navigate");
  assert.equal(navigate(listener, "https://example.com/", { shape: "argument" }), true);
  assert.equal(navigate(listener, "app://-/", { shape: "argument" }), false);
});

test("in development the development server may be navigated to, and in the package it may not", async (t) => {
  const dev = await start(t, { dev: true, built: false });
  assert.equal(navigate(dev.windows[0].listeners.get("will-navigate"), "http://localhost:3000/x"), false);
  assert.equal(navigate(dev.windows[0].listeners.get("will-navigate"), "http://localhost:3001/"), true);
  const packaged = await start(t);
  assert.equal(navigate(packaged.windows[0].listeners.get("will-navigate"), "http://localhost:3000/x"), true);
});

test("the page cannot open windows or attach webviews", async (t) => {
  const { windows } = await start(t);
  assert.deepEqual(windows[0].openHandler({ url: "https://example.com/" }), { action: "deny" });
  assert.deepEqual(windows[0].openHandler({ url: "app://-/" }), { action: "deny" });
  const event = { prevented: false, preventDefault() { this.prevented = true; } };
  windows[0].listeners.get("will-attach-webview")(event, {}, {});
  assert.equal(event.prevented, true);
});

test("no permission is granted or reported as granted", async (t) => {
  const { seen } = await start(t);
  for (const permission of ["media", "geolocation", "notifications", "clipboard-read", "openExternal"]) {
    const answers = [];
    seen.permissionRequest({}, permission, (granted) => answers.push(granted));
    assert.deepEqual(answers, [false], permission);
    assert.equal(seen.permissionCheck({}, permission, "app://-"), false, permission);
  }
});

test("the page is told the token, trimmed", async (t) => {
  const { seen } = await start(t);
  assert.equal(await seen.ipc.get(TOKEN_CHANNEL)(fromPage("app://-/index.html")), "s3cret");
});

test("the token is not told to a frame inside the page, to a page that is not the app's, or to nobody", async (t) => {
  const { seen } = await start(t);
  const ask = seen.ipc.get(TOKEN_CHANNEL);
  assert.equal(await ask(fromPage("app://-/index.html", { url: "app://-/index.html" })), "");
  assert.equal(await ask(fromPage("https://example.com/")), "");
  assert.equal(await ask(fromPage("http://localhost:3000/")), "");
  assert.equal(await ask(fromPage("file:///tmp/evil.html")), "");
  assert.equal(await ask(fromPage("")), "");
  assert.equal(await ask({ senderFrame: null }), "");
  assert.equal(await ask({}), "");
});

test("in development the development server's page is told the token too", async (t) => {
  const { seen } = await start(t, { dev: true, built: false });
  assert.equal(await seen.ipc.get(TOKEN_CHANNEL)(fromPage("http://localhost:3000/")), "s3cret");
  assert.equal(await seen.ipc.get(TOKEN_CHANNEL)(fromPage("http://localhost:3001/")), "");
});

test("a token the daemon has not written yet is an empty answer, not an error", async (t) => {
  const { seen } = await start(t, { token: null });
  assert.equal(await seen.ipc.get(TOKEN_CHANNEL)(fromPage("app://-/")), "");
});

test("the token is read afresh each time, so a daemon that starts later is found", async (t) => {
  const { seen, tokenFile } = await start(t, { token: null });
  const ask = () => seen.ipc.get(TOKEN_CHANNEL)(fromPage("app://-/"));
  assert.equal(await ask(), "");
  fs.writeFileSync(tokenFile, "late\n");
  assert.equal(await ask(), "late");
});

test("app:// requests are answered from the build, with the policy", async (t) => {
  const { seen } = await start(t);
  assert.equal(seen.protocolHandler.scheme, "app");
  const response = await seen.protocolHandler.handler({ url: "app://-/", method: "GET" });
  assert.equal(response.status, 200);
  assert.equal(await response.text(), "<title>FluxDM</title>");
  assert.equal(response.headers.get("content-security-policy"), contentSecurityPolicy());
  assert.equal(response.headers.get("content-type"), "text/html; charset=utf-8");
});

test("app:// requests for anything outside the build, or by any other method, are refused", async (t) => {
  const { seen } = await start(t);
  const handle = seen.protocolHandler.handler;
  assert.equal((await handle({ url: "app://-/..%2f..%2fetc/passwd", method: "GET" })).status, 404);
  assert.equal((await handle({ url: "app://-/missing.js", method: "GET" })).status, 404);
  assert.equal((await handle({ url: "app://-/", method: "POST" })).status, 405);
});

test("closing the last window quits, except where apps are expected to stay open", async (t) => {
  const linux = await start(t);
  linux.seen.appEvents.get("window-all-closed")();
  assert.equal(linux.seen.quits, 1);
  const mac = await start(t, { platform: "darwin" });
  mac.seen.appEvents.get("window-all-closed")();
  assert.equal(mac.seen.quits, 0);
});

test("clicking the app's icon with no window open opens one", async (t) => {
  const { windows, seen } = await start(t);
  seen.appEvents.get("activate")();
  assert.equal(windows.length, 1);
  windows.length = 0;
  seen.appEvents.get("activate")();
  assert.equal(windows.length, 1);
});
