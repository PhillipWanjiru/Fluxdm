import React from "react";
import { render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider, useSelector } from "react-redux";
import { makeStore } from "../../store";
import { received, selectDownloadById } from "../../store/downloadsSlice";
import DownloadItem from "./DownloadItem";
import { cancel, pause, resume } from "../../api/downloads";
import { ApiError } from "../../api/errors";
import { fromDaemon } from "../../model/downloads";
import { daemonDownload } from "../../testing/fixtures";

jest.mock("../../api/downloads");

const dl = (over) => fromDaemon(daemonDownload({ filePath: "/d/a.iso", url: "https://example.com/a.iso", ...over }));

/* The list's job: the row follows the store, and is gone when the download is. */
function Row({ id }) {
  const download = useSelector((s) => selectDownloadById(s, id));
  return download ? <DownloadItem dl={download} /> : <p>gone</p>;
}

function setup(over = {}) {
  const store = makeStore();
  store.dispatch(received([dl({ id: 1, ...over })]));
  const user = userEvent.setup();
  render(
    <Provider store={store}>
      <Row id={1} />
    </Provider>
  );
  return { store, user };
}

const button = (name) => screen.getByRole("button", { name });
const missing = (name) => expect(screen.queryByRole("button", { name })).not.toBeInTheDocument();

beforeEach(() => {
  pause.mockReset();
  resume.mockReset();
  cancel.mockReset();
});

describe("what a download offers", () => {
  test.each([
    ["downloading", { status: 1 }, "Pause"],
    ["queued", { status: 0, speedBps: 0 }, "Pause"],
    ["paused", { status: 2, speedBps: 0 }, "Resume"],
    ["failed", { status: 4, speedBps: 0, errorMessage: "connection reset by peer" }, "Retry"],
  ])("a %s one: %s and Remove", (_state, over, label) => {
    setup(over);
    expect(button(`${label} a.iso`)).toBeEnabled();
    expect(button("Remove a.iso")).toBeEnabled();
    for (const other of ["Pause", "Resume", "Retry"].filter((l) => l !== label)) missing(`${other} a.iso`);
  });

  test("a finished one can only be removed from the list", () => {
    setup({ status: 3, downloaded: 1000, speedBps: 0 });
    expect(button("Remove a.iso")).toBeEnabled();
    for (const label of ["Pause", "Resume", "Retry"]) missing(`${label} a.iso`);
  });
});

describe("pausing, resuming and retrying", () => {
  test("pause, then resume, as the daemon answers", async () => {
    const { user } = setup({ status: 1 });
    pause.mockResolvedValue(dl({ id: 1, status: 2, speedBps: 0 }));
    resume.mockResolvedValue(dl({ id: 1, status: 1 }));

    await user.click(button("Pause a.iso"));
    expect(pause).toHaveBeenCalledWith(1);
    expect(await screen.findByText("PAUSED")).toBeInTheDocument();

    await user.click(button("Resume a.iso"));
    expect(resume).toHaveBeenCalledWith(1);
    expect(await screen.findByText("RUNNING")).toBeInTheDocument();
  });

  test("retry starts a failed download again", async () => {
    const { user } = setup({ status: 4, speedBps: 0, errorMessage: "connection reset by peer" });
    resume.mockResolvedValue(dl({ id: 1, status: 0, downloaded: 0, speedBps: 0 }));

    expect(screen.getAllByText("connection reset by peer").length).toBeGreaterThan(0);
    await user.click(button("Retry a.iso"));

    expect(resume).toHaveBeenCalledWith(1);
    expect(await screen.findByText("QUEUE")).toBeInTheDocument();
    expect(screen.queryByText("connection reset by peer")).not.toBeInTheDocument();
  });

  test("cannot be asked twice while the daemon is answering", async () => {
    const { user } = setup({ status: 1 });
    let answer;
    pause.mockReturnValue(new Promise((resolve) => (answer = resolve)));

    await user.click(button("Pause a.iso"));
    expect(button("Pause a.iso")).toBeDisabled();
    expect(button("Remove a.iso")).toBeDisabled();
    await user.click(button("Pause a.iso"));
    expect(pause).toHaveBeenCalledTimes(1);

    answer(dl({ id: 1, status: 2, speedBps: 0 }));
    expect(await screen.findByRole("button", { name: "Resume a.iso" })).toBeEnabled();
  });

  test("shows what the daemon refused, keeps the buttons usable, and clears it on the next try", async () => {
    const { user } = setup({ status: 1 });
    pause.mockRejectedValueOnce(new ApiError("The daemon is not ready (is fluxdm-core running?).", { kind: "unavailable", status: 503 }));

    await user.click(button("Pause a.iso"));
    expect(await screen.findByRole("alert")).toHaveTextContent("The daemon is not ready (is fluxdm-core running?).");
    expect(button("Pause a.iso")).toBeEnabled();
    expect(screen.getByText("RUNNING")).toBeInTheDocument(); // nothing was claimed

    pause.mockResolvedValueOnce(dl({ id: 1, status: 2, speedBps: 0 }));
    await user.click(button("Pause a.iso"));
    expect(await screen.findByText("PAUSED")).toBeInTheDocument();
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });

  test("a download the daemon no longer has leaves the list", async () => {
    const { user } = setup({ status: 1 });
    pause.mockRejectedValue(new ApiError("no such download", { kind: "not_found", status: 404 }));

    await user.click(button("Pause a.iso"));

    expect(await screen.findByText("gone")).toBeInTheDocument();
  });
});

