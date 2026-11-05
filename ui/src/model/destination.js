/* FluxDM — model/destination.js
 * Where a new download goes, and the checks the daemon will make on it (service/DownloadService.validated),
 * made first so a mistake is shown beside the field instead of coming back as a 400. The daemon stays the
 * authority: these rules only have to be no stricter than its own. */

export const MAX_URL_BYTES = 4095; /* what the core accepts */
export const MAX_PATH_BYTES = 1023;
export const MAX_NAME_BYTES = 255; /* a file name on Linux */
export const MAX_SEGMENTS = 64; /* the daemon clamps to this */
export const SEGMENT_CHOICES = Object.freeze([4, 8, 16, 32]);
export const DEFAULT_DIRECTORY = "~/Downloads"; /* the daemon expands a leading ~/ itself */

/* The daemon's own pattern (DownloadService.URL_SHAPE). Java's \s is ASCII white space only, while JavaScript's
 * also matches U+00A0 and its relatives, so the class is spelled out here: a stricter test than the daemon's
 * would refuse an address the daemon accepts. (\v is U+000B.) */
const URL_SHAPE = /^https?:\/\/[^ \t\n\v\f\r/?#]+(?:[/?#][^ \t\n\v\f\r]*)?$/i;

/* The daemon's own test (DownloadService.hasControlChars), written as a loop rather than a pattern because
 * the lint rules forbid control characters in a regular expression. */
const isControl = (code) => code < 0x20 || code === 0x7f;
export function hasControlChars(text) {
  for (let i = 0; i < text.length; i++) if (isControl(text.charCodeAt(i))) return true;
  return false;
}

/* The length of a string in UTF-8 bytes (TextEncoder is missing from the test environment's jsdom). */
export function utf8Length(text) {
  let bytes = 0;
  for (const ch of text) {
    const c = ch.codePointAt(0);
    bytes += c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
  }
  return bytes;
}

/* What java.nio.file.Path.normalize() makes of an absolute path: "//", "." and "x/.." disappear. The daemon
 * measures the normalized path against its limit, so this check has to as well, or it could refuse a path
 * that the daemon would take. A leading "~" is left as it is: expanding it can only make the path longer. */
export function normalizePath(path) {
  const parts = [];
  for (const part of path.split("/")) {
    if (part === "" || part === ".") continue;
    if (part === "..") parts.pop();
    else parts.push(part);
  }
  return `/${parts.join("/")}`;
}

/* Whether a folder starts the way the daemon needs: "~", "~/…" or "/…" (it expands the first itself). */
export function isAbsoluteDirectory(directory) {
  const dir = directory.trim();
  return dir === "~" || dir.startsWith("~/") || dir.startsWith("/");
}

/* A file name that is safe to offer: no slash, no control characters, not "." or "..". */
export function cleanFileName(name) {
  const cleaned = Array.from(name.replace(/\//g, "_"), (ch) => (isControl(ch.charCodeAt(0)) ? "_" : ch))
    .join("")
    .trim();
  return cleaned === "." || cleaned === ".." ? "" : cleaned;
}

/* The name a URL suggests: the last part of its path, unescaped. "" when it has none. */
export function fileNameFromUrl(url) {
  let last;
  try {
    last = new URL(url.trim()).pathname.split("/").filter(Boolean).pop() || "";
  } catch {
    return "";
  }
  try {
    last = decodeURIComponent(last);
  } catch {
    /* a broken escape: keep the text as it is */
  }
  return cleanFileName(last);
}

/* "~/Downloads" and "a.iso" -> "~/Downloads/a.iso", whether or not the folder ends in a slash. */
export function joinPath(directory, fileName) {
  const dir = directory.trim();
  if (!dir) return "";
  return `${dir.replace(/\/+$/, "")}/${fileName}`;
}

/* What is wrong with an address that is not empty, as { field: "url", message }, or null: the shape and the length
 * the daemon accepts (DownloadService.validated). */
export function validateUrl(address) {
  if (hasControlChars(address) || !URL_SHAPE.test(address)) {
    return { field: "url", message: "The address must start with http:// or https:// and contain no spaces." };
  }
  if (utf8Length(address) > MAX_URL_BYTES) {
    return { field: "url", message: `The address is too long (at most ${MAX_URL_BYTES} bytes).` };
  }
  return null;
}

/* The first thing wrong with a new download, as { field, message }, or null when the daemon should accept it. */
export function validateDownload({ url, directory, fileName }) {
  const address = url.trim();
  if (!address) return { field: "url", message: "Enter the address to download." };
  const wrong = validateUrl(address);
  if (wrong) return wrong;

  const dir = directory.trim();
  if (!dir) return { field: "directory", message: "Choose a folder to save to." };
  if (hasControlChars(dir)) return { field: "directory", message: "The folder cannot contain control characters." };
  if (!isAbsoluteDirectory(dir)) {
    return { field: "directory", message: "The folder must be an absolute path: start with / or ~/." };
  }

  if (!fileName.trim()) return { field: "fileName", message: "Name the file." };
  if (fileName.includes("/") || hasControlChars(fileName)) {
    return { field: "fileName", message: "The file name cannot contain / or control characters." };
  }
  if (fileName.trim() === "." || fileName.trim() === "..") return { field: "fileName", message: "That is not a file name." };
  if (utf8Length(fileName) > MAX_NAME_BYTES) {
    return { field: "fileName", message: `The file name is too long (at most ${MAX_NAME_BYTES} bytes).` };
  }

  if (utf8Length(normalizePath(joinPath(dir, fileName.trim()))) > MAX_PATH_BYTES) {
    return { field: "directory", message: `The path is too long (at most ${MAX_PATH_BYTES} bytes).` };
  }
  return null;
}
