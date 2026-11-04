import React from "react";
import { act, render } from "@testing-library/react";
import { Provider } from "react-redux";
import { fromDaemon } from "../model/downloads";
import { daemonDownload } from "../testing/fixtures";
import { makeStore } from "../store";
import { received } from "../store/downloadsSlice";
import { useSpeedHistory } from "./useSpeedHistory";

beforeEach(() => jest.useFakeTimers("modern"));
afterEach(() => jest.useRealTimers());

function setup(windowSize) {
  const store = makeStore();
  const seen = { history: [] };
  function Probe() {
    seen.history = useSpeedHistory(windowSize);
    return null;
  }
  const { unmount } = render(
    <Provider store={store}>
      <Probe />
    </Provider>
  );
  const tick = (seconds = 1) => act(() => void jest.advanceTimersByTime(seconds * 1000));
  const set = (downloads) => act(() => void store.dispatch(received(downloads.map((d) => fromDaemon(daemonDownload(d))))));
  return { seen, tick, set, unmount };
}

const speeds = (seen) => seen.history.map((p) => p.speed);

describe("useSpeedHistory", () => {
  test("starts flat, one point for every second of the window", () => {
    const { seen } = setup(5);
    expect(speeds(seen)).toEqual([0, 0, 0, 0, 0]);
  });

  test("takes the combined speed of the running downloads once a second, and drops the oldest point", () => {
    const { seen, tick, set } = setup(5);
    set([
      { id: 1, status: 1, speedBps: 100 },
      { id: 2, status: 1, speedBps: 300 },
      { id: 3, status: 2, speedBps: 999 }, // paused: not part of it
    ]);

    tick();
    expect(speeds(seen)).toEqual([0, 0, 0, 0, 400]);
    tick(2);
    expect(speeds(seen)).toEqual([0, 0, 400, 400, 400]);
    tick(3);
    expect(speeds(seen)).toEqual([400, 400, 400, 400, 400]);
    expect(seen.history).toHaveLength(5);
  });

  test("follows the downloads as they finish, and each point has its own time", () => {
    const { seen, tick, set } = setup(3);
    set([{ id: 1, status: 1, speedBps: 50 }]);
    tick();
    set([{ id: 1, status: 3, totalSize: 1000, downloaded: 1000, speedBps: 0 }]);
    tick();

    expect(speeds(seen)).toEqual([0, 50, 0]);
    expect(new Set(seen.history.map((p) => p.t)).size).toBe(3);
  });

  test("stops sampling when it is unmounted", () => {
    const { tick, unmount } = setup(3);
    unmount();
    expect(() => tick(5)).not.toThrow();
    expect(jest.getTimerCount()).toBe(0);
  });
});
