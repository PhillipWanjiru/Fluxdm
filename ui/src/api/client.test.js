/* The token handling in api/client.js: what the daemon's 401 rule (SECURITY.md) needs from the UI. */
const { AxiosError } = require("axios");

const TOKEN_A = "a".repeat(64);
const TOKEN_B = "b".repeat(64);

/* The module caches the token for the life of the page, so every test gets its own copy. */
function loadClient() {
  let client;
  jest.isolateModules(() => {
    ({ client } = require("./client"));
  });
  return client;
}

/* A daemon stand-in behind axios' adapter: it records the Authorization header of every request and answers
 * 401 to everything except `Bearer <accepts()>`. */
function fakeDaemon(client, accepts) {
  const seen = [];
  client.defaults.adapter = async (config) => {
    const auth = config.headers.get("Authorization") || null;
    seen.push(auth);
    const good = auth === `Bearer ${accepts()}`;
    const response = {
      data: good ? { status: "ok" } : { error: "unauthorized" },
      status: good ? 200 : 401,
      statusText: good ? "OK" : "Unauthorized",
      headers: {},
      config,
    };
    if (config.validateStatus(response.status)) return response;
    throw new AxiosError(`Request failed with status code ${response.status}`, AxiosError.ERR_BAD_REQUEST, config, null, response);
  };
  return seen;
}

const electronToken = (...tokens) => {
  const read = jest.fn();
  tokens.forEach((t) => read.mockResolvedValueOnce(t));
  read.mockResolvedValue(tokens[tokens.length - 1]);
  window.fluxdm = { apiToken: read };
  return read;
};

describe("api client token handling", () => {
  let consoleError;
  beforeEach(() => {
    consoleError = jest.spyOn(console, "error").mockImplementation(() => {});
  });
  afterEach(() => {
    consoleError.mockRestore();
    delete window.fluxdm;
    delete process.env.REACT_APP_FLUXDM_TOKEN;
  });

  test("sends the token the Electron main process read, and reads it only once", async () => {
    const read = electronToken(TOKEN_A);
    const client = loadClient();
    const seen = fakeDaemon(client, () => TOKEN_A);

    await client.get("/health");
    await client.get("/stats");

    expect(seen).toEqual([`Bearer ${TOKEN_A}`, `Bearer ${TOKEN_A}`]);
    expect(read).toHaveBeenCalledTimes(1);
  });

  test("reads the token again after a 401 and retries once with the new one", async () => {
    // the daemon was restarted and wrote a new token after the page had loaded
    const read = electronToken(TOKEN_A, TOKEN_B);
    const client = loadClient();
    const seen = fakeDaemon(client, () => TOKEN_B);

    const response = await client.get("/health");

    expect(response.status).toBe(200);
    expect(seen).toEqual([`Bearer ${TOKEN_A}`, `Bearer ${TOKEN_B}`]);
    expect(read).toHaveBeenCalledTimes(2);
  });

  test("gives up after a 401 when the token has not changed", async () => {
    electronToken(TOKEN_A);
    const client = loadClient();
    const seen = fakeDaemon(client, () => TOKEN_B);

    await expect(client.get("/health")).rejects.toMatchObject({ name: "ApiError", kind: "unauthorized", status: 401 });
    expect(seen).toEqual([`Bearer ${TOKEN_A}`]);
  });

  test("sends no Authorization header while no token is available", async () => {
    electronToken("");
    const client = loadClient();
    const seen = fakeDaemon(client, () => TOKEN_A);

    await expect(client.get("/health")).rejects.toMatchObject({ name: "ApiError", kind: "unauthorized", status: 401 });
    expect(seen).toEqual([null]);
  });

  test("takes a token from REACT_APP_FLUXDM_TOKEN in development only", async () => {
    process.env.REACT_APP_FLUXDM_TOKEN = TOKEN_A;
    const nodeEnv = process.env.NODE_ENV;
    try {
      process.env.NODE_ENV = "development";
      let client = loadClient();
      let seen = fakeDaemon(client, () => TOKEN_A);
      await client.get("/health");
      expect(seen).toEqual([`Bearer ${TOKEN_A}`]);

      process.env.NODE_ENV = "production";
      client = loadClient();
      seen = fakeDaemon(client, () => TOKEN_A);
      await expect(client.get("/health")).rejects.toMatchObject({ name: "ApiError", kind: "unauthorized", status: 401 });
      expect(seen).toEqual([null]);
    } finally {
      process.env.NODE_ENV = nodeEnv;
    }
  });
});
