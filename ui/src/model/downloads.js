/* FluxDM — model/downloads.js
 * What the UI knows about one download, and how the daemon's data becomes it.
 *
 * The daemon speaks in numbers (status 0 to 4) and bytes, the UI in named states. Everything that crosses that
 * line lives here, so a component never sees a status code and the daemon's JSON is read in exactly one place:
 *
 *   JSON of GET /api/downloads and of the POST calls   ->  fromDaemon()
 *   push events on /topic/downloads                    ->  applyEvent()
 *
 * Both are pure functions: they never change what they are given.
 */

/* The daemon's status codes (model/Download.java). */
export const DaemonStatus = Object.freeze({ PENDING: 0, RUNNING: 1, PAUSED: 2, DONE: 3, ERROR: 4 });

/* The states the UI filters and badges on. */
export const State = Object.freeze({
  QUEUED: "queued",
  DOWNLOADING: "downloading",
  PAUSED: "paused",
  COMPLETE: "complete",
  FAILED: "failed",
});

const STATE_OF_CODE = {
  [DaemonStatus.PENDING]: State.QUEUED,
  [DaemonStatus.RUNNING]: State.DOWNLOADING,
  [DaemonStatus.PAUSED]: State.PAUSED,
  [DaemonStatus.DONE]: State.COMPLETE,
  [DaemonStatus.ERROR]: State.FAILED,
};

/* A code this UI does not know (a newer daemon may add one, see the roadmap's "waiting for server") is shown as
 * queued: not running, not finished, and nothing to offer but waiting. */
export const stateOf = (code) => STATE_OF_CODE[code] ?? State.QUEUED;

/* Which states each tab of the downloads list shows. A paused download stays under "active": it is one click
 * from running again. */
export const TABS = Object.freeze({
  active: [State.DOWNLOADING, State.PAUSED],
  queued: [State.QUEUED],
  complete: [State.COMPLETE],
  failed: [State.FAILED],
});

/* ── Reading the daemon's JSON ────────────────────────────────────────────────────────────────────────────── */

const count = (v) => {
  const n = Number(v);
  return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0;
};
const text = (v) => (typeof v === "string" ? v : "");

const TYPE_EXTENSIONS = {
  iso: ["iso", "img"],
  archive: ["zip", "tar", "gz", "tgz", "xz", "bz2", "zst", "7z", "rar"],
  video: ["mp4", "mkv", "webm", "avi", "mov", "m4v", "flv", "ts"],
  audio: ["mp3", "m4a", "aac", "flac", "ogg", "opus", "wav"],
  pdf: ["pdf"],
};
const TYPE_OF_EXTENSION = Object.fromEntries(
  Object.entries(TYPE_EXTENSIONS).flatMap(([type, extensions]) => extensions.map((e) => [e, type]))
);

/* The icon a file gets, from its extension. */
export function fileTypeOf(name) {
  const dot = name.lastIndexOf(".");
  return dot < 0 ? "default" : TYPE_OF_EXTENSION[name.slice(dot + 1).toLowerCase()] || "default";
}

/* What to call a download: the file it is saved as, else the last part of its URL, else the URL. */
function nameOf(filePath, url) {
  const fromPath = filePath.split("/").filter(Boolean).pop();
  if (fromPath) return fromPath;
  try {
    const last = new URL(url).pathname.split("/").filter(Boolean).pop();
    if (last) return decodeURIComponent(last);
  } catch {
    /* not a URL, or a broken escape: fall through to the plain text */
  }
  return url;
}

/* Seconds left at the current speed, or null when it cannot be said. */
export function etaFromSpeed(totalSize, downloaded, speedBps) {
  return speedBps > 0 && totalSize > downloaded ? Math.ceil((totalSize - downloaded) / speedBps) : null;
}

/* One Download of the REST API (see api/DownloadController) as the UI model. */
export function fromDaemon(json) {
  const d = json || {};
  const filePath = text(d.filePath);
  const url = text(d.url);
  const state = stateOf(d.status);
  const totalSize = count(d.totalSize);
  const downloaded = count(d.downloaded);
  const live = state === State.DOWNLOADING;
  const speedBps = live ? count(d.speedBps) : 0;
  const name = nameOf(filePath, url);
  return {
    id: Number(d.id),
    url,
    filePath,
    name,
    type: fileTypeOf(name),
    state,
    totalSize,
    downloaded: totalSize > 0 ? Math.min(downloaded, totalSize) : downloaded,
    segments: count(d.segments),
    priority: count(d.priority),
    checksum: text(d.checksum),
    checksumType: count(d.checksumType),
    error: state === State.FAILED ? text(d.errorMessage) : "",
    bandwidthLimit: count(d.bandwidthLimit),
    speedBps,
    etaSeconds: live ? etaFromSpeed(totalSize, downloaded, speedBps) : null,
    activeSegments: 0,
  };
}

/* How far along a download is, 0 to 100. Rounded down, so nothing shows 100 before it is complete. */
export function percentOf(download) {
  if (download.state === State.COMPLETE) return 100;
  if (download.totalSize <= 0) return 0;
  return Math.min(100, Math.floor((download.downloaded * 100) / download.totalSize));
}

/* ── Applying push events ─────────────────────────────────────────────────────────────────────────────────── */

/* The numbers that only mean something while the download is running. */
const IDLE = { speedBps: 0, etaSeconds: null, activeSegments: 0 };

/* Progress carries numbers only. Its own `status` field is the core's (0 idle, 1 connecting, 2 downloading,
 * 3 paused, 4 complete, 5 error, 6 cancelled), a different list from the daemon's, so it is not read: the state
 * changes only through `state` events, which the daemon sends before the first progress of a download. */
function applyProgress(d, e) {
  if (d.state === State.COMPLETE) return d; /* nothing is left to report, and a late frame must not move it back */
  const totalSize = count(e.totalBytes) || d.totalSize;
  const downloaded = count(e.downloadedBytes);
  const running = d.state === State.DOWNLOADING;
  return {
    ...d,
    totalSize,
    downloaded: totalSize > 0 ? Math.min(downloaded, totalSize) : downloaded,
    speedBps: running ? count(e.speedBps) : 0,
    etaSeconds: running && e.etaSeconds >= 0 ? e.etaSeconds : null,
    activeSegments: running ? count(e.activeSegments) : 0,
  };
}

function applyState(d, e) {
  const state = stateOf(e.status);
  return {
    ...d,
    state,
    error: state === State.FAILED ? text(e.message) || d.error : "",
    ...(state === State.DOWNLOADING ? {} : IDLE),
  };
}

function applyComplete(d, e) {
  const totalSize = count(e.totalBytes) || d.totalSize;
  return { ...d, ...IDLE, state: State.COMPLETE, totalSize, downloaded: totalSize, error: "" };
}

/* The download after one push event. The daemon sends progress, status, state, complete, error, cancelled and
 * core (service/DownloadService). Returns the same object when the event changes nothing for it, and undefined
 * for a download the UI does not know: an event cannot make one, because it carries no URL or path, so the
 * caller refreshes the list instead. `cancelled` (the download is gone) and `core` (about the daemon, not a
 * download) are not changes to a download and are handled by the caller. */
export function applyEvent(download, event) {
  if (!download || !event) return download;
  switch (event.type) {
    case "progress":
    case "status":
      return applyProgress(download, event);
    case "state":
      return applyState(download, event);
    case "complete":
      return applyComplete(download, event);
    case "error":
      return { ...download, error: text(event.message) || download.error };
    default:
      return download;
  }
}
