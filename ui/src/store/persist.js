/* FluxDM — store/persist.js
 * Keeps the settings the UI owns between runs, in localStorage (in the Electron app, under the user's data
 * directory). Anything that can go wrong with storage (disabled, full, a private window, damaged text) means
 * "use the defaults", never an error: the UI must start.
 *
 * Only what the UI owns is stored. The daemon-wide speed limit belongs to the daemon and is read from it. */
import { MAX_SEGMENTS, hasControlChars, isAbsoluteDirectory } from "../model/destination";

const KEY = "fluxdm.settings.v1";

/* Whether a stored value may be used; a rejected one falls back to its default. */
const VALID = {
  downloadDir: (v) => typeof v === "string" && isAbsoluteDirectory(v) && !hasControlChars(v),
  defaultSegments: (v) => Number.isInteger(v) && v >= 1 && v <= MAX_SEGMENTS,
  theme: (v) => v === "dark",
};

function pick(settings) {
  const out = {};
  for (const key of Object.keys(VALID)) if (VALID[key](settings[key])) out[key] = settings[key];
  return out;
}

export function loadSettings() {
  try {
    const parsed = JSON.parse(window.localStorage.getItem(KEY) || "{}");
    return parsed && typeof parsed === "object" ? pick(parsed) : {};
  } catch {
    return {};
  }
}

/* Writes the settings whenever the stored part of them changes, and only then: the store changes several
 * times a second while downloads run. Returns what stops the watching. */
export function persistSettings(store) {
  let last = JSON.stringify(pick(store.getState().settings));
  return store.subscribe(() => {
    const now = JSON.stringify(pick(store.getState().settings));
    if (now === last) return;
    last = now;
    try {
      window.localStorage.setItem(KEY, now);
    } catch {
      /* storage is unavailable: the settings last for this run only */
    }
  });
}
