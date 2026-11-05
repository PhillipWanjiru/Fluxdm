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

/* What to tell the user, as { kind, label, message }, or null when there is nothing to say. `kind` is "warning" for
 * something that mends itself and "error" for something that needs the user; `label` is a few words for the
 * sidebar, `message` the sentence for the banner. Only one thing is said at a time: the first that applies, since
 * each one hides the ones below it (no link, no core news; no core, no list). */
export function describeConnection({ link, core, coreReason, syncError }) {
  if (link === Link.UNAUTHORIZED) {
    return {
      kind: "error",
      label: "Token refused",
      message: "The daemon refused this app's API token. The token is read from ~/.fluxdm/api-token; trying again.",
    };
  }
  if (link !== Link.LIVE) {
    return {
      kind: "warning",
      label: link === Link.CONNECTING ? "Connecting…" : "Offline",
      message: "Not connected to the FluxDM daemon. Trying again.",
    };
  }
  if (core === Core.DOWN) {
    const why = coreReason ? ` (${coreReason})` : "";
    return {
      kind: "error",
      label: "Core down",
      message: `The download core is not available${why}. Starting or resuming a download will fail until it is back.`,
    };
  }
  if (syncError) {
    const why = syncError.replace(/[.\s]+$/, "");
    return { kind: "warning", label: "Not in sync", message: `Could not load the list of downloads (${why}). Trying again.` };
  }
  return null;
}

/* The same, for a place that always shows something (the sidebar): all is well is "Connected", kind "ok". */
export function summarizeConnection(state) {
  return describeConnection(state) || { kind: "ok", label: "Connected", message: "Connected to the FluxDM daemon." };
}
