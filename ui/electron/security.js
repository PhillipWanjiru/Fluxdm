/* FluxDM — electron/security.js
 * The decisions the main process takes about what the page may load, reach and ask for. No Electron in here, so that
 * each can be tested on its own (security.test.js); shell.js only applies them. */
const path = require("path");
const { APP_SCHEME, DAEMON_ORIGIN, DEV_ORIGIN } = require("./config");

/* The Content-Security-Policy of the packaged page. Everything is refused unless it is allowed here:
 *   - scripts, styles, images and fonts come from the app itself; no inline script, no eval. (Styles that React
 *     sets through the DOM are not "inline style" to a policy; only style attributes and <style> in markup are.)
 *   - the page may talk to the daemon, over HTTP and over WebSocket (SockJS), and to nothing else on the network;
 *   - no plug-ins, no frames, no workers, no forms, no <base>. */
function contentSecurityPolicy(daemonOrigin = DAEMON_ORIGIN) {
  const { protocol, host } = new URL(daemonOrigin);
  const socket = `${protocol === "https:" ? "wss" : "ws"}://${host}`;
  return [
    "default-src 'none'",
    "script-src 'self'",
    "style-src 'self'",
    "img-src 'self' data:",
    "font-src 'self'",
    `connect-src 'self' ${daemonOrigin} ${socket}`,
    "object-src 'none'",
    "base-uri 'none'",
    "form-action 'none'",
    "frame-ancestors 'none'",
  ].join("; ");
}

/* Whether a URL is the app's own page: app://-/ in the package, the development server when `dev`. Used to decide
 * which navigations to allow and who may ask for the API token. (new URL(...).origin is "null" for a custom
 * scheme, so the scheme and the host are compared themselves.) */
function isTrustedUrl(urlString, { dev = false } = {}) {
  let url;
  try {
    url = new URL(urlString);
  } catch {
    return false;
  }
  if (url.protocol === `${APP_SCHEME}:`) return url.host === "-" && url.username === "" && url.password === "";
  return dev && url.origin === DEV_ORIGIN;
}

/* The file under `root` that an app://-/… address names, or null when it names anything else: another scheme or
 * host, a path that leaves `root` however it is written (.., %2e%2e, %2f, a backslash), a NUL byte. "/" is the page. */
function resolveAppFile(root, requestUrl) {
  let url;
  try {
    url = new URL(requestUrl);
  } catch {
    return null;
  }
  if (url.protocol !== `${APP_SCHEME}:` || url.host !== "-") return null;

  let pathname;
  try {
    pathname = decodeURIComponent(url.pathname);
  } catch {
    return null; /* a malformed % escape */
  }
  if (pathname.includes("\0") || pathname.includes("\\")) return null;

  const parts = pathname.split("/").filter((part) => part !== "");
  if (parts.some((part) => part === "." || part === "..")) return null;

  const file = path.join(root, ...(parts.length === 0 ? ["index.html"] : parts));
  const inside = path.relative(root, file);
  if (inside === "" || inside.startsWith("..") || path.isAbsolute(inside)) return null;
  return file;
}

const TYPES = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".map": "application/json; charset=utf-8",
  ".txt": "text/plain; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".ico": "image/x-icon",
  ".woff": "font/woff",
  ".woff2": "font/woff2",
};

/* What to tell the page a file is. Anything not listed is opaque bytes, which the page cannot run. */
const mimeType = (file) => TYPES[path.extname(file).toLowerCase()] || "application/octet-stream";

module.exports = { contentSecurityPolicy, isTrustedUrl, mimeType, resolveAppFile };
