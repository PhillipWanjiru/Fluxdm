/* FluxDM — store/liveSync.js
 * Keeps the store in step with the daemon: a snapshot of the list (GET /api/downloads) with the push feed's events
 * (api/live.js) on top of it.
 *
 * The two are joined without a gap and without going back in time:
 *   1. the feed connects and subscribes, and says it is ready;
 *   2. the list is fetched, and the events that arrive meanwhile wait in a buffer;
 *   3. the snapshot goes into the store, then the buffered events are applied in order.
 * Every event is absolute (a state, a byte count, never "plus n"), so one that the snapshot already includes does
 * no harm, and every event after the subscription is applied once.
 *
 * A snapshot is taken again after every reconnect, when the daemon says its core is back (it has just reconciled
 * the downloads with it), and once a minute while connected, in case an event was lost on the way. An event for a
 * download the UI does not have, because another client added it, cannot be applied (it carries no URL or path):
 * it asks for a snapshot too, soon but never more often than every few seconds.
 *
 * What is wrong is told through the connection slice (store/connectionSlice.js), never thrown. */
import { getAll } from "../api/downloads";
import { openLiveFeed } from "../api/live";
import { getHealth } from "../api/stats";
import { Link } from "../model/connection";
import { coreChanged, linkChanged, listFailed, listLoaded } from "./connectionSlice";
import { eventReceived, received, selectDownloadById } from "./downloadsSlice";

export const TIMING = Object.freeze({
  unknownDebounceMs: 200 /* after an event for a download the UI does not have, wait this long for more of them */,
  unknownMinGapMs: 3000 /* and never take two snapshots for such events closer together than this */,
  safetyNetMs: 60000 /* while connected, take a snapshot this often anyway */,
  retryMs: Object.freeze([1000, 2000, 5000, 10000, 30000]) /* after a failed snapshot, wait this long; the last repeats */,
  maxBuffered: 5000 /* events kept while a snapshot is fetched; past this the snapshot is taken again */,
});

/* Why the core is not available, from GET /api/health. */
const whyNot = (health) => (health.coreConnected ? "not in step with the daemon yet" : "not connected");

/* Starts keeping `store` in step with the daemon. Returns the function that stops it.
 * options (for tests): open, fetchList, fetchHealth, now, and timing (overrides of TIMING). */
export function startLiveSync(store, options = {}) {
  const { open = openLiveFeed, fetchList = getAll, fetchHealth = getHealth, now = Date.now } = options;
  const t = { ...TIMING, ...options.timing };
  const { dispatch, getState } = store;

  let stopped = false;
  let syncing = false; /* a snapshot is on its way, and events wait in `buffer` */
  let again = false; /* another snapshot was asked for while it was */
  let buffer = [];
  let overflowed = false;
  let failures = 0; /* snapshots that failed in a row */
  let lastUnknownRefresh = -Infinity;
  let coreVersion = 0; /* counts core events, so a health answer that is older than one is dropped */
  let refreshTimer = null;
  let retryTimer = null;
  let safetyTimer = null;

  /* ── Events ─────────────────────────────────────────────────────────────────────────────────────────────── */

  function deliver(event) {
    const unknown = event.id && event.type !== "cancelled" && !selectDownloadById(getState(), event.id);
    if (unknown) refreshSoon();
    else dispatch(eventReceived(event));
  }

  function onEvent(event) {
    if (stopped) return;
    if (!syncing) deliver(event);
    else if (buffer.length < t.maxBuffered) buffer.push(event);
    else overflowed = true;
  }

  function onCore(event) {
    if (stopped || event.type !== "core") return;
    coreVersion += 1;
    const up = event.state === "up";
    dispatch(coreChanged({ up, reason: up ? "" : String(event.reason ?? "") }));
    if (up) resync();
  }

  /* ── Snapshots ──────────────────────────────────────────────────────────────────────────────────────────── */

  function resync() {
    if (stopped) return;
    if (syncing) {
      again = true;
      return;
    }
    clearTimeout(refreshTimer);
    clearTimeout(retryTimer);
    refreshTimer = retryTimer = null;
    syncing = true;
    buffer = [];
    overflowed = false;
    /* started at once; a call that throws instead of rejecting is a failed snapshot all the same */
    new Promise((resolve) => resolve(fetchList())).then(
      (list) => {
        if (stopped) return;
        failures = 0;
        dispatch(received(list));
        dispatch(listLoaded());
        settle();
      },
      (error) => {
        if (stopped) return;
        failures += 1;
        dispatch(listFailed((error && error.message) || "unknown error"));
        settle();
        retrySoon();
      }
    );
  }

  /* The snapshot is in (or failed): the events that waited are applied, in the order they came. */
  function settle() {
    const waiting = buffer;
    const lost = overflowed;
    buffer = [];
    overflowed = false;
    syncing = false;
    for (const event of waiting) deliver(event);
    if (again || lost) {
      again = false;
      resync();
    }
  }

  function refreshSoon() {
    if (stopped || syncing || refreshTimer) return;
    const wait = Math.max(t.unknownDebounceMs, lastUnknownRefresh + t.unknownMinGapMs - now());
    refreshTimer = setTimeout(() => {
      refreshTimer = null;
      lastUnknownRefresh = now();
      resync();
    }, wait);
  }

  function retrySoon() {
    if (stopped || syncing || retryTimer) return;
    const wait = t.retryMs[Math.min(failures, t.retryMs.length) - 1];
    retryTimer = setTimeout(() => {
      retryTimer = null;
      resync();
    }, wait);
  }

  /* What GET /api/health says about the core, unless a core event has said something newer meanwhile. */
  function checkHealth() {
    const version = coreVersion;
    new Promise((resolve) => resolve(fetchHealth())).then(
      (health) => {
        if (stopped || version !== coreVersion) return;
        dispatch(coreChanged({ up: health.ok, reason: health.ok ? "" : whyNot(health) }));
      },
      () => {} /* the core stays "unknown"; the first core event settles it */
    );
  }

  /* ── The feed ───────────────────────────────────────────────────────────────────────────────────────────── */

  function onLink(link) {
    if (stopped) return;
    dispatch(linkChanged(link));
    if (link === Link.LIVE) {
      if (!safetyTimer) safetyTimer = setInterval(resync, t.safetyNetMs);
      return;
    }
    /* No connection: nothing to refresh or retry until there is one again (onReady takes a snapshot). */
    clearInterval(safetyTimer);
    clearTimeout(refreshTimer);
    clearTimeout(retryTimer);
    safetyTimer = refreshTimer = retryTimer = null;
  }

  function onReady() {
    if (stopped) return;
    coreVersion += 1;
    checkHealth();
    resync();
  }

  const feed = open({ onLink, onReady, onEvent, onCore });

  return function stop() {
    stopped = true;
    clearInterval(safetyTimer);
    clearTimeout(refreshTimer);
    clearTimeout(retryTimer);
    Promise.resolve(feed.close()).catch(() => {});
  };
}
