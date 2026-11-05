import React from "react";
import { act, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import App from "./App";
import { makeStore } from "./store";
import { limitKnown } from "./store/bandwidthSlice";
import { linkChanged } from "./store/connectionSlice";
import { received } from "./store/downloadsSlice";
import { Link } from "./model/connection";
import { fromDaemon } from "./model/downloads";
import { daemonDownload, daemonList } from "./testing/fixtures";
import { getTasks } from "./api/scheduler";
import { getBandwidthLimit } from "./api/stats";

jest.mock("./hooks/useLiveSync"); /* the feed itself is tested on its own; here the store is driven by hand */
jest.mock("./api/scheduler");
jest.mock("./api/stats");
jest.mock("./api/grabber");

const MB = 1024 * 1024;
const flush = () => act(async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); });

async function setup() {
  const store = makeStore();
  const user = userEvent.setup();
  render(
    <Provider store={store}>
      <App />
    </Provider>
  );
  await flush();
  return { store, user, dispatch: (action) => act(() => void store.dispatch(action)) };
}

const go = (user, name) => user.click(within(screen.getByRole("navigation", { name: "Main" })).getByRole("button", { name }));
const current = () => within(screen.getByRole("navigation", { name: "Main" })).getByRole("button", { current: "page" });
const addUrl = (options) => screen.queryByRole("button", { name: "+ ADD URL", ...options });

beforeEach(() => {
  getTasks.mockReset();
  getTasks.mockResolvedValue([]);
  getBandwidthLimit.mockReset();
  getBandwidthLimit.mockResolvedValue(0);
});

describe("the views", () => {
  test("starts on the download list", async () => {
    await setup();
    expect(current()).toHaveTextContent("Downloads");
    expect(addUrl()).toBeVisible();
  });

  test.each([
    ["Video Grabber", "VIDEO GRABBER"],
    ["Scheduler", "SCHEDULER"],
    ["Settings", "SETTINGS"],
  ])("%s shows its own view, and the download list gives way", async (item, heading) => {
    const { user } = await setup();
    await go(user, item);

    expect(current()).toHaveTextContent(item);
    expect(screen.getByRole("heading", { name: heading })).toBeVisible();
    expect(addUrl()).not.toBeInTheDocument(); /* hidden from what can be reached */
  });

  test("going back to Downloads shows the list again", async () => {
    const { user } = await setup();
    await go(user, "Settings");
    await go(user, "Downloads");
    expect(current()).toHaveTextContent("Downloads");
    expect(addUrl()).toBeVisible();
    expect(screen.queryByRole("heading", { name: "SETTINGS" })).not.toBeInTheDocument();
  });

  test("only one view is on show at a time", async () => {
    const { user } = await setup();
    await go(user, "Scheduler");
    expect(screen.getAllByRole("heading", { level: 2 })).toHaveLength(1);
  });
});

describe("what stays when another view is on show", () => {
  test("the download list keeps its tab", async () => {
    const { user, dispatch } = await setup();
    dispatch(received(daemonList().map(fromDaemon)));
    expect(screen.getByText("a.iso")).toBeInTheDocument(); /* the active tab, the one it opens on */

    await user.click(screen.getByRole("button", { name: /^COMPLETE/i }));
    expect(screen.getByText("d.pdf")).toBeInTheDocument();
    expect(screen.queryByText("a.iso")).not.toBeInTheDocument();

    await go(user, "Settings");
    await go(user, "Downloads");
    expect(screen.getByText("d.pdf")).toBeInTheDocument();
    expect(screen.queryByText("a.iso")).not.toBeInTheDocument();
  });

  test("the download list is still there, only hidden, so its speed history goes on", async () => {
    const { user } = await setup();
    await go(user, "Scheduler");
    expect(addUrl({ hidden: true })).toBeInTheDocument();
    expect(addUrl({ hidden: true })).not.toBeVisible();
  });

  test("the video grabber keeps what was typed in it", async () => {
    const { user } = await setup();
    await go(user, "Video Grabber");
    await user.type(screen.getByLabelText("ADDRESS OF A VIDEO PAGE"), "https://www.example.com/watch?v=abc");

    await go(user, "Downloads");
    await go(user, "Scheduler");
    await go(user, "Video Grabber");
    expect(screen.getByLabelText("ADDRESS OF A VIDEO PAGE")).toHaveValue("https://www.example.com/watch?v=abc");
  });
});

describe("what is read afresh each time it is opened", () => {
  test("the scheduler reads the schedule when it opens, not before, and again when it is opened again", async () => {
    const { user } = await setup();
    expect(getTasks).not.toHaveBeenCalled();

    await go(user, "Scheduler");
    await flush();
    expect(getTasks).toHaveBeenCalledTimes(1);

    await go(user, "Settings");
    await go(user, "Scheduler");
    await flush();
    expect(getTasks).toHaveBeenCalledTimes(2);
  });

  test("the settings open on their first tab each time", async () => {
    const { user } = await setup();
    await go(user, "Settings");
    await user.click(screen.getByRole("button", { name: "Storage" }));
    expect(screen.getByLabelText("DEFAULT DOWNLOAD FOLDER")).toBeInTheDocument();

    await go(user, "Downloads");
    await go(user, "Settings");
    expect(screen.queryByLabelText("DEFAULT DOWNLOAD FOLDER")).not.toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Network" })).toHaveAttribute("aria-pressed", "true");
  });
});

describe("the sidebar's meters", () => {
  test("the bandwidth is read from the daemon once there is a link, and shown against the speed", async () => {
    getBandwidthLimit.mockResolvedValue(4 * MB);
    const { dispatch } = await setup();
    expect(getBandwidthLimit).not.toHaveBeenCalled(); /* no link yet */
    expect(screen.queryByRole("meter")).not.toBeInTheDocument();

    dispatch(received([fromDaemon(daemonDownload({ id: 1, status: 1, speedBps: 1 * MB, filePath: "/d/a.iso", url: "https://example.com/a.iso" }))]));
    dispatch(linkChanged(Link.LIVE));
    await flush();

    expect(getBandwidthLimit).toHaveBeenCalledTimes(1);
    expect(screen.getByRole("meter", { name: "Bandwidth in use" })).toHaveAttribute("aria-valuetext", "1.0 MB/s of 4.0 MB/s");
  });

  test("the daemon indicator follows the link", async () => {
    const { dispatch } = await setup();
    expect(screen.getByTestId("daemon-status")).toHaveTextContent("Connecting…");
    dispatch(linkChanged(Link.LIVE));
    await flush(); /* the limit is read as soon as there is a link */
    expect(screen.getByTestId("daemon-status")).toHaveTextContent("Connected");
    dispatch(linkChanged(Link.OFFLINE));
    expect(screen.getByTestId("daemon-status")).toHaveTextContent("Offline");
  });

  test("a limit changed in the settings shows in the meter", async () => {
    const { store, dispatch } = await setup();
    dispatch(limitKnown(2 * MB));
    expect(screen.getByRole("meter")).toHaveAttribute("aria-valuemax", String(2 * MB));
    expect(store.getState().bandwidth.limitBps).toBe(2 * MB);
  });
});
