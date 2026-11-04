/* api/live.js against a STOMP broker that answers the way the daemon does (testing/fakeStompServer.js): the real
 * @stomp/stompjs client, real frames, no network. Delays are a few milliseconds so the tests run in real time. */
import { waitFor } from "@testing-library/react";
import { Link } from "../model/connection";
import { createFakeStompServer } from "../testing/fakeStompServer";
import { openLiveFeed } from "./live";

const FAST = { reconnectDelay: 5, maxReconnectDelay: 40 };

let open; /* feeds opened by the test in progress, closed afterwards */
beforeEach(() => {
  open = [];
});
afterEach(async () => {
  await Promise.all(open.map((feed) => feed.close()));
});

/* Opens a feed on `server`. `log` is everything the handlers were told, in order. */
function feedOn(server, over = {}) {
  const log = [];
  const handlers = {
    onLink: (link, detail) => log.push(["link", link, detail]),
    onReady: () => log.push(["ready", server.connections.map((c) => c.subscriptions.length)]),
    onEvent: (event) => log.push(["event", event]),
    onCore: (event) => log.push(["core", event]),
  };
  const feed = openLiveFeed(handlers, { socket: server.socket, token: async () => "secret", retoken: async () => "secret", ...FAST, ...over });
  open.push(feed);
  return {
    feed,
    log,
    links: () => log.filter(([kind]) => kind === "link").map(([, link]) => link),
    ready: () => log.filter(([kind]) => kind === "ready").length,
  };
}

const live = (f, times = 1) => waitFor(() => expect(f.links().filter((l) => l === Link.LIVE)).toHaveLength(times));

