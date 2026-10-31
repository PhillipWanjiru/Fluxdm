/* FluxDM — electron/preload.js */
const { contextBridge, ipcRenderer } = require("electron");
contextBridge.exposeInMainWorld("fluxdm", {
  apiToken: () => ipcRenderer.invoke("fluxdm:api-token"),
  send:    (ch,d) => ipcRenderer.send(ch,d),
  receive: (ch,fn) => ipcRenderer.on(ch,(_,...args)=>fn(...args))
});
