/* FluxDM — electron/shell.js
 * The desktop shell: one window that shows the built page, and the one thing the page cannot do for itself, which is
 * to read the daemon's API token from the user's disk (SECURITY.md).
 *
 * The page is web content and is treated as such. It runs in a sandbox with no Node. It is served from app://-/ with
 * a Content-Security-Policy instead of being opened from a file (a file page sends "Origin: null", which the daemon
 * refuses). It cannot navigate away, open windows or webviews, or be granted a permission, and only the page itself,
 * not a frame inside it, can ask for the token.
 *
 * Electron is passed in, so that all of this can be run against a stand-in (shell.test.js); main.js passes the real
 * one. The rules themselves are in security.js and appProtocol.js. */
const fs = require("fs");
const path = require("path");
const { APP_ORIGIN, APP_SCHEME, DEV_ORIGIN } = require("./config");
const { respond } = require("./appProtocol");
const { isTrustedUrl } = require("./security");

const TOKEN_CHANNEL = "fluxdm:api-token";

function startShell({ electron, dev, buildDir, tokenFile, preload, platform = process.platform }) {
  const { app, BrowserWindow, dialog, ipcMain, protocol, session } = electron;
  const trusted = (url) => isTrustedUrl(url, { dev });

  /* Before the app is ready, or Electron refuses. standard: the scheme has a host and an origin, app://-, which the
   * daemon's Origin check names; secure: the page is a secure context; supportFetchAPI: it may call fetch(). */
  protocol.registerSchemesAsPrivileged([
    { scheme: APP_SCHEME, privileges: { standard: true, secure: true, supportFetchAPI: true } },
  ]);

  if (!app.requestSingleInstanceLock()) {
    app.quit(); /* FluxDM is already open; that one raises its window when it hears of this one */
    return;
  }

  /* The daemon's API token. An empty answer means the daemon has not created the file yet, or that the asker is not
   * the app's own page: the page asks again after its next 401. */
  ipcMain.handle(TOKEN_CHANNEL, async (event) => {
    const frame = event.senderFrame;
    if (!frame || frame.parent || !trusted(frame.url)) return "";
    try {
      return (await fs.promises.readFile(tokenFile, "utf8")).trim();
    } catch {
      return "";
    }
  });

  const createWindow = () => {
    if (!dev && !fs.existsSync(path.join(buildDir, "index.html"))) {
      dialog.showErrorBox("FluxDM", "The interface has not been built yet.\n\nRun “npm run build” in the ui folder, then start FluxDM again.");
      app.quit();
      return;
    }

    const win = new BrowserWindow({
      width: 1200,
      height: 800,
      backgroundColor: "#0a0a0a",
      webPreferences: {
        preload,
        contextIsolation: true, /* the page cannot reach into the preload's world */
        nodeIntegration: false, /* no require() in the page */
        sandbox: true, /* the renderer is confined; the preload gets only Electron's small allowed set */
        webSecurity: true,
        allowRunningInsecureContent: false,
        webviewTag: false,
      },
    });

    const contents = win.webContents;
    /* (event.url is the current way to read it; the second argument is the one Electron 29 still passes.) */
    const stay = (event, url) => {
      if (!trusted(event.url ?? url)) event.preventDefault();
    };
    contents.on("will-navigate", stay);
    contents.on("will-redirect", stay);
    contents.on("will-attach-webview", (event) => event.preventDefault());
    contents.setWindowOpenHandler(() => ({ action: "deny" })); /* nothing in the page opens a window */

    win.loadURL(dev ? DEV_ORIGIN : `${APP_ORIGIN}/`);
  };

  app.on("second-instance", () => {
    const [win] = BrowserWindow.getAllWindows();
    if (!win) return;
    if (win.isMinimized()) win.restore();
    win.focus();
  });
  app.on("window-all-closed", () => {
    if (platform !== "darwin") app.quit();
  });
  app.on("activate", () => {
    if (BrowserWindow.getAllWindows().length === 0) createWindow();
  });

  app.whenReady().then(() => {
    const ses = session.defaultSession;
    ses.protocol.handle(APP_SCHEME, async (request) => {
      const { status, headers, body } = await respond(buildDir, request);
      return new Response(body, { status, headers });
    });
    /* No permission is asked for yet (camera, location, notifications, clipboard…). When a feature needs one, allow
     * it here by name, for the app's own page only. */
    ses.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
    ses.setPermissionCheckHandler(() => false);
    createWindow();
  });
}

module.exports = { startShell, TOKEN_CHANNEL };
