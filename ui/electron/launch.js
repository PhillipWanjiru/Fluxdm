/* FluxDM — electron/launch.js
 * What the environment decides for the shell, as a plain function so that it can be tested: whether this is a
 * development run, where the built page and the preload script are, and where the daemon keeps its API token.
 * main.js hands the result to shell.js together with Electron itself.
 *
 *   NODE_ENV=development   show the development server (npm run electron:dev) instead of the built page
 *   FLUXDM_TOKEN_FILE      where the daemon keeps its API token; the daemon's fluxdm.api.token-file, if it is set */
const path = require("path");

function launchOptions(env, { dirname, homedir }) {
  return {
    dev: env.NODE_ENV === "development",
    buildDir: path.join(dirname, "..", "build"),
    tokenFile: env.FLUXDM_TOKEN_FILE || path.join(homedir, ".fluxdm", "api-token"),
    preload: path.join(dirname, "preload.js"),
  };
}

module.exports = { launchOptions };
