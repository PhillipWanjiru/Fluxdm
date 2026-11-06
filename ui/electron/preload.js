/* FluxDM — electron/preload.js
 * What the page can ask of the main process: the daemon's API token, and nothing else. The page runs web content in
 * a sandbox with no Node; whatever is exposed here is the whole of its power over the user's computer, so it is one
 * function with no arguments, not a channel to send on or to listen to. */
const { contextBridge, ipcRenderer } = require("electron");

contextBridge.exposeInMainWorld("fluxdm", {
  apiToken: () => ipcRenderer.invoke("fluxdm:api-token"),
});
