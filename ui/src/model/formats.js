/* FluxDM — model/formats.js
 * The formats the video grabber can fetch for a page, as choices the user can tell apart.
 *
 * The Python service passes on yt-dlp's list as { id, ext, height, abr } (grabber/fluxdm_grabber/metadata.py): the
 * format's id, its container, its height in pixels (video) and its audio bit rate in kbit/s (audio). That is all
 * there is to go on, so a label says only what those say: it does not claim a format has or lacks sound, because
 * a missing `abr` means "not reported" as often as "no audio". */

/* yt-dlp's own name for "the best single file that has both video and audio"; what the grabber fetches unless
 * told otherwise (GrabberController, formatId). */
export const BEST = "best";

/* The preview images yt-dlp lists beside the real formats: pages of thumbnails, not something to save. */
const isStoryboard = (format) => format.ext === "mhtml";

/* "1080p · MP4", "audio only · M4A · 128 kbps", "360p · MP4 · 96 kbps". */
export function describeFormat(format) {
  const parts = [];
  if (format.height) parts.push(`${format.height}p`);
  else if (format.abr) parts.push("audio only");
  if (format.ext) parts.push(String(format.ext).toUpperCase());
  if (format.abr) parts.push(`${Math.round(format.abr)} kbps`);
  return parts.length > 0 ? parts.join(" · ") : "unknown format";
}

/* What to offer for a page: the best single file first, then the page's own formats, the tallest first, with
 * the audio-only ones last. Each choice is { id, label, detail }. Entries without an id, previews, and a
 * format listed twice are left out. */
export function formatChoices(formats) {
  const seen = new Set([BEST]);
  const own = [];
  for (const format of Array.isArray(formats) ? formats : []) {
    if (!format || format.id === undefined || format.id === null || format.id === "") continue;
    const id = String(format.id);
    if (seen.has(id) || isStoryboard(format)) continue;
    seen.add(id);
    own.push(format);
  }
  own.sort((a, b) => (b.height || 0) - (a.height || 0) || (b.abr || 0) - (a.abr || 0));
  return [
    { id: BEST, label: "Best single file", detail: "video and audio together" },
    ...own.map((format) => ({ id: String(format.id), label: describeFormat(format), detail: `id ${format.id}` })),
  ];
}
