/* store/liveSync.js with the feed and the REST calls replaced by stand-ins the test controls, and time under the
 * test's control as well. */
import { Core, Link } from "../model/connection";
import { fromDaemon } from "../model/downloads";
import { daemonDownload } from "../testing/fixtures";
import { makeStore } from "./index";
import { selectAllDownloads, selectDownloadById } from "./downloadsSlice";
import { startLiveSync } from "./liveSync";

/* Lets every promise that can settle settle (timers are advanced by the test). */
const flush = async () => {
  for (let i = 0; i < 20; i++) await Promise.resolve();
};

function deferred() {
  const d = {};
  d.promise = new Promise((resolve, reject) => Object.assign(d, { resolve, reject }));
  return d;
}

const dl = (over) => fromDaemon(daemonDownload(over));
const progress = (id, downloadedBytes, over = {}) => ({
  type: "progress", id, status: 2, totalBytes: 1000, downloadedBytes, speedBps: 100, etaSeconds: 5, activeSegments: 4, ...over,
});
const state = (id, status, message) => ({ type: "state", id, status, ...(message ? { message } : {}) });
const complete = (id) => ({ type: "complete", id, totalBytes: 1000, elapsedMs: 5000, checksumOk: true });
const coreEvent = (up, reason = up ? "connected" : "ipc closed") => ({ type: "core", id: 0, state: up ? "up" : "down", reason });
const healthy = { ok: true, synced: true, coreConnected: true, coreRestarts: 0 };

function setup({ list = [dl({ id: 1 })], timing } = {}) {
  const store = makeStore();
  const feed = { close: jest.fn(() => Promise.resolve()) };
  let handlers;
  const t = {
    store,
    feed,
    list,
    fetchList: jest.fn(async () => t.list),
    fetchHealth: jest.fn(async () => healthy),
    h: () => handlers,
    get: (id) => selectDownloadById(store.getState(), id),
    ids: () => selectAllDownloads(store.getState()).map((d) => d.id),
    connection: () => store.getState().connection,
    /* the feed connected and subscribed, which is when the snapshot is taken */
    connect: async () => {
      handlers.onLink(Link.LIVE);
      handlers.onReady();
      await flush();
    },
    advance: async (ms) => {
      jest.advanceTimersByTime(ms);
      await flush();
    },
  };
  t.stop = startLiveSync(store, {
    open: (h) => {
      handlers = h;
      return feed;
    },
    fetchList: t.fetchList,
    fetchHealth: t.fetchHealth,
    timing,
  });
  return t;
}

beforeEach(() => jest.useFakeTimers("modern"));
afterEach(() => jest.useRealTimers());

