import { DaemonStatus, State, TABS, applyEvent, etaFromSpeed, fileTypeOf, fromDaemon, mergeSnapshot, percentOf, stateOf } from "./downloads";
import { daemonDownload } from "../testing/fixtures";

const model = (over) => fromDaemon(daemonDownload(over));

describe("stateOf", () => {
  test.each([
    [DaemonStatus.PENDING, State.QUEUED],
    [DaemonStatus.RUNNING, State.DOWNLOADING],
    [DaemonStatus.PAUSED, State.PAUSED],
    [DaemonStatus.DONE, State.COMPLETE],
    [DaemonStatus.ERROR, State.FAILED],
  ])("daemon status %i is %s", (code, state) => {
    expect(stateOf(code)).toBe(state);
  });

  test("a code from a newer daemon is shown as queued", () => {
    expect(stateOf(5)).toBe(State.QUEUED);
    expect(stateOf(undefined)).toBe(State.QUEUED);
    expect(stateOf("running")).toBe(State.QUEUED);
  });

  test("every state belongs to exactly one tab", () => {
    for (const state of Object.values(State)) {
      expect(Object.values(TABS).filter((states) => states.includes(state))).toHaveLength(1);
    }
  });
});

describe("fromDaemon", () => {
  test("reads a running download", () => {
    expect(fromDaemon(daemonDownload())).toEqual({
      id: 1,
      url: "https://example.com/files/ubuntu-24.04.iso",
      filePath: "/home/user/Downloads/ubuntu-24.04.iso",
      name: "ubuntu-24.04.iso",
      type: "iso",
      state: State.DOWNLOADING,
      totalSize: 1000,
      downloaded: 250,
      segments: 8,
      priority: 5,
      checksum: "",
      checksumType: 0,
      error: "",
      bandwidthLimit: 0,
      speedBps: 100,
      etaSeconds: 8, // 750 bytes left at 100 B/s, rounded up
      activeSegments: 0,
    });
  });

  test("speed and ETA only mean something while the download runs", () => {
    for (const status of [DaemonStatus.PENDING, DaemonStatus.PAUSED, DaemonStatus.DONE, DaemonStatus.ERROR]) {
      const d = model({ status, speedBps: 5000 });
      expect(d.speedBps).toBe(0);
      expect(d.etaSeconds).toBeNull();
    }
  });

  test("the error text is kept for failed downloads only", () => {
    expect(model({ status: 4, errorMessage: "connection reset by peer" }).error).toBe("connection reset by peer");
    expect(model({ status: 1, errorMessage: "left over from an earlier attempt" }).error).toBe("");
  });

  test("survives missing and null fields", () => {
    const d = fromDaemon({ id: 3 });
    expect(d).toMatchObject({
      id: 3, url: "", filePath: "", name: "", state: State.QUEUED, totalSize: 0, downloaded: 0, segments: 0,
      checksum: "", error: "", speedBps: 0, etaSeconds: null,
    });
    expect(() => fromDaemon(null)).not.toThrow();
    expect(() => fromDaemon(undefined)).not.toThrow();
  });

  test("never reports more downloaded than the total", () => {
    expect(model({ totalSize: 1000, downloaded: 1500 }).downloaded).toBe(1000);
    expect(model({ totalSize: 0, downloaded: 1500 }).downloaded).toBe(1500); // size not known yet
  });

  test("names a download after its file, then the end of its URL, then the URL", () => {
    expect(model({ filePath: "/a/b/report final.pdf" }).name).toBe("report final.pdf");
    expect(model({ filePath: "", url: "https://example.com/dir/My%20File.zip?token=1" }).name).toBe("My File.zip");
    expect(model({ filePath: "", url: "https://example.com/" }).name).toBe("https://example.com/");
    expect(model({ filePath: "", url: "https://example.com/bad%E0%A4%A.zip" }).name).toBe("https://example.com/bad%E0%A4%A.zip");
    expect(model({ filePath: "", url: "not a url" }).name).toBe("not a url");
  });

  test("does not change what it is given", () => {
    const raw = Object.freeze(daemonDownload());
    expect(() => fromDaemon(raw)).not.toThrow();
  });
});

