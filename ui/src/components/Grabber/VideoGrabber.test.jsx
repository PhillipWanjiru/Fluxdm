import React from "react";
import { act, fireEvent, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import VideoGrabber from "./VideoGrabber";
import { detect, grab } from "../../api/grabber";
import { ApiError } from "../../api/errors";

jest.mock("../../api/grabber");

const PAGE = "https://www.example.com/watch?v=abc&t=30s";

const META = {
  site: "youtube",
  title: "A short clip",
  thumbnail: "https://img.example.com/clip.jpg",
  duration: 185,
  formats: [
    { id: "140", ext: "m4a", height: null, abr: 129 },
    { id: "18", ext: "mp4", height: 360, abr: 96 },
    { id: "137", ext: "mp4", height: 1080, abr: null },
    { id: "sb0", ext: "mhtml", height: 45, abr: null },
  ],
};

/* A call to the grabber that stays open until the test answers it, and that fails the way a real one does when it
 * is abandoned. Each call is recorded: { args, signal, resolve, reject }. */
function openCalls(mock) {
  const calls = [];
  mock.mockImplementation(
    (...args) =>
      new Promise((resolve, reject) => {
        const { signal } = args[args.length - 1];
        signal.addEventListener("abort", () => reject(new ApiError("Cancelled.", { kind: "cancelled" })));
        calls.push({ args, signal, resolve, reject });
      })
  );
  return calls;
}

const flush = () => act(async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); });

function setup() {
  const user = userEvent.setup();
  const view = render(<VideoGrabber />);
  return { user, ...view };
}

const address = () => screen.getByLabelText("ADDRESS OF A VIDEO PAGE");
const detectButton = () => screen.getByRole("button", { name: /^DETECT/ });
const dialog = () => screen.getByRole("dialog", { name: "CHOOSE A FORMAT" });

/* Pastes the address and has it looked up; the lookup is answered with `meta` unless `calls` is given. */
async function lookUp(user, text = PAGE) {
  await user.clear(address());
  if (text) await user.type(address(), text);
  await user.click(detectButton());
}

beforeEach(() => {
  detect.mockReset();
  grab.mockReset();
});

describe("looking a page up", () => {
  test("asks for an address when there is none, and for a proper one when there is not", async () => {
    const { user } = setup();

    await user.click(detectButton());
    expect(screen.getByRole("alert")).toHaveTextContent("Paste the address of a video page.");
    expect(address()).toHaveAttribute("aria-invalid", "true");

    await lookUp(user, "www.example.com/watch");
    expect(screen.getByRole("alert")).toHaveTextContent("The address must start with http:// or https://");

    await lookUp(user, "file:///etc/passwd");
    expect(screen.getByRole("alert")).toHaveTextContent("The address must start with http:// or https://");
    expect(detect).not.toHaveBeenCalled();
  });

  test("the message goes when the address is edited", async () => {
    const { user } = setup();
    await user.click(detectButton());
    expect(screen.getByRole("alert")).toBeInTheDocument();
    await user.type(address(), "h");
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(address()).toHaveAttribute("aria-invalid", "false");
  });

  test("sends the address as typed (without the spaces around it) and offers the formats found", async () => {
    const { user } = setup();
    const calls = openCalls(detect);
    await lookUp(user, `  ${PAGE}  `);

    expect(detect).toHaveBeenCalledTimes(1);
    expect(calls[0].args[0]).toBe(PAGE);
    expect(detectButton()).toBeDisabled();
    expect(detectButton()).toHaveTextContent("DETECTING…");
    expect(screen.getByRole("status")).toHaveTextContent("Reading the page for its formats.");
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();

    await act(async () => calls[0].resolve(META));

    const box = dialog();
    expect(box).toHaveTextContent("A short clip · youtube · 3:05");
    const radios = within(box).getAllByRole("radio");
    expect(radios.map((r) => r.value)).toEqual(["best", "137", "18", "140"]); /* the preview images are not offered */
    expect(radios[0]).toBeChecked();
    expect(radios[0]).toHaveFocus();
    expect(within(box).getByRole("radio", { name: /1080p · MP4/ })).toBeInTheDocument();
    expect(within(box).getByRole("radio", { name: /audio only · M4A · 129 kbps/ })).toBeInTheDocument();
    expect(detectButton()).toBeEnabled();
    expect(screen.queryAllByRole("img")).toHaveLength(0); /* the page's thumbnail is not loaded from the site */
    expect(screen.queryAllByRole("presentation")).toHaveLength(0);
  });

  test("says why a page could not be read", async () => {
    const { user } = setup();
    detect.mockRejectedValue(new ApiError("ERROR: Unsupported URL: https://www.example.com/watch", { kind: "grabber" }));
    await lookUp(user);

    expect(screen.getByRole("alert")).toHaveTextContent("ERROR: Unsupported URL: https://www.example.com/watch");
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(detectButton()).toBeEnabled();
    expect(screen.getByRole("status")).toBeEmptyDOMElement();
  });

  test("can be cancelled, and that is not an error", async () => {
    const { user } = setup();
    const calls = openCalls(detect);
    await lookUp(user);
    await user.click(within(screen.getByRole("status")).getByRole("button", { name: "Cancel" }));
    await flush();

    expect(calls[0].signal.aborted).toBe(true);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(detectButton()).toBeEnabled();
    expect(detectButton()).toHaveTextContent("DETECT");
  });

  test("a second lookup is not started while one is under way", async () => {
    const { user } = setup();
    openCalls(detect);
    await lookUp(user);
    fireEvent.submit(screen.getByRole("form", { name: "Look up a video page" }));
    expect(detect).toHaveBeenCalledTimes(1);
  });

  test("gives up on the call when the view is closed", async () => {
    const { user, unmount } = setup();
    const calls = openCalls(detect);
    await lookUp(user);
    unmount();
    expect(calls[0].signal.aborted).toBe(true);
  });

  test("shows the sites it can read until something has come of a lookup", async () => {
    const { user } = setup();
    expect(screen.getByRole("list", { name: "Sites" })).toBeInTheDocument();
    detect.mockResolvedValue(META);
    await lookUp(user);
    await user.click(within(dialog()).getByRole("button", { name: "Cancel" }));
    expect(screen.getByRole("list", { name: "Sites" })).toBeInTheDocument();
  });
});

