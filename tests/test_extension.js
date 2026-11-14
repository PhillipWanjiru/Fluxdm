"use strict";
/* FluxDM — tests/test_extension.js
 *
 * The browser extension's background script, run against a stand-in for the chrome.* APIs (no browser needed):
 *
 *   node --test tests/test_extension.js        (Node 18 or newer, no packages)
 *
 * The rule they pin down: the browser's own download is cancelled only after FluxDM said it took it. Every other
 * outcome (no host, a crashing host, a silent host, a host that says anything but "accepted") must leave the
 * download alone. The extension used to cancel every download and hand it to nothing.
 */
const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const EXT = path.join(__dirname, "..", "extension");
const BACKGROUND = path.join(EXT, "background", "background.js");
const bg = require(BACKGROUND);

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

/** A stand-in for chrome.runtime and chrome.downloads. `host` decides what the native host does:
 *  { reply } answers, { lastError } fails the way a missing or crashed host does, { throws } makes
 *  sendNativeMessage itself throw, { silent: true } never answers. */
function fakeChrome(host = {}) {
  const calls = { native: [], cancel: [], erase: [], order: [] };
  const listeners = [];
  const api = {
    runtime: {
      lastError: undefined,
      sendNativeMessage(name, message, callback) {
        calls.native.push({ name, message });
        calls.order.push("native");
        if (host.throws) throw new Error("Access to the specified native messaging host is forbidden.");
        if (host.silent) return;
        setImmediate(() => {
          api.runtime.lastError = host.lastError ? { message: host.lastError } : undefined;
          callback(host.lastError ? undefined : host.reply);
          api.runtime.lastError = undefined;
        });
      },
    },
    downloads: {
      onCreated: { addListener: fn => listeners.push(fn) },
      cancel(id, callback) { calls.cancel.push(id); calls.order.push("cancel"); setImmediate(callback); },
      erase(query, callback) { calls.erase.push(query); calls.order.push("erase"); setImmediate(callback); },
    },
  };
  return { api, calls, listeners };
}

const download = (extra = {}) => ({ id: 7, url: "https://files.example/a.iso", incognito: false, ...extra });

test("which downloads are offered", () => {
  const yes = [
    "https://files.example/a.iso", "http://files.example/a.iso", "HTTP://FILES.EXAMPLE/A", "https://h:8443/p?q=1#f",
    "https://user:pw@h/x", "http://127.0.0.1:7171/x", "http://[::1]/x",
  ];
  const no = [
    "", "blob:https://files.example/3f2a", "data:text/plain;base64,QQ==", "file:///etc/passwd", "ftp://h/x",
    "javascript:alert(1)", "chrome-extension://abc/x", "about:blank", "https://", "//h/x", "https:h/x", "h/x",
    "https://h/a b", "https://h/a\nb", "https://h/a\u0000b", "https://h/\u007f", "https://h/\u0085", " https://h/x",
    "https://h/x ", "https://" + "a".repeat(bg.MAX_URL), "x".repeat(bg.MAX_URL + 1), undefined, null, 42, {}, ["https://h/x"],
  ];
  for (const url of yes) assert.equal(bg.eligible({ url }), true, `offer ${JSON.stringify(url)}`);
  for (const url of no) assert.equal(bg.eligible({ url }), false, `do not offer ${JSON.stringify(url)}`);
  assert.equal(bg.eligible(undefined), false);
  assert.equal(bg.eligible(null), false);
  assert.equal(bg.eligible(download({ incognito: true })), false, "a private window's download is never offered");
  assert.equal(bg.eligible(download({ incognito: false })), true);
  assert.equal(bg.eligible({ url: "https://h/x" }), true, "incognito absent means not private");
});

