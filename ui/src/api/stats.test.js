import { client } from "./client";
import { getBandwidthLimit, getHealth, getStats, setBandwidthLimit } from "./stats";
import { installFakeDaemon } from "../testing/fakeDaemon";

describe("api/stats", () => {
  test("getStats reads the totals and the combined speed", async () => {
    installFakeDaemon(client, { "GET /stats": () => ({ data: { total: 5, active: 2, queued: 1, complete: 2, speedBps: 1500000 } }) });
    expect(await getStats()).toEqual({ total: 5, active: 2, queued: 1, complete: 2, speedBps: 1500000 });
  });

  test("a missing or negative number reads as 0", async () => {
    installFakeDaemon(client, { "GET /stats": () => ({ data: { total: 5, speedBps: -3 } }) });
    expect(await getStats()).toEqual({ total: 5, active: 0, queued: 0, complete: 0, speedBps: 0 });
  });

  test("the global limit is read and set in bytes per second", async () => {
    const daemon = installFakeDaemon(client, {
      "GET /bandwidth": () => ({ data: { globalLimitBps: 0 } }),
      "PUT /bandwidth": ({ body }) => ({ data: { globalLimitBps: body.globalLimitBps } }),
    });
    expect(await getBandwidthLimit()).toBe(0);
    expect(await setBandwidthLimit(2_000_000)).toBe(2_000_000);
    expect(daemon.calls[1].body).toEqual({ globalLimitBps: 2_000_000 });
  });

  test("health says whether the core is connected and in step", async () => {
    const core = (over) => ({ connected: true, socket: "/run/user/1000/fluxdm.sock", spawnedByDaemon: true, pid: 4242, restarts: 0, ...over });
    installFakeDaemon(client, { "GET /health": () => ({ data: { status: "degraded", synced: false, core: core({ connected: false, pid: null, restarts: 3 }) } }) });
    expect(await getHealth()).toEqual({ ok: false, synced: false, coreConnected: false, coreRestarts: 3 });
    installFakeDaemon(client, { "GET /health": () => ({ data: { status: "ok", synced: true, core: core() } }) });
    expect(await getHealth()).toEqual({ ok: true, synced: true, coreConnected: true, coreRestarts: 0 });
  });

  test("a health answer without the core's details says it is not connected", async () => {
    installFakeDaemon(client, { "GET /health": () => ({ data: { status: "degraded", synced: false } }) });
    expect(await getHealth()).toEqual({ ok: false, synced: false, coreConnected: false, coreRestarts: 0 });
    installFakeDaemon(client, { "GET /health": () => ({ data: { status: "degraded", synced: false, core: "disconnected" } }) });
    expect(await getHealth()).toEqual({ ok: false, synced: false, coreConnected: false, coreRestarts: 0 });
  });

  test("health without a status is refused", async () => {
    installFakeDaemon(client, { "GET /health": () => ({ data: {} }) });
    await expect(getHealth()).rejects.toMatchObject({ kind: "protocol" });
  });
});
