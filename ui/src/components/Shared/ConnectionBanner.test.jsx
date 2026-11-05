import React from "react";
import { act, render, screen } from "@testing-library/react";
import { Provider } from "react-redux";
import { Link } from "../../model/connection";
import { makeStore } from "../../store";
import { coreChanged, linkChanged, listFailed } from "../../store/connectionSlice";
import ConnectionBanner from "./ConnectionBanner";

beforeEach(() => jest.useFakeTimers("modern"));
afterEach(() => jest.useRealTimers());

function setup() {
  const store = makeStore();
  render(
    <Provider store={store}>
      <ConnectionBanner />
    </Provider>
  );
  const dispatch = (action) => act(() => void store.dispatch(action));
  const advance = (ms) => act(() => void jest.advanceTimersByTime(ms));
  return { store, dispatch, advance };
}

const region = () => screen.getByRole("status");

describe("the connection banner", () => {
  test("is an empty live region while there is nothing to say", () => {
    const { dispatch } = setup();
    dispatch(linkChanged(Link.LIVE));
    expect(region()).toBeEmptyDOMElement();
  });

  test("waits a second before it appears, so a start-up does not flash it", () => {
    const { advance } = setup(); // connecting, as at every start
    advance(999);
    expect(region()).toBeEmptyDOMElement();
    advance(1);
    expect(region()).toHaveTextContent("Not connected to the FluxDM daemon.");
    expect(screen.getByText(/Not connected/)).toHaveAttribute("data-kind", "warning");
  });

  test("never appears when the connection is there within that second", () => {
    const { dispatch, advance } = setup();
    advance(500);
    dispatch(linkChanged(Link.LIVE));
    advance(5000);
    expect(region()).toBeEmptyDOMElement();
  });

  test("changes its message at once while it is showing, and goes when all is well", () => {
    const { dispatch, advance } = setup();
    advance(1000);
    expect(region()).toHaveTextContent("Not connected");

    dispatch(linkChanged(Link.UNAUTHORIZED));
    expect(region()).toHaveTextContent("refused this app's API token");
    expect(screen.getByText(/refused/)).toHaveAttribute("data-kind", "error");

    dispatch(linkChanged(Link.LIVE));
    expect(region()).toBeEmptyDOMElement();
  });

  test("tells when the core is down, and when the list could not be loaded", () => {
    const { dispatch, advance } = setup();
    dispatch(linkChanged(Link.LIVE));

    dispatch(coreChanged({ up: false, reason: "ipc closed" }));
    advance(1000);
    expect(region()).toHaveTextContent("The download core is not available (ipc closed).");

    dispatch(coreChanged({ up: true }));
    expect(region()).toBeEmptyDOMElement();

    dispatch(listFailed("The daemon is not ready."));
    advance(1000);
    expect(region()).toHaveTextContent("Could not load the list of downloads (The daemon is not ready).");
  });
});
