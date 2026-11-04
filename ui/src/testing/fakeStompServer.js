/* A STOMP broker behind a fake socket, for tests of the live feed: no network, but the real @stomp/stompjs client
 * talks to it in real STOMP frames, and it answers the way the daemon does (security/StompAuthInterceptor): a CONNECT
 * with the wrong token gets an ERROR frame and the socket is closed. Not a test file itself.
 *
 *   const server = createFakeStompServer({ token: "secret" });
 *   openLiveFeed(handlers, { socket: server.socket, ... });
 *
 *   server.connections        every socket that was opened, oldest first:
 *                             { connect (the CONNECT frame's headers), subscriptions (destinations), open, closedBy }
 *   server.publish(dest, body)  a MESSAGE to everyone subscribed to dest; body is an object (sent as JSON) or a
 *                             string (sent as it is, to test what is not JSON)
 *   server.dropAll()          the broker goes away: every open socket closes
 *   server.refuse(n)          the next n sockets never open, as when nothing is listening
 *   server.token              the token that is accepted; change it to rotate
 *   server.acknowledgeDisconnect  false: a DISCONNECT is not answered with a RECEIPT (a broker that has hung) */

const OPEN = 1;
const CLOSED = 3;

function parseFrame(text) {
  const [head, ...rest] = text.split("\n\n");
  const lines = head.split("\n");
  const command = lines.shift();
  const headers = {};
  for (const line of lines) {
    const at = line.indexOf(":");
    if (at > 0 && !(line.slice(0, at) in headers)) headers[line.slice(0, at)] = line.slice(at + 1);
  }
  return { command, headers, body: rest.join("\n\n").replace(/\0$/, "") };
}

const frame = (command, headers, body = "") =>
  `${command}\n${Object.entries(headers).map(([k, v]) => `${k}:${v}`).join("\n")}\n\n${body}\0`;

export function createFakeStompServer({ token = "secret", refusal = "missing or invalid API token" } = {}) {
  const server = {
    token,
    acknowledgeDisconnect: true,
    connections: [],
    refusing: 0,
    refuse(n) {
      server.refusing = n;
    },
    dropAll() {
      for (const c of server.connections) if (c.socket.readyState === OPEN) c.socket.finish(1006, "server");
    },
    publish(destination, body) {
      const text = typeof body === "string" ? body : JSON.stringify(body);
      for (const c of server.connections) {
        if (c.socket.readyState !== OPEN || !c.authenticated) continue;
        for (const [id, dest] of c.subscriptionIds) {
          if (dest === destination) {
            c.socket.deliver(
              frame("MESSAGE", { destination, subscription: id, "message-id": `m${Math.random()}`, "content-type": "application/json" }, text)
            );
          }
        }
      }
    },
    socket() {
      const connection = {
        connect: null,
        authenticated: false,
        subscriptionIds: new Map(),
        closedBy: null,
        get subscriptions() {
          return [...connection.subscriptionIds.values()];
        },
        get open() {
          return connection.socket.readyState === OPEN;
        },
      };
      const socket = {
        url: "fake://broker/ws",
        readyState: 0,
        binaryType: "blob",
        onopen: null,
        onmessage: null,
        onclose: null,
        onerror: null,

        send(data) {
          if (socket.readyState !== OPEN) return;
          const text = typeof data === "string" ? data : new TextDecoder().decode(data);
          if (text.trim() === "") return; /* a heartbeat */
          const { command, headers } = parseFrame(text);
          if (command === "CONNECT" || command === "STOMP") {
            connection.connect = headers;
            if (headers.Authorization === `Bearer ${server.token}`) {
              connection.authenticated = true;
              socket.deliver(frame("CONNECTED", { version: "1.2", "heart-beat": "0,0" }));
            } else {
              socket.deliver(frame("ERROR", { message: refusal, "content-type": "text/plain" }, refusal));
              setTimeout(() => socket.finish(1002, "protocol error", "server"), 0);
            }
          } else if (!connection.authenticated) {
            socket.deliver(frame("ERROR", { message: "not connected" }));
            setTimeout(() => socket.finish(1002, "protocol error", "server"), 0);
          } else if (command === "SUBSCRIBE") {
            connection.subscriptionIds.set(headers.id, headers.destination);
          } else if (command === "UNSUBSCRIBE") {
            connection.subscriptionIds.delete(headers.id);
          } else if (command === "DISCONNECT") {
            if (headers.receipt && server.acknowledgeDisconnect) socket.deliver(frame("RECEIPT", { "receipt-id": headers.receipt }));
          }
        },

        close() {
          if (socket.readyState === CLOSED) return;
          socket.readyState = 2;
          setTimeout(() => socket.finish(1000, "", "client"), 0);
        },

        /* ── the broker's side ── */
        deliver(text) {
          if (socket.readyState === OPEN && socket.onmessage) socket.onmessage({ data: text });
        },
        finish(code, reason = "", by = "server") {
          if (socket.readyState === CLOSED) return;
          socket.readyState = CLOSED;
          connection.closedBy = by;
          if (socket.onclose) socket.onclose({ code, reason, wasClean: code === 1000 });
        },
      };
      connection.socket = socket;
      server.connections.push(connection);

      setTimeout(() => {
        if (socket.readyState !== 0) return;
        if (server.refusing > 0) {
          server.refusing -= 1;
          socket.finish(2000, "Cannot connect to server", "network"); /* what SockJS reports for it */
          return;
        }
        socket.readyState = OPEN;
        if (socket.onopen) socket.onopen({});
      }, 0);
      return socket;
    },
  };
  return server;
}
