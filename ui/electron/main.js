/* FluxDM — electron/main.js */
const { app, BrowserWindow, ipcMain } = require("electron");
const fs = require("fs");
const os = require("os");
const path = require("path");

/* The daemon's API token (SECURITY.md). Only this process can read the file; the page asks for the text over IPC.
 * FLUXDM_TOKEN_FILE overrides the place, like fluxdm.api.token-file does for the daemon. An empty answer means
 * the daemon has not created the file yet: the page asks again after its next 401. */
const tokenFile = () => process.env.FLUXDM_TOKEN_FILE || path.join(os.homedir(), ".fluxdm", "api-token");
ipcMain.handle("fluxdm:api-token", async () => {
  try { return (await fs.promises.readFile(tokenFile(), "utf8")).trim(); } catch { return ""; }
});

function createWindow() {
  const win = new BrowserWindow({ width:1200,height:800,backgroundColor:"#0a0a0a",
    webPreferences:{preload:path.join(__dirname,"preload.js"),contextIsolation:true}});
  win.loadURL(process.env.NODE_ENV==="development" ? "http://localhost:3000"
    : `file://${path.join(__dirname,"../build/index.html")}`);
}
app.whenReady().then(createWindow);
app.on("window-all-closed",()=>{ if(process.platform!=="darwin") app.quit(); });
