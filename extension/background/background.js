/* FluxDM — background.js */
chrome.downloads.onCreated.addListener(item => {
  chrome.downloads.cancel(item.id);
  chrome.runtime.sendMessage({ type: "INTERCEPT", url: item.url });
});
