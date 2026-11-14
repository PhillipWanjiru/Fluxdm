/* FluxDM — background.js
 *
 * Offers every download the browser starts to FluxDM, and cancels the browser's own copy only once FluxDM has said
 * it took it. Anything else leaves the browser's download alone: no native host installed, a host that fails,
 * crashes or never answers, a reply that is not {"status": "accepted"}. An extension that cancels a download it did
 * not hand over loses the user's file, so the safe state is the default and the hand-off has to earn the cancel.
 *
 * The offer is one native message per download (chrome.runtime.sendNativeMessage), not a port held in a variable:
 * a Manifest V3 service worker is stopped after a short idle time and the port would go with it.
 *
 * What is sent: the address (and the final address after redirects), the referrer, the type and the size the
 * browser knows. No cookies and no page content. Downloads from private windows are never offered: FluxDM keeps
 * a database of what it downloads.
 *
 * The functions take the chrome.* object as an argument and are exported when a module system is present, so the
 * tests can run them against a stand-in. In the service worker only the listener at the bottom runs.
 */
"use strict";

(function () {
  const HOST = "com.fluxdm.host";
  const MAX_URL = 8192;                        // the host accepts no more
  const SCHEMES = ["http:", "https:"];

  /** True for the address of something FluxDM could fetch by itself: http(s), no blanks or control characters. */
  function plainWebAddress(text) {
    if (typeof text !== "string" || text.length === 0 || text.length > MAX_URL) return false;
    if (/[\x00-\x20\x7f-\x9f]/.test(text)) return false;                  // blanks and control characters
    if (!/^https?:\/\//i.test(text)) return false;                        // "https:host/x" parses, but the browser never sends it
    let url;
    try { url = new URL(text); } catch (e) { return false; }
    return SCHEMES.includes(url.protocol);
  }

  /** Whether a download is one to offer: a plain web address, and not from a private window. */
  function eligible(item) {
    return !!item && !item.incognito && plainWebAddress(item.url);
  }

  /** The message about a download. Only fields that passed a check are included. */
  function request(item) {
    const message = { type: "download", url: item.url };
    if (plainWebAddress(item.finalUrl) && item.finalUrl !== item.url) message.final_url = item.finalUrl;
    if (plainWebAddress(item.referrer)) message.referrer = item.referrer;
    if (typeof item.mime === "string" && item.mime.length > 0 && item.mime.length <= 255) message.mime = item.mime;
    if (Number.isSafeInteger(item.fileSize) && item.fileSize > 0) message.size = item.fileSize;
    return message;
  }

  /** Ask the host to take the download. Resolves true only for {"status": "accepted"}, false in every other case
   *  (and never rejects); if the host never answers it does not resolve at all, and the browser carries on. */
  function offer(chromeApi, item) {
    return new Promise(resolve => {
      try {
        chromeApi.runtime.sendNativeMessage(HOST, request(item), reply => {
          if (chromeApi.runtime.lastError) { resolve(false); return; }     // no host, or it exited without answering
          resolve(!!reply && typeof reply === "object" && reply.status === "accepted");
        });
      } catch (e) {
        resolve(false);                                                      // for example: permission missing
      }
    });
  }

  /** Handle one new download. Resolves to "ignored" (not one to offer), "kept" (the browser keeps it) or
   *  "handed-off" (FluxDM has it; the browser's copy is cancelled and removed from its list). */
  async function onCreated(chromeApi, item) {
    if (!eligible(item)) return "ignored";
    if (!(await offer(chromeApi, item))) return "kept";
    await new Promise(resolve => chromeApi.downloads.cancel(item.id, () => { void chromeApi.runtime.lastError; resolve(); }));
    await new Promise(resolve => chromeApi.downloads.erase({ id: item.id }, () => { void chromeApi.runtime.lastError; resolve(); }));
    return "handed-off";
  }

  if (typeof chrome !== "undefined" && chrome.downloads && chrome.downloads.onCreated) {
    chrome.downloads.onCreated.addListener(item => { onCreated(chrome, item).catch(() => {}); });
  }
  if (typeof module !== "undefined" && module.exports) {
    module.exports = { HOST, MAX_URL, plainWebAddress, eligible, request, offer, onCreated };
  }
})();
