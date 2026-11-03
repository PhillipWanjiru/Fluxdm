import reducer, { eventReceived, received, removed, selectCounts, selectTab, upserted } from "./downloadsSlice";
import { State, fromDaemon } from "../model/downloads";
import { daemonDownload, daemonList } from "../testing/fixtures";

const list = () => daemonList().map(fromDaemon);
const wrap = (downloads) => ({ downloads });
const initial = () => reducer(undefined, { type: "@@init" });
const loaded = () => reducer(initial(), received(list()));
const idsOf = (state) => state.ids;

describe("downloads slice", () => {
  test("keeps the list newest first, and a new list replaces the old one", () => {
    const state = loaded();
    expect(idsOf(state)).toEqual([5, 4, 3, 2, 1]);
    const replaced = reducer(state, received([fromDaemon(daemonDownload({ id: 9 }))]));
    expect(idsOf(replaced)).toEqual([9]);
  });

  test("upserted adds a download and then updates it in place", () => {
    let state = reducer(initial(), upserted(fromDaemon(daemonDownload({ id: 2, status: 0 }))));
    expect(state.entities[2].state).toBe(State.QUEUED);
    state = reducer(state, upserted(fromDaemon(daemonDownload({ id: 2, status: 2 }))));
    expect(idsOf(state)).toEqual([2]);
    expect(state.entities[2].state).toBe(State.PAUSED);
  });

  test("removed takes a download out", () => {
    expect(idsOf(reducer(loaded(), removed(3)))).toEqual([5, 4, 2, 1]);
  });

  describe("eventReceived", () => {
    test("applies progress to the download it names", () => {
      const state = reducer(loaded(), eventReceived({ type: "progress", id: 1, totalBytes: 1000, downloadedBytes: 600, speedBps: 40, etaSeconds: 10, activeSegments: 8 }));
      expect(state.entities[1]).toMatchObject({ downloaded: 600, speedBps: 40, etaSeconds: 10 });
      expect(state.entities[2].downloaded).toBe(250); // the others are untouched
    });

    test("a state event changes the state", () => {
      const state = reducer(loaded(), eventReceived({ type: "state", id: 1, status: 2 }));
      expect(state.entities[1].state).toBe(State.PAUSED);
    });

    test("cancelled removes the download", () => {
      expect(idsOf(reducer(loaded(), eventReceived({ type: "cancelled", id: 2 })))).toEqual([5, 4, 3, 1]);
    });

    test("events about unknown downloads, the core, or nothing leave the state as it was", () => {
      const state = loaded();
      expect(reducer(state, eventReceived({ type: "progress", id: 99, downloadedBytes: 1 }))).toBe(state);
      expect(reducer(state, eventReceived({ type: "core", id: 0, state: "down", reason: "gone" }))).toBe(state);
      expect(reducer(state, eventReceived({ type: "core", state: "up" }))).toBe(state);
      expect(reducer(state, eventReceived(null))).toBe(state);
    });

    test("an event that changes nothing leaves the state as it was", () => {
      const state = loaded();
      expect(reducer(state, eventReceived({ type: "progress", id: 4, downloadedBytes: 3 }))).toBe(state); // 4 is complete
    });
  });

  test("selectors count and filter by tab", () => {
    const state = wrap(loaded());
    expect(selectCounts(state)).toEqual({ total: 5, active: 2, queued: 1, complete: 1, failed: 1 });
    expect(selectTab(state, "active").map((d) => d.id)).toEqual([2, 1]);
    expect(selectTab(state, "failed").map((d) => d.error)).toEqual(["connection reset by peer"]);
    expect(selectTab(state, "nonsense")).toEqual([]);
  });
});