describe("choosing a format and fetching it", () => {
  async function atTheDialog(user) {
    detect.mockResolvedValue(META);
    await lookUp(user);
    await flush();
    return dialog();
  }

  test("fetches the best file unless another is chosen", async () => {
    const { user } = setup();
    const calls = openCalls(grab);
    await atTheDialog(user);
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));

    expect(grab).toHaveBeenCalledTimes(1);
    expect(calls[0].args.slice(0, 2)).toEqual([PAGE, "best"]);
  });

  test("fetches the format chosen, and while it does, says so, locks the choice and offers to stop waiting", async () => {
    const { user } = setup();
    const calls = openCalls(grab);
    const box = await atTheDialog(user);

    await user.click(within(box).getByRole("radio", { name: /1080p/ }));
    await user.click(within(box).getByRole("button", { name: "Download" }));

    expect(calls[0].args.slice(0, 2)).toEqual([PAGE, "137"]);
    expect(within(box).getByRole("status")).toHaveTextContent("Downloading in the grabber.");
    expect(within(box).getByRole("button", { name: "Downloading…" })).toBeDisabled();
    expect(within(box).getByRole("button", { name: "Stop waiting" })).toBeEnabled();
    for (const radio of within(box).getAllByRole("radio")) expect(radio).toBeDisabled();
    expect(detectButton()).toBeDisabled();
  });

  test("when it is done, closes the dialog and says what was saved", async () => {
    const { user } = setup();
    const calls = openCalls(grab);
    await atTheDialog(user);
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));
    await act(async () => calls[0].resolve({ status: "ok", title: "A short clip", url: PAGE }));

    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(screen.getByRole("status")).toHaveTextContent("Saved “A short clip” in the grabber’s download folder (by default ~/Downloads).");
    expect(detectButton()).toBeEnabled();
  });

  test("the note goes when another page is looked up", async () => {
    const { user } = setup();
    const calls = openCalls(grab);
    await atTheDialog(user);
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));
    await act(async () => calls[0].resolve({ status: "ok", title: "A short clip" }));
    expect(screen.getByRole("status")).toHaveTextContent("Saved");

    detect.mockReturnValue(new Promise(() => {}));
    await user.click(detectButton());
    expect(screen.getByRole("status")).not.toHaveTextContent("Saved");
  });

  test("a failure is shown in the dialog, which stays, so another format can be tried", async () => {
    const { user } = setup();
    grab.mockRejectedValueOnce(new ApiError("ERROR: Requested format is not available", { kind: "grabber" }));
    const box = await atTheDialog(user);

    await user.click(within(box).getByRole("radio", { name: /1080p/ }));
    await user.click(within(box).getByRole("button", { name: "Download" }));
    await flush();

    expect(within(box).getByRole("alert")).toHaveTextContent("ERROR: Requested format is not available");
    expect(within(box).getByRole("radio", { name: /1080p/ })).toBeChecked(); /* the choice is kept */
    expect(within(box).getByRole("button", { name: "Download" })).toBeEnabled();

    grab.mockResolvedValueOnce({ status: "ok", title: "A short clip" });
    await user.click(within(box).getByRole("radio", { name: /360p/ }));
    await user.click(within(box).getByRole("button", { name: "Download" }));
    await flush();

    expect(grab).toHaveBeenLastCalledWith(PAGE, "18", expect.anything());
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(screen.getByRole("status")).toHaveTextContent("Saved");
  });

  test("the error of a failed try goes when the dialog is opened again", async () => {
    const { user } = setup();
    grab.mockRejectedValueOnce(new ApiError("ERROR: nope", { kind: "grabber" }));
    await atTheDialog(user);
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));
    await flush();
    expect(within(dialog()).getByRole("alert")).toBeInTheDocument();
    await user.click(within(dialog()).getByRole("button", { name: "Cancel" }));

    await lookUp(user);
    await flush();
    expect(within(dialog()).queryByRole("alert")).not.toBeInTheDocument();
  });

  test("stopping the wait lets go of the call, closes the dialog and says the grabber may still finish", async () => {
    const { user } = setup();
    const calls = openCalls(grab);
    await atTheDialog(user);
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));
    await user.click(within(dialog()).getByRole("button", { name: "Stop waiting" }));
    await flush();

    expect(calls[0].signal.aborted).toBe(true);
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(screen.getByRole("status")).toHaveTextContent("Stopped waiting for “A short clip”. The grabber may still finish it");
    expect(detectButton()).toBeEnabled();
  });

  test("fetches the page that was looked up, not what the address field says by now", async () => {
    const { user } = setup();
    grab.mockResolvedValue({ status: "ok", title: "x" });
    await atTheDialog(user);
    fireEvent.change(address(), { target: { value: "https://other.example.com/v/2" } });
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));
    expect(grab).toHaveBeenCalledWith(PAGE, "best", expect.anything());
  });

  test("a page that offers no formats still offers the best file", async () => {
    const { user } = setup();
    detect.mockResolvedValue({ site: "generic", title: "Bare", formats: [] });
    await lookUp(user);
    await flush();
    expect(within(dialog()).getAllByRole("radio")).toHaveLength(1);
    expect(within(dialog()).getByRole("radio", { name: /Best single file/ })).toBeChecked();
  });
});

