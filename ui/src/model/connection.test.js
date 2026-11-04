import { Core, Link, describeConnection } from "./connection";

const state = (over) => ({ link: Link.LIVE, core: Core.UP, coreReason: "", syncError: "", ...over });

describe("describeConnection", () => {
  test("says nothing while all is well, or while the core is not known yet", () => {
    expect(describeConnection(state())).toBeNull();
    expect(describeConnection(state({ core: Core.UNKNOWN }))).toBeNull();
  });

  test("says the daemon is out of reach, for as long as there is no link", () => {
    for (const link of [Link.CONNECTING, Link.OFFLINE]) {
      expect(describeConnection(state({ link }))).toEqual({ kind: "warning", message: "Not connected to the FluxDM daemon. Trying again." });
    }
  });

  test("says a refused token needs the user, and where the token comes from", () => {
    const notice = describeConnection(state({ link: Link.UNAUTHORIZED }));
    expect(notice.kind).toBe("error");
    expect(notice.message).toContain("~/.fluxdm/api-token");
  });

  test("says the core is not available, with the reason when there is one", () => {
    expect(describeConnection(state({ core: Core.DOWN, coreReason: "ipc closed" }))).toEqual({
      kind: "error",
      message: "The download core is not available (ipc closed). Starting or resuming a download will fail until it is back.",
    });
    expect(describeConnection(state({ core: Core.DOWN })).message).toMatch(/^The download core is not available\. /);
  });

  test("says why the list could not be loaded, without doubling the full stop", () => {
    expect(describeConnection(state({ syncError: "The daemon is not ready." }))).toEqual({
      kind: "warning",
      message: "Could not load the list of downloads (The daemon is not ready). Trying again.",
    });
    expect(describeConnection(state({ syncError: "timeout" })).message).toContain("(timeout)");
  });

  test("says one thing at a time: the link before the core, the core before the list", () => {
    expect(describeConnection(state({ link: Link.OFFLINE, core: Core.DOWN, syncError: "x" })).message).toMatch(/^Not connected/);
    expect(describeConnection(state({ core: Core.DOWN, syncError: "x" })).message).toMatch(/^The download core/);
  });
});