describe("the snapshot", () => {
  test("is taken when the feed is ready, and shown", async () => {
    const t = setup({ list: [dl({ id: 2 }), dl({ id: 1 })] });
    expect(t.fetchList).not.toHaveBeenCalled();

    await t.connect();

    expect(t.fetchList).toHaveBeenCalledTimes(1);
    expect(t.ids()).toEqual([2, 1]);
    expect(t.connection()).toMatchObject({ link: Link.LIVE, syncError: "" });
  });

  test("is joined with the events that arrive while it is on its way, applied after it and in order", async () => {
    const snapshot = deferred();
    const t = setup();
    t.fetchList.mockReturnValueOnce(snapshot.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    t.h().onEvent(progress(1, 900));
    t.h().onEvent(progress(1, 950, { speedBps: 7 }));
    expect(t.get(1)).toBeUndefined(); // they wait for the snapshot
    snapshot.resolve([dl({ id: 1, status: 1, downloaded: 250 })]);
    await flush();

    expect(t.get(1)).toMatchObject({ downloaded: 950, speedBps: 7 });
  });

  test("is not set back by an event the snapshot already includes", async () => {
    const snapshot = deferred();
    const t = setup();
    t.fetchList.mockReturnValueOnce(snapshot.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    // the download started, ran, and finished while the snapshot was being made; it says "complete"
    t.h().onEvent(state(1, 1));
    t.h().onEvent(progress(1, 400));
    t.h().onEvent(complete(1));
    snapshot.resolve([dl({ id: 1, status: 3, totalSize: 1000, downloaded: 1000, speedBps: 0 })]);
    await flush();

    expect(t.get(1)).toMatchObject({ state: "complete", downloaded: 1000, speedBps: 0 });
  });

  test("a late progress frame does not reopen a download the snapshot says is complete", async () => {
    const snapshot = deferred();
    const t = setup();
    t.fetchList.mockReturnValueOnce(snapshot.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    t.h().onEvent(progress(1, 400));
    snapshot.resolve([dl({ id: 1, status: 3, totalSize: 1000, downloaded: 1000, speedBps: 0 })]);
    await flush();

    expect(t.get(1)).toMatchObject({ state: "complete", downloaded: 1000 });
  });

  test("does not move the progress of a running download back to what the database last saved", async () => {
    const t = setup({ list: [dl({ id: 1, status: 1, downloaded: 250 })] });
    await t.connect();
    t.h().onEvent(progress(1, 700));

    // the core comes back; the database was last written two seconds ago
    t.list = [dl({ id: 1, status: 1, downloaded: 400 })];
    t.h().onCore(coreEvent(true));
    await flush();

    expect(t.get(1).downloaded).toBe(700);
  });

  test("is taken again, once, when asked for several times while one is on its way", async () => {
    const first = deferred();
    const t = setup();
    t.fetchList.mockReturnValueOnce(first.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();
    expect(t.fetchList).toHaveBeenCalledTimes(1); // started at once

    t.h().onCore(coreEvent(true));
    t.h().onCore(coreEvent(true));
    expect(t.fetchList).toHaveBeenCalledTimes(1);
    first.resolve([dl({ id: 1 })]);
    await flush();

    expect(t.fetchList).toHaveBeenCalledTimes(2);
  });

  test("is taken again when too many events piled up while it was on its way", async () => {
    const first = deferred();
    const t = setup({ timing: { maxBuffered: 2 } });
    t.fetchList.mockReturnValueOnce(first.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    for (let bytes = 100; bytes <= 300; bytes += 100) t.h().onEvent(progress(1, bytes));
    first.resolve([dl({ id: 1 })]);
    await flush();

    expect(t.fetchList).toHaveBeenCalledTimes(2);
  });

  test("is taken once a minute while connected, in case an event was lost, and not otherwise", async () => {
    const t = setup();
    await t.connect();
    await t.advance(59999);
    expect(t.fetchList).toHaveBeenCalledTimes(1);
    await t.advance(1);
    expect(t.fetchList).toHaveBeenCalledTimes(2);
    await t.advance(60000);
    expect(t.fetchList).toHaveBeenCalledTimes(3);

    t.h().onLink(Link.OFFLINE);
    await t.advance(300000);
    expect(t.fetchList).toHaveBeenCalledTimes(3);
  });
});

describe("events after the snapshot", () => {
  test("are applied at once", async () => {
    const t = setup({ list: [dl({ id: 1, status: 1, downloaded: 250 })] });
    await t.connect();

    t.h().onEvent(progress(1, 600, { speedBps: 50, etaSeconds: 8 }));
    expect(t.get(1)).toMatchObject({ downloaded: 600, speedBps: 50, etaSeconds: 8, activeSegments: 4 });

    t.h().onEvent(complete(1));
    expect(t.get(1)).toMatchObject({ state: "complete", downloaded: 1000 });
  });

  test("that remove a download remove it without a snapshot", async () => {
    const t = setup({ list: [dl({ id: 1 }), dl({ id: 2 })] });
    await t.connect();

    t.h().onEvent({ type: "cancelled", id: 1 });
    t.h().onEvent({ type: "cancelled", id: 77 }); // never heard of it: nothing to remove, nothing to fetch
    await t.advance(10000);

    expect(t.ids()).toEqual([2]);
    expect(t.fetchList).toHaveBeenCalledTimes(1);
  });

  test("for a download the UI does not have ask for a snapshot soon, and once for a burst", async () => {
    const t = setup({ list: [dl({ id: 1 })] });
    await t.connect();

    t.list = [dl({ id: 9, filePath: "/d/new.iso", url: "https://example.com/new.iso" }), dl({ id: 1 })];
    t.h().onEvent(state(9, 1));
    t.h().onEvent(progress(9, 10));
    t.h().onEvent(progress(9, 20));
    expect(t.get(9)).toBeUndefined();

    await t.advance(199);
    expect(t.fetchList).toHaveBeenCalledTimes(1);
    await t.advance(1);
    expect(t.fetchList).toHaveBeenCalledTimes(2);
    expect(t.get(9)).toMatchObject({ name: "new.iso" });

    t.h().onEvent(progress(9, 30)); // known now: applied, no more snapshots
    expect(t.get(9).downloaded).toBe(30);
    await t.advance(10000);
    expect(t.fetchList).toHaveBeenCalledTimes(2);
  });

  test("for downloads that never turn up cost one snapshot every three seconds at the most", async () => {
    const t = setup({ list: [dl({ id: 1 })] });
    await t.connect();

    t.h().onEvent(progress(9, 10));
    await t.advance(200);
    expect(t.fetchList).toHaveBeenCalledTimes(2); // 9 is not in the list: it is an orphan

    t.h().onEvent(progress(9, 20)); // as soon as that snapshot is in: the next one has to wait out the three seconds
    await t.advance(2999);
    expect(t.fetchList).toHaveBeenCalledTimes(2);
    await t.advance(1);
    expect(t.fetchList).toHaveBeenCalledTimes(3);
  });

  test("that waited for the snapshot and are about a download it lacks ask for another one", async () => {
    const snapshot = deferred();
    const t = setup();
    t.fetchList.mockReturnValueOnce(snapshot.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    t.h().onEvent(state(9, 1)); // added just after the snapshot was made
    snapshot.resolve([dl({ id: 1 })]);
    await flush();
    expect(t.fetchList).toHaveBeenCalledTimes(1);
    t.list = [dl({ id: 9 }), dl({ id: 1 })];
    await t.advance(200);

    expect(t.fetchList).toHaveBeenCalledTimes(2);
    expect(t.ids()).toEqual([9, 1]);
  });
});

describe("when the snapshot cannot be had", () => {
  test("says why, tries again after growing pauses, and says nothing once it has worked", async () => {
    const t = setup();
    t.fetchList
      .mockRejectedValueOnce(new Error("The daemon is not ready."))
      .mockRejectedValueOnce(new Error("The daemon is not ready."));
    await t.connect();

    expect(t.connection().syncError).toBe("The daemon is not ready.");
    await t.advance(999);
    expect(t.fetchList).toHaveBeenCalledTimes(1);
    await t.advance(1);
    expect(t.fetchList).toHaveBeenCalledTimes(2); // fails again
    await t.advance(1999);
    expect(t.fetchList).toHaveBeenCalledTimes(2);
    await t.advance(1);
    expect(t.fetchList).toHaveBeenCalledTimes(3); // works

    expect(t.connection().syncError).toBe("");
    expect(t.ids()).toEqual([1]);
  });

  test("still applies the events that waited, to what is already there", async () => {
    const t = setup({ list: [dl({ id: 1, status: 1, downloaded: 250 })] });
    await t.connect();

    const failing = deferred();
    t.fetchList.mockReturnValueOnce(failing.promise);
    t.h().onCore(coreEvent(true)); // a snapshot is asked for...
    t.h().onEvent(progress(1, 800));
    failing.reject(new Error("The daemon is not reachable."));
    await flush();

    expect(t.get(1).downloaded).toBe(800);
    expect(t.connection().syncError).toBe("The daemon is not reachable.");
  });

  test("is not retried while the connection is down, and is taken again when it is back", async () => {
    const t = setup();
    t.fetchList.mockRejectedValueOnce(new Error("The daemon is not reachable."));
    await t.connect();
    expect(t.connection().syncError).not.toBe("");

    t.h().onLink(Link.OFFLINE);
    expect(t.connection()).toMatchObject({ link: Link.OFFLINE, core: Core.UNKNOWN, syncError: "" });
    await t.advance(120000);
    expect(t.fetchList).toHaveBeenCalledTimes(1);

    await t.connect();
    expect(t.fetchList).toHaveBeenCalledTimes(2);
    expect(t.ids()).toEqual([1]);
  });
});

describe("the core", () => {
  test("is read from the daemon's health when the feed is ready", async () => {
    const t = setup();
    await t.connect();
    expect(t.connection()).toMatchObject({ core: Core.UP, coreReason: "" });

    const down = setup();
    down.fetchHealth.mockResolvedValue({ ok: false, synced: false, coreConnected: false, coreRestarts: 2 });
    await down.connect();
    expect(down.connection()).toMatchObject({ core: Core.DOWN, coreReason: "not connected" });

    const behind = setup();
    behind.fetchHealth.mockResolvedValue({ ok: false, synced: false, coreConnected: true, coreRestarts: 0 });
    await behind.connect();
    expect(behind.connection()).toMatchObject({ core: Core.DOWN, coreReason: "not in step with the daemon yet" });
  });

  test("stays unknown when the health cannot be read", async () => {
    const t = setup();
    t.fetchHealth.mockRejectedValue(new Error("The daemon is not reachable."));
    await t.connect();
    expect(t.connection().core).toBe(Core.UNKNOWN);
  });

  test("follows the daemon's events, and a core that is back means a new snapshot", async () => {
    const t = setup();
    await t.connect();

    t.h().onCore(coreEvent(false, "ipc closed"));
    expect(t.connection()).toMatchObject({ core: Core.DOWN, coreReason: "ipc closed" });
    expect(t.fetchList).toHaveBeenCalledTimes(1);

    t.h().onCore(coreEvent(true));
    await flush();
    expect(t.connection()).toMatchObject({ core: Core.UP, coreReason: "" });
    expect(t.fetchList).toHaveBeenCalledTimes(2);
  });

  test("is not overruled by a health answer that is older than an event", async () => {
    const health = deferred();
    const t = setup();
    t.fetchHealth.mockReturnValueOnce(health.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    t.h().onCore(coreEvent(false, "ipc closed"));
    health.resolve(healthy); // it was asked before the core went down
    await flush();

    expect(t.connection().core).toBe(Core.DOWN);
  });
});

describe("stopping", () => {
  test("closes the feed and ignores whatever is still on its way", async () => {
    const snapshot = deferred();
    const t = setup();
    t.fetchList.mockReturnValueOnce(snapshot.promise);
    t.h().onLink(Link.LIVE);
    t.h().onReady();

    t.stop();
    snapshot.resolve([dl({ id: 1 })]);
    await flush();
    t.h().onEvent(progress(1, 5));
    t.h().onLink(Link.OFFLINE);
    t.h().onCore(coreEvent(false));
    await t.advance(300000);

    expect(t.feed.close).toHaveBeenCalledTimes(1);
    expect(t.ids()).toEqual([]);
    expect(t.connection().link).toBe(Link.LIVE); // the last thing it was told before it was stopped
    expect(t.fetchList).toHaveBeenCalledTimes(1);
  });
});
