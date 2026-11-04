import React from "react";
import { render } from "@testing-library/react";
import { Provider } from "react-redux";
import { makeStore } from "../store";
import { startLiveSync } from "../store/liveSync";
import { useLiveSync } from "./useLiveSync";

jest.mock("../store/liveSync");

function Probe() {
  useLiveSync();
  return null;
}

const mount = (store, strict = false) => {
  const tree = (
    <Provider store={store}>
      <Probe />
    </Provider>
  );
  return render(strict ? <React.StrictMode>{tree}</React.StrictMode> : tree);
};

beforeEach(() => startLiveSync.mockReset());

describe("useLiveSync", () => {
  test("starts the sync on the app's store when it mounts, and stops it when it unmounts", () => {
    const stop = jest.fn();
    startLiveSync.mockReturnValue(stop);
    const store = makeStore();

    const { unmount } = mount(store);
    expect(startLiveSync).toHaveBeenCalledTimes(1);
    expect(startLiveSync).toHaveBeenCalledWith(store);
    expect(stop).not.toHaveBeenCalled();

    unmount();
    expect(stop).toHaveBeenCalledTimes(1);
  });

  test("survives React's development double mount: the first sync is stopped before the second starts", () => {
    const calls = [];
    startLiveSync.mockImplementation(() => {
      const id = startLiveSync.mock.calls.length;
      calls.push(`start ${id}`);
      return () => calls.push(`stop ${id}`);
    });

    const { unmount } = mount(makeStore(), true);
    unmount();

    expect(calls).toEqual(["start 1", "stop 1", "start 2", "stop 2"]);
  });
});
