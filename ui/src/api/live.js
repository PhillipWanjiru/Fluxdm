/* FluxDM — api/live.js
 * The push feed: STOMP over SockJS at /ws (daemon: config/WebSocketConfig). This file only moves frames. What to
 * do with them, and how to catch up after a gap, is store/liveSync.js.
 *
 *   /topic/downloads   progress, status, state, complete, error and cancelled events of every download
 *   /topic/core        the download core going up or down (event id 0)
 *
 * The token goes in the CONNECT frame as the native header "Authorization: Bearer <token>", because a browser
 * cannot put a header on a WebSocket handshake (SECURITY.md). A connection without the right token gets a STOMP
 * ERROR frame and is closed, which is reported as Link.UNAUTHORIZED, with the token read again before the next try.
 *
 * Every attempt asks for the token again, and a lost connection is retried with a growing delay, for as long as the
 * feed is open: the daemon being started after the UI, restarted, or briefly gone is not an error to give up on.
 * A new connection has no subscriptions, so each one subscribes again and then says it is ready (onReady). */
import { Client, ReconnectionTimeMode } from "@stomp/stompjs";
import SockJS from "sockjs-client";
import { Link } from "../model/connection";
import { currentToken, refreshToken } from "./client";
import { SOCKJS_URL } from "./config";

export const TOPIC_DOWNLOADS = "/topic/downloads";
export const TOPIC_CORE = "/topic/core";

/* The browser's own WebSocket first, then the two HTTP fallbacks that need nothing but XHR. SockJS' other
 * transports (iframes) are for browsers that are not Chromium and would only add moving parts. */
const TRANSPORTS = ["websocket", "xhr-streaming", "xhr-polling"];
const defaultSocket = () => new SockJS(SOCKJS_URL, null, { transports: TRANSPORTS });

/* One frame's body as an event object, or null when it is not one (the daemon only sends JSON objects with a
 * "type"; anything else is not for this UI). */
function parse(body) {
  try {
    const event = JSON.parse(body);
    return event && typeof event === "object" && typeof event.type === "string" ? event : null;
  } catch {
    return null;
  }
}

/* Opens the feed and keeps it open until close() is called.
 *
 *   handlers.onLink(link, detail)  the connection changed (Link.*); detail is the daemon's reason when it refused
 *   handlers.onReady()             connected and subscribed: take a snapshot now, events follow from here on
 *   handlers.onEvent(event)        an event from /topic/downloads
 *   handlers.onCore(event)         an event from /topic/core
 *
 * options (for tests): socket, token, retoken, and reconnectDelay, maxReconnectDelay and closeGraceMs in ms.
 * Returns { close }, which stops the feed and resolves once the socket is closed. */
export function openLiveFeed(handlers, options = {}) {
  const {
    socket = defaultSocket,
    token = currentToken,
    retoken = refreshToken,
    reconnectDelay = 1000,
    maxReconnectDelay = 15000,
    closeGraceMs = 1000,
  } = options;

  let connected = false; /* this attempt got its CONNECTED frame */
  let refused = false; /* this attempt got an ERROR frame instead: that, not "offline", is what its close means */
  let unauthorized = false; /* the last attempt to reach the daemon was refused (kept so it does not flicker) */
  let closed = false; /* close() was called: the socket closing is the end, not news */

  const client = new Client({
    webSocketFactory: socket,
    reconnectDelay,
    maxReconnectDelay,
    reconnectTimeMode: ReconnectionTimeMode.EXPONENTIAL,
    connectionTimeout: 10000, /* an attempt that neither connects nor fails is abandoned and retried */
    /* No heartbeat settings: the client offers the library's defaults and the daemon's broker declines them
     * (it sends none), which on a loopback socket loses nothing. If the daemon ever enables them, they apply. */

    beforeConnect: async (self) => {
      connected = false;
      refused = false;
      if (!unauthorized) handlers.onLink(Link.CONNECTING); /* "unauthorized" stays until an attempt gets in */
      let value = "";
      try {
        value = await token();
      } catch {
        /* no token to be had: the daemon will say so, and that is what gets reported */
      }
      self.connectHeaders = value ? { Authorization: `Bearer ${value}` } : {};
    },

    onConnect: () => {
      connected = true;
      unauthorized = false;
      client.subscribe(TOPIC_DOWNLOADS, (frame) => {
        const event = parse(frame.body);
        if (event) handlers.onEvent(event);
      });
      client.subscribe(TOPIC_CORE, (frame) => {
        const event = parse(frame.body);
        if (event) handlers.onCore(event);
      });
      handlers.onLink(Link.LIVE);
      handlers.onReady();
    },

    onStompError: (frame) => {
      /* Before CONNECTED, the daemon refused the CONNECT frame: the token is missing or wrong. After it, an ERROR
       * frame is the broker ending the session, which the close that follows reports. */
      if (connected) return;
      refused = unauthorized = true;
      handlers.onLink(Link.UNAUTHORIZED, (frame.headers && frame.headers.message) || "");
      retoken().catch(() => {}); /* the next attempt asks for it */
    },

    onWebSocketClose: () => {
      if (closed || refused) return;
      unauthorized = false; /* this attempt did not get as far as the daemon's answer */
      handlers.onLink(Link.OFFLINE);
    },
  });

  client.activate();
  return {
    close: () => {
      closed = true;
      const done = client.deactivate(); /* says DISCONNECT and waits for the broker's receipt before closing */
      /* A broker that never answers would leave the socket open for good, so after a moment it is closed here. */
      const timer = setTimeout(() => client.forceDisconnect(), closeGraceMs);
      return done.finally(() => clearTimeout(timer));
    },
  };
}
