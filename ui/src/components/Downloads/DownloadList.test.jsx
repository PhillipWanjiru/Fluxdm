import React from "react";
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import DownloadList from "./DownloadList";
import { received } from "../../store/downloadsSlice";
import { fromDaemon } from "../../model/downloads";
import { daemonList } from "../../testing/fixtures";

/* The list shows what is in the store; filling it is the live sync's job (store/liveSync.js). */
function renderList(downloads = daemonList().map(fromDaemon)) {
  const store = makeStore();
  store.dispatch(received(downloads));
  return render(
    <Provider store={store}>
      <DownloadList />
    </Provider>
  );
}

const tab = (name) => screen.getByRole("button", { name: new RegExp(`^${name}\\s*\\d+$`, "i") });

describe("the downloads list", () => {
  test("shows the daemon's downloads under the tab their state belongs to", async () => {
    renderList();

    // active: the running and the paused download, newest first
    expect(await screen.findByText("a.iso")).toBeInTheDocument();
    expect(screen.getByText("b.zip")).toBeInTheDocument();
    expect(screen.queryByText("c.mp4")).not.toBeInTheDocument();
    expect(screen.queryByText("d.pdf")).not.toBeInTheDocument();
    expect(screen.queryByText("e.bin")).not.toBeInTheDocument();

    const user = userEvent.setup();
    await user.click(tab("queued"));
    expect(screen.getByText("c.mp4")).toBeInTheDocument();
    expect(screen.queryByText("a.iso")).not.toBeInTheDocument();

    await user.click(tab("complete"));
    expect(screen.getByText("d.pdf")).toBeInTheDocument();
    expect(screen.getByText("100%")).toBeInTheDocument();

    await user.click(tab("failed"));
    expect(screen.getByText("e.bin")).toBeInTheDocument();
    expect(screen.getByText("FAILED")).toBeInTheDocument();
    expect(screen.getByText("connection reset by peer")).toBeInTheDocument();
  });

  test("counts each tab", async () => {
    renderList();
    await screen.findByText("a.iso");

    expect(tab("active")).toHaveTextContent("2");
    expect(tab("queued")).toHaveTextContent("1");
    expect(tab("complete")).toHaveTextContent("1");
    expect(tab("failed")).toHaveTextContent("1");
  });

  test("says so when there is nothing to show", async () => {
    renderList([]);
    expect(await screen.findByText("NO DOWNLOADS IN THIS VIEW")).toBeInTheDocument();
    expect(tab("active")).toHaveTextContent("0");
  });

  test("a running download shows its speed and the time left; a paused one does not", async () => {
    renderList();
    const running = await screen.findByTestId("download-1");
    expect(within(running).getByText("100 B/s")).toBeInTheDocument();
    expect(within(running).getByText(/8s left/)).toBeInTheDocument();
    expect(within(running).getByText("RUNNING")).toBeInTheDocument();

    const paused = screen.getByTestId("download-2");
    expect(within(paused).getByText("PAUSED")).toBeInTheDocument();
    expect(within(paused).queryByText(/left/)).not.toBeInTheDocument();
  });

  test("a card opens to show where the file goes", async () => {
    renderList();
    const user = userEvent.setup();
    await user.click(await screen.findByText("a.iso"));
    expect(screen.getByText("/d/a.iso")).toBeInTheDocument();
    expect(screen.getByText("example.com")).toBeInTheDocument();
  });

  test("the cards count what is running, queued, done and all, and the speed is that of the running downloads", async () => {
    renderList();
    await screen.findByText("a.iso");

    const card = (label) => screen.getByRole("group", { name: label });
    expect(card("RUNNING")).toHaveTextContent("1");
    expect(card("QUEUED")).toHaveTextContent("1");
    expect(card("COMPLETED")).toHaveTextContent("1");
    expect(card("TOTAL")).toHaveTextContent("5");

    // download 1 runs at 100 B/s; the paused one (which the active tab also shows) does not count
    expect(screen.getByText("↓ 100 B/s")).toBeInTheDocument();
    expect(screen.getByRole("img", { name: "Combined download speed over the last minute" })).toBeInTheDocument();
  });
});
