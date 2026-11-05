import React from "react";
import { act, render, screen } from "@testing-library/react";
import { Provider } from "react-redux";
import { Link } from "../../model/connection";
import { makeStore } from "../../store";
import { coreChanged, linkChanged, listFailed } from "../../store/connectionSlice";
import ConnectionStatus from "./ConnectionStatus";

function setup(...actions) {
  const store = makeStore();
  for (const action of actions) store.dispatch(action);
  render(
    <Provider store={store}>
      <ConnectionStatus />
    </Provider>
  );
  return { store, dispatch: (action) => act(() => void store.dispatch(action)) };
}

const shown = () => screen.getByTestId("daemon-status");

describe("the daemon indicator", () => {
  test("is always there, under its own caption", () => {
    setup();
    expect(screen.getByText("DAEMON")).toBeInTheDocument();
  });

  test.each([
    ["while connecting", [], "Connecting…", "warning"],
    ["when the link is live", [linkChanged(Link.LIVE)], "Connected", "ok"],
    ["when live and the core is up", [linkChanged(Link.LIVE), coreChanged({ up: true })], "Connected", "ok"],
    ["when the link was lost", [linkChanged(Link.OFFLINE)], "Offline", "warning"],
    ["when the token was refused", [linkChanged(Link.UNAUTHORIZED)], "Token refused", "error"],
    ["when the core is down", [linkChanged(Link.LIVE), coreChanged({ up: false, reason: "ipc closed" })], "Core down", "error"],
    ["when the list could not be loaded", [linkChanged(Link.LIVE), listFailed("timeout")], "Not in sync", "warning"],
  ])("%s: says %s, kind %s", (_when, actions, label, kind) => {
    setup(...actions);
    expect(shown()).toHaveTextContent(new RegExp(`^${label}$`)); /* in words, not only in colour */
    expect(shown()).toHaveAttribute("data-kind", kind);
  });

  test("carries the banner's sentence as its tooltip", () => {
    setup(linkChanged(Link.LIVE), coreChanged({ up: false, reason: "ipc closed" }));
    expect(shown()).toHaveAttribute("title", expect.stringContaining("The download core is not available (ipc closed)."));
  });

  test("follows the connection as it changes", () => {
    const { dispatch } = setup(linkChanged(Link.LIVE));
    expect(screen.getByText("Connected")).toBeInTheDocument();

    dispatch(linkChanged(Link.OFFLINE));
    expect(screen.getByText("Offline")).toBeInTheDocument();
    expect(screen.queryByText("Connected")).not.toBeInTheDocument();

    dispatch(linkChanged(Link.LIVE));
    expect(screen.getByText("Connected")).toBeInTheDocument();
  });

  test("hides its dot from screen readers, which are told the words", () => {
    setup(linkChanged(Link.LIVE));
    expect(shown()).toContainHTML('aria-hidden="true"');
    expect(shown()).toHaveTextContent(/^Connected$/);
  });
});