describe("the live feed", () => {
  test("connects with the token, subscribes to both topics, and only then says it is ready", async () => {
    const server = createFakeStompServer();
    const f = feedOn(server);
    await live(f);

    expect(server.connections).toHaveLength(1);
    expect(server.connections[0].connect.Authorization).toBe("Bearer secret");
    expect(server.connections[0].subscriptions.sort()).toEqual(["/topic/core", "/topic/downloads"]);
    expect(f.links()).toEqual([Link.CONNECTING, Link.LIVE]);
    // by the time ready was reported, the broker already had both subscriptions of the one connection
    expect(f.log.find(([kind]) => kind === "ready")).toEqual(["ready", [2]]);
    expect(f.ready()).toBe(1);
  });

  test("hands over the events of each topic, and nothing that is not an event", async () => {
    const server = createFakeStompServer();
    const f = feedOn(server);
    await live(f);

    server.publish("/topic/downloads", { type: "progress", id: 1, downloadedBytes: 10 });
    server.publish("/topic/downloads", "this is not json");
    server.publish("/topic/downloads", "42");
    server.publish("/topic/downloads", "[1,2]");
    server.publish("/topic/downloads", { id: 7 }); // no type
    server.publish("/topic/core", { type: "core", id: 0, state: "down", reason: "ipc closed" });
    server.publish("/topic/downloads", { type: "complete", id: 1 });

    await waitFor(() => expect(f.log.filter(([kind]) => kind === "event" || kind === "core")).toHaveLength(3));
    expect(f.log.filter(([kind]) => kind === "event" || kind === "core")).toEqual([
      ["event", { type: "progress", id: 1, downloadedBytes: 10 }],
      ["core", { type: "core", id: 0, state: "down", reason: "ipc closed" }],
      ["event", { type: "complete", id: 1 }],
    ]);
  });

  test("reconnects when the broker goes away, subscribes again, and says it is ready again", async () => {
    const server = createFakeStompServer();
    const f = feedOn(server);
    await live(f);

    server.dropAll();
    await live(f, 2);

    expect(f.links()).toEqual([Link.CONNECTING, Link.LIVE, Link.OFFLINE, Link.CONNECTING, Link.LIVE]);
    expect(server.connections).toHaveLength(2);
    expect(server.connections[1].subscriptions.sort()).toEqual(["/topic/core", "/topic/downloads"]);
    expect(f.ready()).toBe(2);

    server.publish("/topic/downloads", { type: "complete", id: 3 });
    await waitFor(() => expect(f.log).toContainEqual(["event", { type: "complete", id: 3 }]));
  });

  test("keeps trying while nothing is listening, and connects once something is", async () => {
    const server = createFakeStompServer();
    server.refuse(3);
    const f = feedOn(server);
    await live(f);

    expect(server.connections).toHaveLength(4);
    expect(f.links().filter((l) => l === Link.OFFLINE)).toHaveLength(3);
    expect(f.links().at(-1)).toBe(Link.LIVE);
  });

  test("reports a refused token with the daemon's reason, reads the token again, and then gets in", async () => {
    const server = createFakeStompServer({ token: "rotated" });
    let current = "stale";
    const token = jest.fn(async () => current);
    const retoken = jest.fn(async () => (current = "rotated"));
    const f = feedOn(server, { token, retoken });
    await live(f);

    // refused once, the refusal stays in place while the next attempt is made, and neither "offline" nor
    // "connecting" is shown in between
    expect(f.links()).toEqual([Link.CONNECTING, Link.UNAUTHORIZED, Link.LIVE]);
    expect(f.log.find(([, link]) => link === Link.UNAUTHORIZED)[2]).toBe("missing or invalid API token");
    expect(retoken).toHaveBeenCalledTimes(1);
    expect(server.connections.map((c) => c.connect.Authorization)).toEqual(["Bearer stale", "Bearer rotated"]);
  });

  test("keeps reporting the refusal for as long as the token is wrong", async () => {
    const server = createFakeStompServer({ token: "right" });
    const f = feedOn(server, { token: async () => "wrong", retoken: async () => "wrong" });

    await waitFor(() => expect(server.connections.length).toBeGreaterThanOrEqual(3));
    expect(f.links().every((l) => l === Link.CONNECTING || l === Link.UNAUTHORIZED)).toBe(true);
    expect(f.links().filter((l) => l === Link.CONNECTING)).toHaveLength(1);
  });

  test("with no token, sends no Authorization header, and is told so", async () => {
    const server = createFakeStompServer();
    const f = feedOn(server, { token: async () => "", retoken: async () => "" });

    await waitFor(() => expect(f.links()).toContain(Link.UNAUTHORIZED));
    expect(server.connections[0].connect).not.toHaveProperty("Authorization");
  });

  test("a token that cannot be read does not end the retries", async () => {
    const server = createFakeStompServer();
    const token = jest.fn().mockRejectedValueOnce(new Error("ipc failed")).mockResolvedValue("secret");
    const f = feedOn(server, { token });
    await live(f);

    expect(server.connections[0].connect).not.toHaveProperty("Authorization");
    expect(server.connections[1].connect.Authorization).toBe("Bearer secret");
  });

  test("a dropped connection after a refusal is reported as offline, not as a refusal", async () => {
    const server = createFakeStompServer({ token: "right" });
    let current = "wrong";
    const f = feedOn(server, { token: async () => current, retoken: async () => current });
    await waitFor(() => expect(f.links()).toContain(Link.UNAUTHORIZED));

    server.refuse(100); // the daemon goes away; what the next attempts meet is "cannot connect"
    await waitFor(() => expect(f.links().at(-1)).toBe(Link.OFFLINE));
    expect(f.links().at(-1)).toBe(Link.OFFLINE);
    current = "right";
    server.refuse(0);
    await live(f);
  });

  test("close() ends it: the socket is closed, nothing is reported, and it does not reconnect", async () => {
    const server = createFakeStompServer();
    const f = feedOn(server);
    await live(f);
    const before = f.log.length;

    await f.feed.close();
    await new Promise((resolve) => setTimeout(resolve, 60)); // longer than several retry delays

    expect(server.connections).toHaveLength(1);
    expect(server.connections[0].open).toBe(false);
    expect(f.log).toHaveLength(before);
  });

  test("close() closes the socket itself when the broker never acknowledges the disconnect", async () => {
    const server = createFakeStompServer();
    server.acknowledgeDisconnect = false;
    const f = feedOn(server, { closeGraceMs: 20 });
    await live(f);

    await f.feed.close(); // resolves only once the socket is closed

    expect(server.connections[0].open).toBe(false);
    expect(server.connections[0].closedBy).toBe("client");
  });
});
