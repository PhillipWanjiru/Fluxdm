/* FluxDM — electron/tray.js */
const { Tray, Menu } = require("electron");
let tray=null;
function createTray(iconPath) {
  tray = new Tray(iconPath);
  tray.setContextMenu(Menu.buildFromTemplate([
    {label:"Open FluxDM"},{type:"separator"},{label:"Quit",role:"quit"}
  ]));
}
module.exports = { createTray };
