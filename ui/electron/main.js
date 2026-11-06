/* FluxDM — electron/main.js
 * The entry point of the desktop shell (package.json "main"). It hands Electron to shell.js with the choices the
 * environment makes; the logic is in shell.js, where it can be tested with a stand-in for Electron.
 *
 *   NODE_ENV=development   show the development server (npm run electron:dev) instead of the built page
 *   FLUXDM_TOKEN_FILE      where the daemon keeps its API token; the daemon's fluxdm.api.token-file, if it is set */
const electron = require("electron");
const os = require("os");
const path = require("path");
const { startShell } = require("./shell");

startShell({
  electron,
  dev: process.env.NODE_ENV === "development",
  buildDir: path.join(__dirname, "..", "build"),
  tokenFile: process.env.FLUXDM_TOKEN_FILE || path.join(os.homedir(), ".fluxdm", "api-token"),
  preload: path.join(__dirname, "preload.js"),
});