test("what is sent about a download", () => {
  assert.deepEqual(bg.request(download()), { type: "download", url: "https://files.example/a.iso" });
  assert.deepEqual(
    bg.request(download({ finalUrl: "https://cdn.example/a.iso?sig=1", referrer: "https://files.example/page", mime: "application/x-iso9660-image", fileSize: 123456 })),
    { type: "download", url: "https://files.example/a.iso", final_url: "https://cdn.example/a.iso?sig=1",
      referrer: "https://files.example/page", mime: "application/x-iso9660-image", size: 123456 });
  // fields that fail their check are left out, not passed on
  assert.deepEqual(
    bg.request(download({ finalUrl: "https://files.example/a.iso", referrer: "javascript:1", mime: "", fileSize: -1 })),
    { type: "download", url: "https://files.example/a.iso" }, "same final address, bad referrer, empty type, negative size");
  assert.deepEqual(bg.request(download({ finalUrl: "blob:x", mime: "a".repeat(256), fileSize: 1.5 })),
    { type: "download", url: "https://files.example/a.iso" }, "bad final address, long type, fractional size");
  const sent = Object.keys(bg.request(download({ cookie: "s=1", headers: { A: "b" }, filename: "/etc/x", state: "in_progress" })));
  assert.deepEqual(sent, ["type", "url"], "nothing else the browser object carries is forwarded");
});

test("a download FluxDM accepts is cancelled and removed, after the answer", async () => {
  const { api, calls } = fakeChrome({ reply: { status: "accepted" } });
  assert.equal(await bg.onCreated(api, download()), "handed-off");
  assert.equal(calls.native.length, 1);
  assert.equal(calls.native[0].name, "com.fluxdm.host");
  assert.deepEqual(calls.cancel, [7]);
  assert.deepEqual(calls.erase, [{ id: 7 }]);
  assert.deepEqual(calls.order, ["native", "cancel", "erase"], "the browser's copy is touched only after the host answered");
});

test("everything but an acceptance leaves the browser's download alone", async () => {
  const replies = [
    { status: "ok" }, { status: "error", error: "no" }, {}, { status: "ACCEPTED" }, { status: "accepted " },
    { status: ["accepted"] }, { status: true }, { accepted: true }, undefined, null, "accepted", 1, true, [],
  ];
  for (const reply of replies) {
    const { api, calls } = fakeChrome({ reply });
    assert.equal(await bg.onCreated(api, download()), "kept", `reply ${JSON.stringify(reply)}`);
    assert.deepEqual([calls.cancel, calls.erase], [[], []], `reply ${JSON.stringify(reply)} must not cancel`);
  }
});

test("a missing, crashing or forbidden host leaves the download alone", async () => {
  for (const host of [{ lastError: "Specified native messaging host not found." },
                      { lastError: "Native host has exited." }, { throws: true }]) {
    const { api, calls } = fakeChrome(host);
    assert.equal(await bg.onCreated(api, download()), "kept", JSON.stringify(host));
    assert.deepEqual([calls.cancel, calls.erase], [[], []]);
  }
});

test("a host that never answers does not cancel anything either", async () => {
  const { api, calls } = fakeChrome({ silent: true });
  const outcome = await Promise.race([bg.onCreated(api, download()), sleep(60).then(() => "still waiting")]);
  assert.equal(outcome, "still waiting");
  assert.deepEqual([calls.cancel, calls.erase], [[], []]);
});

test("a download that is not offered does not reach the host at all", async () => {
  for (const item of [download({ incognito: true }), download({ url: "blob:https://a/b" }), download({ url: "file:///x" }), {}, undefined]) {
    const { api, calls } = fakeChrome({ reply: { status: "accepted" } });
    assert.equal(await bg.onCreated(api, item), "ignored");
    assert.deepEqual([calls.native, calls.cancel, calls.erase], [[], [], []]);
  }
});

test("a failure to cancel does not undo the hand-off or throw", async () => {
  const { api, calls } = fakeChrome({ reply: { status: "accepted" } });
  api.downloads.cancel = (id, callback) => { calls.cancel.push(id); api.runtime.lastError = { message: "Download must be in progress" }; callback(); api.runtime.lastError = undefined; };
  assert.equal(await bg.onCreated(api, download()), "handed-off");
  assert.deepEqual(calls.erase, [{ id: 7 }]);
});

