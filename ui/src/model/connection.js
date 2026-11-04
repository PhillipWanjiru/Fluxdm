/* FluxDM — model/connection.js
 * How the UI stands with the daemon, and what to tell the user about it.
 *
 *   Link   the push feed (api/live.js): CONNECTING while an attempt is under way, LIVE when events flow, OFFLINE
 *          between attempts, UNAUTHORIZED when the daemon refused the token
 *   Core   the download core behind the daemon, as far as the daemon has said (events on /topic/core, GET /api/health)
 */

export const Link = Object.freeze({
  CONNECTING: "connecting",
  LIVE: "live",
  OFFLINE: "offline",
  UNAUTHORIZED: "unauthorized",
});

export const Core = Object.freeze({ UNKNOWN: "unknown", UP: "up", DOWN: "down" });

/* What to tell the user, as { kind, message }, or null when there is nothing to say. `kind` is "warning" for
 * something that mends itself and "error" for something that needs the user. Only one thing is said at a time:
 * the first that applies, since each one hides the ones below it (no link, no core news; no core, no list). */
export function describeConnection({ link, core, coreReason, syncError }) {
  if (link === Link.UNAUTHORIZED) {
    return {
      kind: "error",
      message: "The daemon refused this app's API token. The token is read from ~/.fluxdm/api-token; trying again.",
    };
  }
  if (link !== Link.LIVE) {
    return { kind: "warning", message: "Not connected to the FluxDM daemon. Trying again." };
  }
  if (core === Core.DOWN) {
    const why = coreReason ? ` (${coreReason})` : "";
    return {
      kind: "error",
      message: `The download core is not available${why}. Starting or resuming a download will fail until it is back.`,
    };
  }
  if (syncError) {
    const why = syncError.replace(/[.\s]+$/, "");
    return { kind: "warning", message: `Could not load the list of downloads (${why}). Trying again.` };
  }
  return null;
}
