import { ApiError } from "../api/errors";
import { cancel, pause, resume } from "../api/downloads";
import { fromDaemon } from "../model/downloads";
import { daemonDownload } from "../testing/fixtures";
import { pauseDownload, removeDownload, resumeDownload } from "./downloadActions";
import { received, selectAllDownloads, selectDownloadById } from "./downloadsSlice";
import { makeStore } from "./index";

jest.mock("../api/downloads");

const dl = (over) => fromDaemon(daemonDownload(over));

function setup() {
  const store = makeStore();
  store.dispatch(received([dl({ id: 1, status: 1 }), dl({ id: 2, status: 2, speedBps: 0 })]));
  return store;
}
const ids = (store) => selectAllDownloads(store.getState()).map((d) => d.id);
const refusal = (kind, status, message) => new ApiError(message, { kind, status });

beforeEach(() => {
  pause.mockReset();
  resume.mockReset();
  cancel.mockReset();
});

describe("pauseDownload and resumeDownload", () => {
  test("show what the daemon answers, and only once it has", async () => {
    const store = setup();
    pause.mockResolvedValue(dl({ id: 1, status: 2, speedBps: 0 }));
    resume.mockResolvedValue(dl({ id: 2, status: 1 }));

    const pending = store.dispatch(pauseDownload(1));
    expect(selectDownloadById(store.getState(), 1).state).toBe("downloading"); // not before the daemon agrees
    await pending;
    expect(pause).toHaveBeenCalledWith(1);
    expect(selectDownloadById(store.getState(), 1).state).toBe("paused");

    await store.dispatch(resumeDownload(2));
    expect(resume).toHaveBeenCalledWith(2);
    expect(selectDownloadById(store.getState(), 2).state).toBe("downloading");
  });

  test("throw the daemon's refusal and leave the download as it was", async () => {
    const store = setup();
    const before = store.getState().downloads;
    pause.mockRejectedValue(refusal("conflict", 409, "the download has finished"));
    resume.mockRejectedValue(refusal("unavailable", 503, "The daemon is not ready (is fluxdm-core running?)."));

    await expect(store.dispatch(pauseDownload(1))).rejects.toMatchObject({ kind: "conflict", message: "the download has finished" });
    await expect(store.dispatch(resumeDownload(2))).rejects.toMatchObject({ kind: "unavailable" });
    expect(store.getState().downloads).toBe(before);
  });

  test("take a download the daemon does not know any more out of the list, without an error", async () => {
    const store = setup();
    pause.mockRejectedValue(refusal("not_found", 404, "no such download"));
    resume.mockRejectedValue(refusal("not_found", 404, "no such download"));

    await expect(store.dispatch(pauseDownload(1))).resolves.toBeUndefined();
    await expect(store.dispatch(resumeDownload(2))).resolves.toBeUndefined();
    expect(ids(store)).toEqual([]);
  });
});

describe("removeDownload", () => {
  test("cancels at the daemon, then removes the download from the list", async () => {
    const store = setup();
    cancel.mockResolvedValue(undefined);

    const pending = store.dispatch(removeDownload(1));
    expect(ids(store)).toEqual([2, 1]); // still there until the daemon has done it
    await pending;

    expect(cancel).toHaveBeenCalledWith(1);
    expect(ids(store)).toEqual([2]);
  });

  test("keeps the download when the daemon refuses, and drops it when the daemon never had it", async () => {
    const store = setup();
    cancel.mockRejectedValueOnce(refusal("unreachable", null, "Cannot reach the FluxDM daemon."));
    await expect(store.dispatch(removeDownload(1))).rejects.toMatchObject({ kind: "unreachable" });
    expect(ids(store)).toEqual([2, 1]);

    cancel.mockRejectedValueOnce(refusal("not_found", 404, "no such download"));
    await expect(store.dispatch(removeDownload(1))).resolves.toBeUndefined();
    expect(ids(store)).toEqual([2]);
  });
});
