/* FluxDM — model/bandwidth.js
 * The limit on all downloads together. The daemon keeps it in bytes per second (GET and PUT /api/bandwidth,
 * globalLimitBps; 0 is no limit); the settings field is in KB/s, the unit the rest of the UI prints (model/format.js:
 * 1 KB is 1024 bytes). */

export const BYTES_PER_KB = 1024;

/* Far beyond any link there is; the bound only keeps what is typed to a sensible number. */
export const MAX_LIMIT_KB = 10000000;

/* The text of the field, in KB/s, as the daemon's bytes per second: { bps }, or { error } to show beside the field. */
export function parseLimit(text) {
  const typed = String(text ?? "").trim();
  if (typed === "") return { error: "Enter a number of KB/s, or 0 for no limit." };
  if (!/^\d+$/.test(typed)) return { error: "Use a whole number of KB/s." };
  const kb = Number(typed);
  if (kb > MAX_LIMIT_KB) return { error: `The limit cannot be more than ${MAX_LIMIT_KB} KB/s.` };
  return { bps: kb * BYTES_PER_KB };
}

/* What the daemon holds, as the field shows it. The daemon may hold a number that is not a whole count of KB (another
 * client set it), which is shown rounded. */
export const limitToKb = (bps) => Math.round(bps / BYTES_PER_KB);
