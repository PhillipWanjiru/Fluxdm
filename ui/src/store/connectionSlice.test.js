import { Core, Link } from "../model/connection";
import reducer, { coreChanged, initialConnection, linkChanged, listFailed, listLoaded, selectConnection } from "./connectionSlice";

const run = (...actions) => actions.reduce(reducer, reducer(undefined, { type: "@@init" }));

describe("connection slice", () => {
  test("starts out connecting, with nothing known about the core and no list error", () => {
    expect(run()).toEqual(initialConnection);
    expect(initialConnection).toEqual({ link: Link.CONNECTING, core: Core.UNKNOWN, coreReason: "", syncError: "" });
  });

  test("keeps the core's state and the reason it is down", () => {
    expect(run(linkChanged(Link.LIVE), coreChanged({ up: false, reason: "ipc closed" }))).toMatchObject({ core: Core.DOWN, coreReason: "ipc closed" });
    expect(run(linkChanged(Link.LIVE), coreChanged({ up: false }))).toMatchObject({ core: Core.DOWN, coreReason: "" });
    expect(run(linkChanged(Link.LIVE), coreChanged({ up: false, reason: "x" }), coreChanged({ up: true, reason: "connected" }))).toMatchObject({
      core: Core.UP,
      coreReason: "", // a reason belongs to "down"
    });
  });

  test("keeps the reason a list could not be loaded until one is", () => {
    const failed = run(linkChanged(Link.LIVE), listFailed("The daemon is not ready."));
    expect(failed.syncError).toBe("The daemon is not ready.");
    expect(reducer(failed, listLoaded()).syncError).toBe("");
  });

  test("forgets what it knew of the core and of the list when the link goes", () => {
    const before = run(linkChanged(Link.LIVE), coreChanged({ up: false, reason: "x" }), listFailed("y"));
    for (const link of [Link.OFFLINE, Link.CONNECTING, Link.UNAUTHORIZED]) {
      expect(reducer(before, linkChanged(link))).toEqual({ link, core: Core.UNKNOWN, coreReason: "", syncError: "" });
    }
    expect(reducer(before, linkChanged(Link.LIVE))).toMatchObject({ core: Core.DOWN, syncError: "y" }); // still live: unchanged
  });

  test("selectConnection reads it from the store", () => {
    const connection = run(linkChanged(Link.LIVE));
    expect(selectConnection({ connection })).toBe(connection);
  });
});