describe("the dialog", () => {
  async function open(user) {
    detect.mockResolvedValue(META);
    await lookUp(user);
    await flush();
  }

  test("is closed by Cancel and by Escape, and nothing is fetched", async () => {
    const { user } = setup();
    await open(user);
    await user.click(within(dialog()).getByRole("button", { name: "Cancel" }));
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();

    await lookUp(user);
    await flush();
    await user.keyboard("{Escape}");
    expect(screen.queryByRole("dialog")).not.toBeInTheDocument();
    expect(grab).not.toHaveBeenCalled();
  });

  test("is not closed by Escape while the grabber is fetching", async () => {
    const { user } = setup();
    openCalls(grab);
    await open(user);
    await user.click(within(dialog()).getByRole("button", { name: "Download" }));
    await user.keyboard("{Escape}");
    expect(dialog()).toBeInTheDocument();
  });

  test("the formats are a radio group that the arrow keys move through", async () => {
    const { user } = setup();
    await open(user);
    expect(within(dialog()).getByRole("radiogroup", { name: "Format" })).toBeInTheDocument();
    expect(within(dialog()).getAllByRole("radio")[0]).toHaveFocus();

    await user.keyboard("{ArrowDown}");
    expect(within(dialog()).getByRole("radio", { name: /1080p/ })).toBeChecked();
    expect(within(dialog()).getByRole("radio", { name: /1080p/ })).toHaveFocus();
  });

  test("tells where the file goes, and that it is not in the list of downloads", async () => {
    const { user } = setup();
    await open(user);
    expect(dialog()).toHaveTextContent("The grabber saves the file in its own download folder (by default ~/Downloads); it does not appear in the list of downloads.");
  });

  test("copes with a page that has no title", async () => {
    const { user } = setup();
    detect.mockResolvedValue({ site: "", title: "", formats: [] });
    await lookUp(user);
    await flush();
    expect(dialog()).toHaveTextContent("Untitled");
  });
});
