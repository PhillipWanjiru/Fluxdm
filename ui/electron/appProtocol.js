/* FluxDM — electron/appProtocol.js
 * Answers a request for app://-/… from the built page (ui/build). Takes anything with a `url` and a `method`, as
 * Electron's request has, and returns { status, headers, body } without throwing, so that shell.js has only to wrap
 * it in a Response and a test can read it without Electron. */
const fs = require("fs");
const { contentSecurityPolicy, mimeType, resolveAppFile } = require("./security");

const TEXT = { "content-type": "text/plain; charset=utf-8", "x-content-type-options": "nosniff" };
const refuse = (status, text, extra = {}) => ({ status, headers: { ...TEXT, ...extra }, body: text });

async function respond(root, request, { csp = contentSecurityPolicy(), readFile = fs.promises.readFile } = {}) {
  const method = String(request.method || "GET").toUpperCase();
  if (method !== "GET" && method !== "HEAD") return refuse(405, "Method not allowed", { allow: "GET, HEAD" });

  const file = resolveAppFile(root, request.url);
  if (file === null) return refuse(404, "Not found");

  let body;
  try {
    body = await readFile(file);
  } catch {
    return refuse(404, "Not found"); /* missing, a directory, unreadable: all just absent */
  }
  return {
    status: 200,
    headers: {
      "content-type": mimeType(file),
      /* on every file, though only the page's own counts: a policy on a script or a style changes nothing */
      "content-security-policy": csp,
      "x-content-type-options": "nosniff",
      "cache-control": "no-store",
    },
    body: method === "HEAD" ? null : body,
  };
}

module.exports = { respond };