// The script as the browser runs it: no module system, a global `chrome`, and the listener it registers.
function load(chrome) {
  vm.runInNewContext(fs.readFileSync(BACKGROUND, "utf8"), { chrome, URL }, { filename: "background.js" });
}

test("the registered listener: a host that fails costs the user nothing", async () => {
  for (const host of [{ lastError: "Specified native messaging host not found." }, { reply: { status: "error" } }, { reply: { status: "ok" } }, { throws: true }]) {
    const { api, calls, listeners } = fakeChrome(host);
    load(api);
    assert.equal(listeners.length, 1, "one listener on downloads.onCreated");
    listeners[0](download());
    await sleep(40);
    assert.deepEqual([calls.cancel, calls.erase], [[], []], `host ${JSON.stringify(host)}: the browser's download stays`);
    assert.equal(calls.native.length, host.reply || host.lastError || host.throws ? 1 : 0);
  }
});

test("the registered listener: a host that accepts gets the download", async () => {
  const { api, calls, listeners } = fakeChrome({ reply: { status: "accepted" } });
  load(api);
  listeners[0](download({ id: 11 }));
  for (let i = 0; i < 50 && calls.erase.length === 0; i++) await sleep(10);
  assert.deepEqual(calls.cancel, [11]);
  assert.deepEqual(JSON.parse(JSON.stringify(calls.erase)), [{ id: 11 }]);    // made in another realm: compare as data
});

test("the registered listener never throws, whatever the browser passes it", () => {
  const { api, listeners } = fakeChrome({ reply: { status: "accepted" } });
  load(api);
  for (const item of [undefined, null, 5, "x", {}, { url: 5 }, { url: "https://h/x", incognito: true }]) {
    assert.doesNotThrow(() => listeners[0](item));
  }
});

test("the manifest asks for what the code uses and nothing else", () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(EXT, "manifest.json"), "utf8"));
  assert.equal(manifest.manifest_version, 3);
  assert.deepEqual([...manifest.permissions].sort(), ["downloads", "nativeMessaging"]);
  assert.equal(manifest.host_permissions, undefined, "no access to any site: nothing here reads a page");
  assert.equal(manifest.content_scripts, undefined, "no script is injected into every page");
  assert.equal(manifest.optional_permissions, undefined);

  const code = fs.readdirSync(path.join(EXT, "background")).concat(fs.readdirSync(path.join(EXT, "popup")).map(f => path.join("..", "popup", f)))
    .filter(f => f.endsWith(".js")).map(f => fs.readFileSync(path.join(EXT, "background", f), "utf8")).join("\n");
  const evidence = { downloads: /\.downloads\b/, nativeMessaging: /sendNativeMessage|connectNative/ };
  for (const permission of manifest.permissions) {
    assert.ok(evidence[permission], `no rule yet for how "${permission}" shows up in the code: add one next to this test`);
    assert.match(code, evidence[permission], `"${permission}" is declared but nothing uses it`);
  }
});

test("every file the manifest names exists", () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(EXT, "manifest.json"), "utf8"));
  const named = [manifest.background.service_worker, manifest.action.default_popup, manifest.action.default_icon];
  for (const file of named) assert.ok(fs.existsSync(path.join(EXT, file)), `${file} is named in the manifest but missing`);
  const popup = fs.readFileSync(path.join(EXT, "popup", "popup.html"), "utf8");
  for (const [, ref] of popup.matchAll(/(?:src|href)="([^"]+)"/g)) {
    assert.ok(fs.existsSync(path.join(EXT, "popup", ref)), `popup.html refers to ${ref}, which is missing`);
  }
  assert.ok(!fs.existsSync(path.join(EXT, "background", "native_bridge.js")), "the unused port-based bridge is gone: its job is in background.js");
});