describe("fileTypeOf", () => {
  test.each([
    ["ubuntu.iso", "iso"],
    ["backup.tar.gz", "archive"],
    ["Clip.MP4", "video"],
    ["song.flac", "audio"],
    ["paper.pdf", "pdf"],
    ["data.bin", "default"],
    ["README", "default"],
    [".hidden", "default"],
  ])("%s is %s", (name, type) => {
    expect(fileTypeOf(name)).toBe(type);
  });
});

describe("percentOf and etaFromSpeed", () => {
  test("rounds down, so nothing reads 100% before it is complete", () => {
    expect(percentOf(model({ totalSize: 1000, downloaded: 999 }))).toBe(99);
    expect(percentOf(model({ totalSize: 1000, downloaded: 0 }))).toBe(0);
  });

  test("is 0 while the size is unknown, and 100 once complete", () => {
    expect(percentOf(model({ totalSize: 0, downloaded: 500 }))).toBe(0);
    expect(percentOf(model({ status: 3, totalSize: 0, downloaded: 0 }))).toBe(100);
  });

  test("the ETA needs a speed and something left to fetch", () => {
    expect(etaFromSpeed(1000, 0, 100)).toBe(10);
    expect(etaFromSpeed(1000, 0, 0)).toBeNull();
    expect(etaFromSpeed(0, 0, 100)).toBeNull();
    expect(etaFromSpeed(1000, 1000, 100)).toBeNull();
  });
});

describe("applyEvent", () => {
  const running = () => model({ status: 1, totalSize: 1000, downloaded: 100, speedBps: 50 });

  test("progress updates the numbers and nothing else", () => {
    const before = running();
    const after = applyEvent(before, {
      type: "progress", id: 1, status: 2 /* the core's code, not the daemon's */,
      totalBytes: 1000, downloadedBytes: 400, speedBps: 300, etaSeconds: 2, activeSegments: 4,
    });
    expect(after).toMatchObject({ state: State.DOWNLOADING, downloaded: 400, speedBps: 300, etaSeconds: 2, activeSegments: 4 });
    expect(after.name).toBe(before.name);
  });

  test("an unknown ETA (-1) becomes null", () => {
    const after = applyEvent(running(), { type: "progress", id: 1, totalBytes: 1000, downloadedBytes: 100, speedBps: 0, etaSeconds: -1, activeSegments: 1 });
    expect(after.etaSeconds).toBeNull();
  });

  test("progress cannot start or resurrect a download: only state events change the state", () => {
    const paused = model({ status: 2 });
    const after = applyEvent(paused, { type: "progress", id: 1, totalBytes: 1000, downloadedBytes: 300, speedBps: 999, etaSeconds: 5, activeSegments: 3 });
    expect(after).toMatchObject({ state: State.PAUSED, downloaded: 300, speedBps: 0, etaSeconds: null, activeSegments: 0 });
  });

  test("a late progress frame does not move a finished download back", () => {
    const done = model({ status: 3, totalSize: 1000, downloaded: 1000 });
    expect(applyEvent(done, { type: "progress", id: 1, totalBytes: 1000, downloadedBytes: 900, speedBps: 1 })).toBe(done);
  });

  test("keeps the known total when a frame does not carry one, and never exceeds it", () => {
    const after = applyEvent(running(), { type: "progress", id: 1, totalBytes: 0, downloadedBytes: 5000, speedBps: 1 });
    expect(after.totalSize).toBe(1000);
    expect(after.downloaded).toBe(1000);
  });

  test("a status reply counts as progress", () => {
    const after = applyEvent(running(), { type: "status", id: 1, status: 2, totalBytes: 1000, downloadedBytes: 600, totalSegments: 8, activeSegments: 8, speedBps: 10, etaSeconds: -1 });
    expect(after).toMatchObject({ downloaded: 600, activeSegments: 8 });
  });

  test("state events move between states and clear what no longer applies", () => {
    const paused = applyEvent(running(), { type: "state", id: 1, status: DaemonStatus.PAUSED });
    expect(paused).toMatchObject({ state: State.PAUSED, speedBps: 0, etaSeconds: null, activeSegments: 0 });

    const failed = applyEvent(running(), { type: "state", id: 1, status: DaemonStatus.ERROR, message: "disk full" });
    expect(failed).toMatchObject({ state: State.FAILED, error: "disk full", speedBps: 0 });

    const retried = applyEvent(failed, { type: "state", id: 1, status: DaemonStatus.PENDING });
    expect(retried).toMatchObject({ state: State.QUEUED, error: "" });
  });

  test("the error event sets the text, and the state event that follows keeps it", () => {
    const withText = applyEvent(running(), { type: "error", id: 1, message: "connection reset by peer" });
    expect(withText.error).toBe("connection reset by peer");
    expect(applyEvent(withText, { type: "state", id: 1, status: DaemonStatus.ERROR })).toMatchObject({ state: State.FAILED, error: "connection reset by peer" });
    expect(applyEvent(withText, { type: "error", id: 1, message: "" }).error).toBe("connection reset by peer");
  });

  test("complete fills the bar and clears the transient numbers", () => {
    const after = applyEvent(running(), { type: "complete", id: 1, totalBytes: 1000, elapsedMs: 5000, checksumOk: true });
    expect(after).toMatchObject({ state: State.COMPLETE, totalSize: 1000, downloaded: 1000, speedBps: 0, etaSeconds: null, error: "" });
  });

  test("an unknown download, event or type changes nothing", () => {
    const d = running();
    expect(applyEvent(undefined, { type: "progress", id: 9 })).toBeUndefined();
    expect(applyEvent(d, undefined)).toBe(d);
    expect(applyEvent(d, { type: "something-new", id: 1 })).toBe(d);
    expect(applyEvent(d, { type: "cancelled", id: 1 })).toBe(d); // the caller removes it
  });

  test("does not change what it is given", () => {
    const frozen = Object.freeze({ ...running() });
    expect(() => applyEvent(frozen, { type: "progress", id: 1, totalBytes: 1000, downloadedBytes: 200, speedBps: 1 })).not.toThrow();
    expect(() => applyEvent(frozen, { type: "state", id: 1, status: DaemonStatus.PAUSED })).not.toThrow();
  });
});

