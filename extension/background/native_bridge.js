/* FluxDM — native_bridge.js */
const PORT_NAME = "com.fluxdm.host";
let port = null;
function connect() {
  port = chrome.runtime.connectNative(PORT_NAME);
  port.onDisconnect.addListener(() => { port = null; });
}
function sendToCore(payload) { if (!port) connect(); port.postMessage(payload); }