describe("removing", () => {
  test("asks first, saying what is lost, and does nothing if it is cancelled", async () => {
    const { user } = setup({ status: 1 });

    await user.click(button("Remove a.iso"));
    const dialog = screen.getByRole("alertdialog", { name: "Remove this download?" });
    expect(dialog).toHaveTextContent("a.iso stops, and what was downloaded of it so far is deleted.");

    await user.click(within(dialog).getByRole("button", { name: "Cancel" }));
    expect(screen.queryByRole("alertdialog")).not.toBeInTheDocument();
    expect(cancel).not.toHaveBeenCalled();
    expect(screen.getByTestId("download-1")).toBeInTheDocument();
  });

  test("cancels the download at the daemon once confirmed, and the row goes", async () => {
    const { user } = setup({ status: 2, speedBps: 0 });
    cancel.mockResolvedValue(undefined);

    await user.click(button("Remove a.iso"));
    await user.click(within(screen.getByRole("alertdialog")).getByRole("button", { name: "Remove" }));

    expect(cancel).toHaveBeenCalledWith(1);
    expect(await screen.findByText("gone")).toBeInTheDocument();
  });

  test("tells for a finished download that its file is kept", async () => {
    const { user } = setup({ status: 3, downloaded: 1000, speedBps: 0 });
    await user.click(button("Remove a.iso"));
    const dialog = screen.getByRole("alertdialog", { name: "Remove from the list?" });
    expect(dialog).toHaveTextContent("The file it saved stays where it is.");
  });

  test("keeps the row and says why when the daemon will not", async () => {
    const { user } = setup({ status: 1 });
    cancel.mockRejectedValue(new ApiError("Cannot reach the FluxDM daemon.", { kind: "unreachable" }));

    await user.click(button("Remove a.iso"));
    await user.click(within(screen.getByRole("alertdialog")).getByRole("button", { name: "Remove" }));

    expect(await screen.findByRole("alert")).toHaveTextContent("Cannot reach the FluxDM daemon.");
    expect(screen.getByTestId("download-1")).toBeInTheDocument();
  });

  test("Escape closes the question", async () => {
    const { user } = setup({ status: 1 });
    await user.click(button("Remove a.iso"));
    await user.keyboard("{Escape}");
    expect(screen.queryByRole("alertdialog")).not.toBeInTheDocument();
    expect(cancel).not.toHaveBeenCalled();
  });
});

describe("the card", () => {
  test("opens and closes with its name, which says whether it is open", async () => {
    const { user } = setup({ status: 1 });
    const toggle = screen.getByRole("button", { name: "a.iso" });
    expect(toggle).toHaveAttribute("aria-expanded", "false");

    await user.click(toggle);
    expect(toggle).toHaveAttribute("aria-expanded", "true");
    const details = screen.getByRole("group", { name: "Details of a.iso" });
    expect(toggle).toHaveAttribute("aria-controls", details.id); /* the name says which part it opens */
    expect(details).toHaveTextContent("/d/a.iso");
    expect(details).toHaveTextContent("example.com");

    await user.click(toggle);
    expect(toggle).toHaveAttribute("aria-expanded", "false");
    expect(screen.queryByRole("group", { name: "Details of a.iso" })).not.toBeInTheDocument();
  });

  test("clicking an action does not open it", async () => {
    const { user } = setup({ status: 1 });
    pause.mockResolvedValue(dl({ id: 1, status: 2, speedBps: 0 }));
    await user.click(button("Pause a.iso"));
    expect(screen.getByRole("button", { name: "a.iso" })).toHaveAttribute("aria-expanded", "false");
  });

  test("its bar is a progress bar that says how far it is", () => {
    setup({ status: 1, totalSize: 1000, downloaded: 250 });
    const bar = screen.getByRole("progressbar", { name: "a.iso progress" });
    expect(bar).toHaveAttribute("aria-valuenow", "25");
    expect(bar).toHaveAttribute("aria-valuemax", "100");
  });
});
