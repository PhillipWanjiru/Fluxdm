/* FluxDM — model/format.js
 * Sizes, speeds and times as the list shows them. */

const UNITS = ["B", "KB", "MB", "GB", "TB"];

const digits = (v) => (v >= 100 ? 0 : 1);

/* 1536 -> "1.5 KB". Units are powers of 1024, like the tools it sits beside. Not a number, or negative: "—". */
export function formatBytes(bytes) {
  const n = Number(bytes);
  if (!Number.isFinite(n) || n < 0) return "—";
  if (n < 1024) return `${Math.floor(n)} B`;
  let value = n;
  let unit = 0;
  /* 1023.96 KB rounds to "1024 KB": step up before showing that */
  while (unit < UNITS.length - 1 && Number(value.toFixed(digits(value))) >= 1024) {
    value /= 1024;
    unit += 1;
  }
  return `${value.toFixed(digits(value))} ${UNITS[unit]}`;
}

export const formatSpeed = (bytesPerSecond) => `${formatBytes(bytesPerSecond)}/s`;

/* Seconds as "45s", "3m 05s", "2h 07m", "1d 04h". Unknown (null, negative, not a number): "—". */
export function formatEta(seconds) {
  if (seconds === null || seconds === undefined || !Number.isFinite(Number(seconds)) || seconds < 0) return "—";
  const s = Math.round(seconds);
  const pad = (n) => String(n).padStart(2, "0");
  if (s < 60) return `${s}s`;
  if (s < 3600) return `${Math.floor(s / 60)}m ${pad(s % 60)}s`;
  if (s < 86400) return `${Math.floor(s / 3600)}h ${pad(Math.floor((s % 3600) / 60))}m`;
  return `${Math.floor(s / 86400)}d ${pad(Math.floor((s % 86400) / 3600))}h`;
}

/* The length of a video, as a player shows it: 185 -> "3:05", 7620 -> "2:07:00". Unknown: "—". */
export function formatDuration(seconds) {
  if (seconds === null || seconds === undefined || !Number.isFinite(Number(seconds)) || seconds < 0) return "—";
  const s = Math.round(seconds);
  const pad = (n) => String(n).padStart(2, "0");
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  return h > 0 ? `${h}:${pad(m)}:${pad(s % 60)}` : `${m}:${pad(s % 60)}`;
}
