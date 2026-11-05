import { Core, Link, describeConnection, summarizeConnection } from "./connection";

const state = (over) => ({ link: Link.LIVE, core: Core.UP, coreReason: "", syncError: "", ...over });

describe("describeConnection", () => {
  test("says nothing while all is well, or while the core is not known yet", () => {
    expect(describeConnection(state())).toBeNull();
    expect(describeConnection(state({ core: Core.UNKNOWN }))).toBeNull();
  });

  test("says the daemon is out of reach, for as long as there is no link", () => {
    for (const link of [Link.CONNECTING, Link.OFFLINE]) {
      expect(describeConnection(state({ link }))).toMatchObject({ kind: "warning", message: "Not connected to the FluxDM daemon. Trying again." });
    }
  });

  test("tells an attempt under way from a wait between attempts, in the short label", () => {
    expect(describeConnection(state({ link: Link.CONNECTING })).label).toBe("Connecting…");
    expect(describeConnection(state({ link: Link.OFFLINE })).label).toBe("Offline");
  });

  test("says a refused token needs the user, and where the token comes from", () => {
    const notice = describeConnection(state({ link: Link.UNAUTHORIZED }));
    expect(notice.kind).toBe("error");
    expect(notice.label).toBe("Token refused");
    expect(notice.message).toContain("~/.fluxdm/api-token");
  });

  test("says the core is not available, with the reason when there is one", () => {
    expect(describeConnection(state({ core: Core.DOWN, coreReason: "ipc closed" }))).toEqual({
      kind: "error",
      label: "Core down",
      message: "The download core is not available (ipc closed). Starting or resuming a download will fail until it is back.",
    });
    expect(describeConnection(state({ core: Core.DOWN })).message).toMatch(/^The download core is not available\. /);
  });

  test("says why the list could not be loaded, without doubling the full stop", () => {
    expect(describeConnection(state({ syncError: "The daemon is not ready." }))).toEqual({
      kind: "warning",
      label: "Not in sync",
      message: "Could not load the list of downloads (The daemon is not ready). Trying again.",
    });
    expect(describeConnection(state({ syncError: "timeout" })).message).toContain("(timeout)");
  });

  test("says one thing at a time: the link before the core, the core before the list", () => {
    expect(describeConnection(state({ link: Link.OFFLINE, core: Core.DOWN, syncError: "x" })).message).toMatch(/^Not connected/);
    expect(describeConnection(state({ core: Core.DOWN, syncError: "x" })).message).toMatch(/^The download core/);
  });
});

describe("summarizeConnection", () => {
  test("is what describeConnection says, when it says something", () => {
    for (const over of [{ link: Link.OFFLINE }, { link: Link.UNAUTHORIZED }, { core: Core.DOWN }, { syncError: "x" }]) {
      expect(summarizeConnection(state(over))).toEqual(describeConnection(state(over)));
    }
  });

  test("is connected, kind ok, when there is nothing to say", () => {
    expect(summarizeConnection(state())).toEqual({ kind: "ok", label: "Connected", message: "Connected to the FluxDM daemon." });
    expect(summarizeConnection(state({ core: Core.UNKNOWN })).kind).toBe("ok");
  });
});
