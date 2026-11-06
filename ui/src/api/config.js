/* FluxDM — api/config.js
 * Where the daemon listens: server.address and server.port in daemon/src/main/resources/application.properties.
 * The address is the literal 127.0.0.1 and not "localhost": the name can resolve to ::1, where nothing listens.
 * The Electron shell builds the packaged page's Content-Security-Policy from the same address (electron/config.js),
 * and the page can reach nothing else: if you move the daemon to another port, change it there too. The two are
 * compared by api/config.test.js. */
export const DAEMON_ORIGIN = "http://127.0.0.1:7171";
export const API_BASE = `${DAEMON_ORIGIN}/api`;
export const SOCKJS_URL = `${DAEMON_ORIGIN}/ws`;
