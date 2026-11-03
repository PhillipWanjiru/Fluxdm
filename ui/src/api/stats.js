/* FluxDM — api/stats.js
 * Totals, the daemon-wide speed limit and the daemon's own health. */
import { client } from "./client";
import { ApiError } from "./errors";

const whole = (v) => {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
};

/* GET /api/stats: how many downloads there are in each group, and the speed of all of them together. */
export async function getStats() {
  const { data } = await client.get("/stats");
  return {
    total: whole(data.total),
    active: whole(data.active),
    queued: whole(data.queued),
    complete: whole(data.complete),
    speedBps: whole(data.speedBps),
  };
}

/* GET and PUT /api/bandwidth: the limit on all downloads together, in bytes per second, 0 for none. */
export async function getBandwidthLimit() {
  const { data } = await client.get("/bandwidth");
  return whole(data.globalLimitBps);
}

export async function setBandwidthLimit(bytesPerSec) {
  const { data } = await client.put("/bandwidth", { globalLimitBps: bytesPerSec });
  return whole(data.globalLimitBps);
}

/* GET /api/health: `ok` is false while the daemon is up but its core is not connected or not in step. The core's
 * own state is an object there (IPCBridgeService.status()): connected or not, who started it, how often it was
 * restarted. */
export async function getHealth() {
  const { data } = await client.get("/health");
  if (!data || typeof data.status !== "string") {
    throw new ApiError("The daemon sent no health status.", { kind: "protocol" });
  }
  const core = data.core && typeof data.core === "object" ? data.core : {};
  return {
    ok: data.status === "ok",
    synced: data.synced === true,
    coreConnected: core.connected === true,
    coreRestarts: whole(core.restarts),
  };
}
