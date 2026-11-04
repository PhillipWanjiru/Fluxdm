/* FluxDM — api/client.js
 * The daemon asks for "Authorization: Bearer <token>" on every request (see SECURITY.md). The token is the
 * file ~/.fluxdm/api-token, which only this user can read, so a web page cannot fetch it. Where it comes from:
 *   - in the Electron app, the main process reads the file (window.fluxdm.apiToken(), see electron/preload.js);
 *   - in a plain browser while developing:  REACT_APP_FLUXDM_TOKEN="$(cat ~/.fluxdm/api-token)" npm start
 *     (a development server only: the variable is ignored in a production build, so a build never carries it).
 * It is read once, and again after a 401 (or after the live feed was refused, api/live.js), which is what the
 * daemon answers if it was started after the UI or the token file was deleted to rotate the token.
 *
 * A failed call rejects with an ApiError (api/errors.js), never with axios' own error. */
import axios from "axios";
import { API_BASE } from "./config";
import { toApiError } from "./errors";

export const client = axios.create({ baseURL: API_BASE, timeout: 10000 });

let cached = "";

async function readToken() {
  const fromElectron = window.fluxdm && window.fluxdm.apiToken ? await window.fluxdm.apiToken() : "";
  const fromEnv = process.env.NODE_ENV === "development" ? process.env.REACT_APP_FLUXDM_TOKEN || "" : "";
  return fromElectron || fromEnv;
}

/* The token to send now: the one read before, or read it if there is none yet ("" when there is no token). */
export async function currentToken() {
  if (!cached) cached = await readToken();
  return cached;
}

/* Reads the token again, after the daemon refused the one in use, and keeps what is there now. */
export async function refreshToken() {
  cached = await readToken();
  return cached;
}

client.interceptors.request.use(async (config) => {
  const token = await currentToken();
  if (token) config.headers.Authorization = `Bearer ${token}`;
  return config;
});

client.interceptors.response.use(
  (response) => response,
  async (error) => {
    const config = error.config;
    if (error.response && error.response.status === 401 && config && !config.retriedWithNewToken) {
      const before = cached;
      const fresh = await refreshToken();
      if (fresh && fresh !== before) {
        config.retriedWithNewToken = true;
        config.headers.Authorization = `Bearer ${fresh}`;
        return client(config);
      }
    }
    return Promise.reject(toApiError(error));
  }
);
