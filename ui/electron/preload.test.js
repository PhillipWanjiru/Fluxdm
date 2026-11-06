const test = require("node:test");
const assert = require("node:assert/strict");
const Module = require("module");
const { TOKEN_CHANNEL } = require("./shell");

/* Runs preload.js against a stand-in for "electron" that records what it exposes and what it sends. */
function runPreload() {
  const exposed = [];
  const invoked = [];
  const electron = {
    contextBridge: { exposeInMainWorld: (name, api) => exposed.push({ name, api }) },
    ipcRenderer: {
      invoke: async (...args) => {
        invoked.push(args);
        return "the-token";
      },
    },
  };
  const file = require.resolve("./preload");
  const load = Module._load;
  Module._load = function (request, ...rest) {
    return request === "electron" ? electron : load.call(this, request, ...rest);
  };
  try {
    delete require.cache[file];
    require(file);
  } finally {
    Module._load = load;
    delete require.cache[file];
  }
  return { exposed, invoked };
}

test("the page is given one global, with one function in it", () => {
  const { exposed } = runPreload();
  assert.equal(exposed.length, 1);
  assert.equal(exposed[0].name, "fluxdm");
  assert.deepEqual(Object.keys(exposed[0].api), ["apiToken"]);
  assert.equal(typeof exposed[0].api.apiToken, "function");
});

test("asking for the token is one call on the token channel with nothing in it", async () => {
  const { exposed, invoked } = runPreload();
  assert.equal(await exposed[0].api.apiToken(), "the-token");
  assert.deepEqual(invoked, [[TOKEN_CHANNEL]]);
});
