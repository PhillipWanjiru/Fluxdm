import React from "react";
import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import { initialSettings } from "../../store/settingsSlice";
import { selectAllDownloads } from "../../store/downloadsSlice";
import AddDownloadModal from "./AddDownloadModal";
import { start } from "../../api/downloads";
import { ApiError } from "../../api/errors";
import { fromDaemon } from "../../model/downloads";
import { daemonDownload } from "../../testing/fixtures";

jest.mock("../../api/downloads");

function setup(settings = {}) {
  const store = makeStore({ settings: { ...initialSettings, ...settings } });
  const onClose = jest.fn();
  const user = userEvent.setup();
  render(
    <Provider store={store}>
      <AddDownloadModal onClose={onClose} />
    </Provider>
  );
  return { store, onClose, user };
}

const url = () => screen.getByLabelText("URL");
const dir = () => screen.getByLabelText("SAVE TO FOLDER");
const name = () => screen.getByLabelText("FILE NAME");
const startButton = () => screen.getByRole("button", { name: /start download/i });

beforeEach(() => {
  start.mockReset();
});

describe("the new-download dialog", () => {
  test("offers the default folder and segment count from the settings", () => {
    setup({ downloadDir: "/data/dl", defaultSegments: 16 });
    expect(dir()).toHaveValue("/data/dl");
    expect(screen.getByRole("button", { name: "16×" })).toHaveAttribute("aria-pressed", "true");
    expect(screen.getByRole("button", { name: "8×" })).toHaveAttribute("aria-pressed", "false");
  });

  test("suggests a file name from the address until the user names the file", async () => {
    const { user } = setup();
    await user.type(url(), "https://example.com/pub/ubuntu-24.04.iso");
    expect(name()).toHaveValue("ubuntu-24.04.iso");
    expect(screen.getByText("→ ~/Downloads/ubuntu-24.04.iso")).toBeInTheDocument();

    await user.clear(name());
    await user.type(name(), "mine.iso");
    await user.type(url(), "?mirror=2");
    expect(name()).toHaveValue("mine.iso"); // the address no longer overwrites it
  });

  test("starts the download at the chosen path, shows it at once, and closes", async () => {
    start.mockResolvedValue(fromDaemon(daemonDownload({ id: 12, status: 0, filePath: "/home/u/Downloads/ubuntu-24.04.iso" })));
    const { store, onClose, user } = setup();
    await user.type(url(), "https://example.com/ubuntu-24.04.iso");
    await user.click(screen.getByRole("button", { name: "4×" }));
    await user.click(startButton());

    expect(start).toHaveBeenCalledWith({ url: "https://example.com/ubuntu-24.04.iso", filePath: "~/Downloads/ubuntu-24.04.iso", segments: 4 });
    expect(selectAllDownloads(store.getState()).map((d) => d.id)).toEqual([12]);
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  test("Enter in a field starts it too, and surrounding spaces do not matter", async () => {
    start.mockResolvedValue(fromDaemon(daemonDownload({ id: 1 })));
    const { user } = setup();
    await user.type(url(), "  https://example.com/a.zip  {Enter}");
    expect(start).toHaveBeenCalledWith(expect.objectContaining({ url: "https://example.com/a.zip", filePath: "~/Downloads/a.zip" }));
  });

  test("says what is wrong instead of sending a request the daemon would refuse", async () => {
    const { user, onClose } = setup();
    await user.click(startButton());
    expect(screen.getByRole("alert")).toHaveTextContent("Enter the address to download.");

    await user.type(url(), "ftp://example.com/a.zip");
    expect(screen.getByRole("alert")).toHaveTextContent("http:// or https://");
    expect(url()).toHaveAttribute("aria-invalid", "true");

    await user.clear(url());
    await user.type(url(), "https://example.com/");
    expect(screen.getByRole("alert")).toHaveTextContent("Name the file.");
    expect(name()).toHaveAttribute("aria-invalid", "true");

    expect(start).not.toHaveBeenCalled();
    expect(onClose).not.toHaveBeenCalled();
  });

  test("keeps the dialog open and shows the daemon's refusal", async () => {
    start.mockRejectedValue(new ApiError("another download is already using /home/u/Downloads/a.zip", { kind: "conflict", status: 409 }));
    const { user, onClose } = setup();
    await user.type(url(), "https://example.com/a.zip");
    await user.click(startButton());

    expect(await screen.findByRole("alert")).toHaveTextContent("another download is already using /home/u/Downloads/a.zip");
    expect(onClose).not.toHaveBeenCalled();
    expect(startButton()).toBeEnabled(); // it can be tried again with another name

    start.mockResolvedValue(fromDaemon(daemonDownload({ id: 2 })));
    await user.clear(name());
    await user.type(name(), "b.zip");
    await user.click(startButton());
    expect(start).toHaveBeenLastCalledWith(expect.objectContaining({ filePath: "~/Downloads/b.zip" }));
    expect(onClose).toHaveBeenCalledTimes(1);
  });

  test("cannot be started twice while the daemon is answering", async () => {
    let finish;
    start.mockReturnValue(new Promise((resolve) => (finish = resolve)));
    const { user } = setup();
    await user.type(url(), "https://example.com/a.zip");
    await user.click(startButton());
    expect(screen.getByRole("button", { name: /starting/i })).toBeDisabled();
    await user.keyboard("{Enter}");
    expect(start).toHaveBeenCalledTimes(1);
    finish(fromDaemon(daemonDownload({ id: 3 })));
  });

  test("Escape and Cancel close it", async () => {
    const first = setup();
    await first.user.keyboard("{Escape}");
    expect(first.onClose).toHaveBeenCalledTimes(1);
    await first.user.click(screen.getByRole("button", { name: "CANCEL" }));
    expect(first.onClose).toHaveBeenCalledTimes(2);
  });
});
