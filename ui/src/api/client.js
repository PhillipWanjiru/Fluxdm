/* FluxDM — api/client.js
 * The daemon asks for "Authorization: Bearer <token>" on every request (see SECURITY.md). The token is the
 * file ~/.fluxdm/api-token, which only this user can read, so a web page cannot fetch it. Where it comes from:
 *   - in the Electron app, the main process reads the file (window.fluxdm.apiToken(), see electron/preload.js);
 *   - in a plain browser while developing:  REACT_APP_FLUXDM_TOKEN="$(cat ~/.fluxdm/api-token)" npm start
 *     (a development server only: the variable is ignored in a production build, so a build never carries it).
 * It is read once, and again after a 401, which is what the daemon answers if it was started after the UI or
 * the token file was deleted to rotate the token. */
import axios from "axios";

export const client = axios.create({ baseURL: "http://localhost:7171/api", timeout: 10000 });

let cached = "";

async function readToken() {
  const fromElectron = window.fluxdm && window.fluxdm.apiToken ? await window.fluxdm.apiToken() : "";
  const fromEnv = process.env.NODE_ENV === "development" ? process.env.REACT_APP_FLUXDM_TOKEN || "" : "";
  return fromElectron || fromEnv;
}

client.interceptors.request.use(async (config) => {
  if (!cached) cached = await readToken();
  if (cached) config.headers.Authorization = `Bearer ${cached}`;
  return config;
});

client.interceptors.response.use(
  (response) => response,
  async (error) => {
    const config = error.config;
    if (error.response && error.response.status === 401 && config && !config.retriedWithNewToken) {
      const fresh = await readToken();
      if (fresh && fresh !== cached) {
        cached = fresh;
        config.retriedWithNewToken = true;
        config.headers.Authorization = `Bearer ${fresh}`;
        return client(config);
      }
    }
    console.error("[FluxDM]", error);
    return Promise.reject(error);
  }
);
