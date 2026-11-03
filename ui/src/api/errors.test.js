import { AxiosError, CanceledError } from "axios";
import { client } from "./client";
import { ApiError, toApiError } from "./errors";
import { installFakeDaemon } from "../testing/fakeDaemon";

const failWith = (status, data) => installFakeDaemon(client, { "GET /health": () => ({ status, data }) });
const caught = async () => {
  try {
    await client.get("/health");
  } catch (e) {
    return e;
  }
  throw new Error("the call did not fail");
};

describe("what a refusal from the daemon becomes", () => {
  test.each([
    [400, "bad_request"],
    [404, "not_found"],
    [409, "conflict"],
    [503, "unavailable"],
    [500, "server"],
    [418, "http"],
  ])("HTTP %i is %s, with the daemon's own text", async (status, kind) => {
    failWith(status, { error: "the words to show" });
    const e = await caught();
    expect(e).toBeInstanceOf(ApiError);
    expect(e).toMatchObject({ kind, status, message: "the words to show" });
  });

  test("a body without text gets a sentence of its own", async () => {
    failWith(404, "<html>whatever</html>");
    expect((await caught()).message).toBe("The daemon does not know that.");
    failWith(500, undefined);
    expect((await caught()).message).toBe("The daemon answered HTTP 500.");
  });

  test("401 and 403 say what the daemon checks", async () => {
    failWith(401, { error: "unauthorized" });
    expect(await caught()).toMatchObject({ kind: "unauthorized", status: 401 });
    failWith(403, undefined);
    const e = await caught();
    expect(e).toMatchObject({ kind: "forbidden", status: 403 });
    expect(e.message).toMatch(/Host or Origin/);
  });
});

describe("toApiError", () => {
  test("a daemon that cannot be reached", () => {
    const e = toApiError(new AxiosError("Network Error", AxiosError.ERR_NETWORK, {}, {}));
    expect(e).toMatchObject({ kind: "unreachable", status: null });
    expect(e.message).toMatch(/127\.0\.0\.1:7171/);
    expect(e.message).toMatch(/allowed-origins/);
  });

  test("a daemon that does not answer in time", () => {
    expect(toApiError(new AxiosError("timeout of 10000ms exceeded", AxiosError.ECONNABORTED, {}, {})).kind).toBe("timeout");
    expect(toApiError(new AxiosError("timeout", AxiosError.ETIMEDOUT, {}, {})).kind).toBe("timeout");
  });

  test("a request the page gave up on", () => {
    expect(toApiError(new CanceledError("canceled")).kind).toBe("cancelled"); // what an AbortController produces
    expect(toApiError(new AxiosError("canceled", AxiosError.ERR_CANCELED, {}, {})).kind).toBe("cancelled");
  });

  test("a mistake in our own code is not reported as a network problem", () => {
    const e = toApiError(new TypeError("x is not a function"));
    expect(e).toMatchObject({ kind: "unknown", message: "x is not a function" });
  });

  test("an ApiError passes through unchanged", () => {
    const original = new ApiError("already one", { kind: "conflict", status: 409 });
    expect(toApiError(original)).toBe(original);
  });

  test("keeps the original error as its cause", () => {
    const cause = new AxiosError("Network Error", AxiosError.ERR_NETWORK, {}, {});
    expect(toApiError(cause).cause).toBe(cause);
  });
});
