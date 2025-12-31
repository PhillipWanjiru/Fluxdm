/* FluxDM — electron/main.js */
const { app, BrowserWindow } = require("electron");
const path = require("path");
function createWindow() {
  const win = new BrowserWindow({ width:1200,height:800,backgroundColor:"#0a0a0a",
    webPreferences:{preload:path.join(__dirname,"preload.js"),contextIsolation:true}});
  win.loadURL(process.env.NODE_ENV==="development" ? "http://localhost:3000"
    : `file://${path.join(__dirname,"../build/index.html")}`);
}
app.whenReady().then(createWindow);
app.on("window-all-closed",()=>{ if(process.platform!=="darwin") app.quit(); });
