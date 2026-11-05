import { makeStore } from "./index";
import { selectLimitBps } from "./bandwidthSlice";
import { loadLimit, saveLimit } from "./bandwidthActions";
import { getBandwidthLimit, setBandwidthLimit } from "../api/stats";
import { ApiError } from "../api/errors";

jest.mock("../api/stats");

beforeEach(() => {
  getBandwidthLimit.mockReset();
  setBandwidthLimit.mockReset();
});

describe("loadLimit", () => {
  test("puts what the daemon says in the store", async () => {
    getBandwidthLimit.mockResolvedValue(524288);
    const store = makeStore();
    await expect(store.dispatch(loadLimit())).resolves.toBe(true);
    expect(selectLimitBps(store.getState())).toBe(524288);
  });

  test("a failure is not thrown, and leaves what the store had", async () => {
    const store = makeStore();
    getBandwidthLimit.mockResolvedValueOnce(1024);
    await store.dispatch(loadLimit());
    getBandwidthLimit.mockRejectedValueOnce(new ApiError("Cannot reach the daemon.", { kind: "unreachable" }));

    await expect(store.dispatch(loadLimit())).resolves.toBe(false);
    expect(selectLimitBps(store.getState())).toBe(1024);
  });
});

describe("saveLimit", () => {
  test("sets the limit and keeps what the daemon answers, not what was asked", async () => {
    setBandwidthLimit.mockResolvedValue(2097152);
    const store = makeStore();
    await store.dispatch(saveLimit(2097152));
    expect(setBandwidthLimit).toHaveBeenCalledWith(2097152);
    expect(selectLimitBps(store.getState())).toBe(2097152);
    expect(getBandwidthLimit).not.toHaveBeenCalled();
  });

  test("after a refusal asks the daemon what it holds, and still rejects with the reason", async () => {
    const refusal = new ApiError("The core did not answer.", { kind: "unavailable", status: 503 });
    setBandwidthLimit.mockRejectedValue(refusal);
    getBandwidthLimit.mockResolvedValue(2097152); /* the daemon stored it before it told the core */
    const store = makeStore();

    await expect(store.dispatch(saveLimit(2097152))).rejects.toBe(refusal);
    expect(getBandwidthLimit).toHaveBeenCalledTimes(1);
    expect(selectLimitBps(store.getState())).toBe(2097152);
  });

  test("if the daemon cannot be asked either, the reason of the refusal is still the one given", async () => {
    const refusal = new ApiError("Cannot reach the daemon.", { kind: "unreachable" });
    setBandwidthLimit.mockRejectedValue(refusal);
    getBandwidthLimit.mockRejectedValue(new ApiError("Cannot reach the daemon.", { kind: "unreachable" }));
    const store = makeStore();

    await expect(store.dispatch(saveLimit(1024))).rejects.toBe(refusal);
    expect(selectLimitBps(store.getState())).toBeNull();
  });
});
