/* FluxDM — electron/main.js
 * The entry point of the desktop shell (package.json "main"). It hands Electron to shell.js with the choices the
 * environment makes (launch.js); the logic is in shell.js, where it can be tested with a stand-in for Electron. */
const electron = require("electron");
const os = require("os");
const { launchOptions } = require("./launch");
const { startShell } = require("./shell");

startShell({ electron, ...launchOptions(process.env, { dirname: __dirname, homedir: os.homedir() }) });
