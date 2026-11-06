import { API_BASE, DAEMON_ORIGIN, SOCKJS_URL } from "./config";

/* The Electron shell lives outside src, where the page's bundler cannot see it, so it holds its own copy of the
 * daemon's address, and the Content-Security-Policy that limits the page to that address is built from it. */
const shell = require("../../electron/config");

test("the page and the Electron shell name the same daemon", () => {
  expect(shell.DAEMON_ORIGIN).toBe(DAEMON_ORIGIN);
});

test("the REST and the live-feed addresses are on that daemon", () => {
  expect(API_BASE).toBe(`${DAEMON_ORIGIN}/api`);
  expect(SOCKJS_URL).toBe(`${DAEMON_ORIGIN}/ws`);
});

test("the daemon is addressed by the literal loopback, never by a name that can resolve to ::1", () => {
  expect(new URL(DAEMON_ORIGIN).hostname).toBe("127.0.0.1");
});
