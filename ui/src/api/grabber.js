/* FluxDM — api/grabber.js
 * The video grabber, which the daemon forwards to the Python service (grabber/). Both calls take their
 * arguments as QUERY parameters, which is what the daemon's controller reads; a JSON body is ignored.
 *
 * The daemon reports a failure of the Python service as a normal 200 with {"error": "..."} (ipc/PythonBridge),
 * so a call that "worked" can still have failed: both functions turn that into an ApiError. */
import { client } from "./client";
import { ApiError } from "./errors";

/* Finding the formats of a page takes a while: yt-dlp fetches and parses the page. */
const DETECT_TIMEOUT_MS = 60_000;

function checked(data) {
  if (!data || typeof data !== "object") {
    throw new ApiError("The grabber sent no answer.", { kind: "protocol" });
  }
  if (data.error) throw new ApiError(String(data.error), { kind: "grabber" });
  return data;
}

/* What a page offers: { site, title, thumbnail, duration, formats: [{ id, ext, height, abr }] }. `signal`
 * (AbortController) lets the page stop waiting. */
export async function detect(url, { signal } = {}) {
  const { data } = await client.post("/grabber/detect", null, { params: { url }, timeout: DETECT_TIMEOUT_MS, signal });
  const meta = checked(data);
  return { ...meta, formats: Array.isArray(meta.formats) ? meta.formats.map((f) => ({ ...f, id: String(f.id) })) : [] };
}

/* Downloads the video in the Python service and answers when it is done, which can take as long as the
 * download does: no timeout, and a `signal` (AbortController) so the page can stop waiting. */
export async function grab(url, formatId = "best", { signal } = {}) {
  const { data } = await client.post("/grabber/grab", null, { params: { url, formatId }, timeout: 0, signal });
  return checked(data);
}
