import React from "react";
import { act, render } from "@testing-library/react";
import { Provider } from "react-redux";
import { makeStore } from "../store";
import { linkChanged } from "../store/connectionSlice";
import { selectLimitBps } from "../store/bandwidthSlice";
import { useBandwidthSync } from "./useBandwidthSync";
import { Link } from "../model/connection";
import { getBandwidthLimit } from "../api/stats";

jest.mock("../api/stats");

function Probe() {
  useBandwidthSync();
  return null;
}

const mount = (store) =>
  render(
    <Provider store={store}>
      <Probe />
    </Provider>
  );
const flush = () => act(async () => { for (let i = 0; i < 10; i++) await Promise.resolve(); });

beforeEach(() => {
  getBandwidthLimit.mockReset();
  getBandwidthLimit.mockResolvedValue(1048576);
});

describe("useBandwidthSync", () => {
  test("reads nothing while there is no link to the daemon", async () => {
    const store = makeStore();
    mount(store);
    await flush();
    expect(getBandwidthLimit).not.toHaveBeenCalled();

    act(() => { store.dispatch(linkChanged(Link.OFFLINE)); });
    act(() => { store.dispatch(linkChanged(Link.UNAUTHORIZED)); });
    await flush();
    expect(getBandwidthLimit).not.toHaveBeenCalled();
  });

  test("reads the limit when the link goes live, and again after every reconnect", async () => {
    const store = makeStore();
    mount(store);

    act(() => { store.dispatch(linkChanged(Link.LIVE)); });
    await flush();
    expect(getBandwidthLimit).toHaveBeenCalledTimes(1);
    expect(selectLimitBps(store.getState())).toBe(1048576);

    act(() => { store.dispatch(linkChanged(Link.OFFLINE)); });
    getBandwidthLimit.mockResolvedValue(0);
    act(() => { store.dispatch(linkChanged(Link.LIVE)); });
    await flush();
    expect(getBandwidthLimit).toHaveBeenCalledTimes(2);
    expect(selectLimitBps(store.getState())).toBe(0);
  });

  test("reads it at once when it is mounted over a link that is already live", async () => {
    const store = makeStore();
    store.dispatch(linkChanged(Link.LIVE));
    mount(store);
    await flush();
    expect(getBandwidthLimit).toHaveBeenCalledTimes(1);
  });

  test("a change to anything else about the connection does not read it again", async () => {
    const store = makeStore();
    mount(store);
    act(() => { store.dispatch(linkChanged(Link.LIVE)); });
    await flush();
    act(() => { store.dispatch({ type: "connection/coreChanged", payload: { up: false, reason: "x" } }); });
    await flush();
    expect(getBandwidthLimit).toHaveBeenCalledTimes(1);
  });
});
