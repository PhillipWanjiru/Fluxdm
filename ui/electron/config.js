/* FluxDM — electron/config.js
 * What the main process has to know that the page also knows. The daemon's address is written twice, here and in
 * src/api/config.js (the page's code cannot import from outside src, nor this file from inside it, without a bundler
 * in between); src/api/config.test.js fails if the two ever differ. */
module.exports = Object.freeze({
  /* Where the daemon listens: server.address and server.port in daemon/src/main/resources/application.properties. */
  DAEMON_ORIGIN: "http://127.0.0.1:7171",

  /* The packaged page is served under app://-/ by main.js, not loaded from file://: a page opened from a file sends
   * "Origin: null", which the daemon refuses, and has no origin of its own for a policy to name. The host is "-"
   * because a standard scheme needs one, and this is the origin the daemon allows (fluxdm.api.allowed-origins). */
  APP_SCHEME: "app",
  APP_ORIGIN: "app://-",

  /* The development server (npm start), when the app is run with NODE_ENV=development. */
  DEV_ORIGIN: "http://localhost:3000",
});