describe("mergeSnapshot", () => {
  const live = (over) => model({ status: DaemonStatus.RUNNING, totalSize: 1000, ...over });

  test("keeps the further-along progress of a download that is running in both", () => {
    // the UI saw 700 bytes in a progress event; the database was last written at 400
    const known = { ...live({ downloaded: 250 }), downloaded: 700, speedBps: 55, etaSeconds: 6, activeSegments: 3 };
    const fresh = live({ downloaded: 400, speedBps: 100, segments: 16 });

    expect(mergeSnapshot(known, fresh)).toEqual({
      ...fresh,
      downloaded: 700,
      speedBps: 55,
      etaSeconds: 6,
      activeSegments: 3,
    });
  });

  test("takes the snapshot when it is further along, or equal", () => {
    const known = { ...live({ downloaded: 250 }), downloaded: 300 };
    const fresh = live({ downloaded: 400 });
    expect(mergeSnapshot(known, fresh)).toBe(fresh);
    expect(mergeSnapshot({ ...known, downloaded: 400 }, fresh)).toBe(fresh);
  });

  test("takes the snapshot's word for everything else", () => {
    const fresh = live({ downloaded: 100 });
    expect(mergeSnapshot(undefined, fresh)).toBe(fresh); // not known yet
    // paused, queued, finished or failed in either: the state changed, so it is a new situation
    const furtherAlong = { ...live({ downloaded: 100 }), downloaded: 900 };
    expect(mergeSnapshot({ ...furtherAlong, state: State.PAUSED }, fresh)).toBe(fresh);
    expect(mergeSnapshot(furtherAlong, model({ status: DaemonStatus.PAUSED, downloaded: 100 }))).toMatchObject({ downloaded: 100 });
    expect(mergeSnapshot(furtherAlong, model({ status: DaemonStatus.DONE, totalSize: 1000, downloaded: 1000 }))).toMatchObject({ state: State.COMPLETE });
    expect(mergeSnapshot(furtherAlong, model({ status: DaemonStatus.ERROR, errorMessage: "boom" }))).toMatchObject({ state: State.FAILED, error: "boom" });
  });

  test("does not change what it is given", () => {
    const known = Object.freeze({ ...live({ downloaded: 250 }), downloaded: 700 });
    const fresh = Object.freeze(live({ downloaded: 400 }));
    expect(() => mergeSnapshot(known, fresh)).not.toThrow();
  });
});
