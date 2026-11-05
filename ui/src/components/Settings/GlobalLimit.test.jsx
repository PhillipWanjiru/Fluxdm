import React from "react";
import { act, render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import { limitKnown, selectLimitBps } from "../../store/bandwidthSlice";
import GlobalLimit from "./GlobalLimit";
import { getBandwidthLimit, setBandwidthLimit } from "../../api/stats";
import { ApiError } from "../../api/errors";

jest.mock("../../api/stats");

const MB = 1024 * 1024;
const flush = () => act(async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); });

/* The store holds `known` (null: the daemon has not said), and the daemon answers the read made when the form opens
 * with `held` (by default the same). */
async function setup({ known = 2 * MB, held = known } = {}) {
  if (held === "never") getBandwidthLimit.mockReturnValue(new Promise(() => {}));
  else getBandwidthLimit.mockResolvedValue(held);
  const store = makeStore();
  if (known !== null) store.dispatch(limitKnown(known));
  const user = userEvent.setup();
  render(
    <Provider store={store}>
      <GlobalLimit />
    </Provider>
  );
  await flush();
  return { store, user };
}

const field = () => screen.getByLabelText(/GLOBAL BANDWIDTH LIMIT/);
const apply = () => screen.getByRole("button", { name: /^APPLY/ });
const type = async (user, text) => {
  await user.clear(field());
  if (text) await user.type(field(), text);
};

beforeEach(() => {
  getBandwidthLimit.mockReset();
  setBandwidthLimit.mockReset();
});

describe("what is shown", () => {
  test("is the limit the daemon holds, in KB/s, and says it in words", async () => {
    await setup({ known: 2 * MB });
    expect(field()).toHaveValue("2048");
    expect(screen.getByText("Currently 2.0 MB/s, for all downloads together.")).toBeInTheDocument();
    expect(apply()).toBeDisabled();
  });

  test("no limit is 0", async () => {
    await setup({ known: 0 });
    expect(field()).toHaveValue("0");
    expect(screen.getByText("Currently no limit.")).toBeInTheDocument();
  });

  test("a limit that is not a whole count of KB (set by another client) is shown rounded", async () => {
    await setup({ known: 1500000 });
    expect(field()).toHaveValue("1465");
  });

  test("is read from the daemon when it opens, whatever the store thought", async () => {
    await setup({ known: 1 * MB, held: 3 * MB });
    expect(getBandwidthLimit).toHaveBeenCalledTimes(1);
    expect(field()).toHaveValue("3072");
  });

  test("waits for the daemon when it has not said, and then shows what it says", async () => {
    getBandwidthLimit.mockReset();
    let answer;
    getBandwidthLimit.mockReturnValue(new Promise((resolve) => { answer = resolve; }));
    const store = makeStore();
    render(
      <Provider store={store}>
        <GlobalLimit />
      </Provider>
    );
    await flush();
    expect(field()).toBeDisabled();
    expect(field()).toHaveValue("");
    expect(apply()).toBeDisabled();
    expect(screen.getByText("Waiting for the daemon…")).toBeInTheDocument();

    await act(async () => answer(512 * 1024));
    expect(field()).toBeEnabled();
    expect(field()).toHaveValue("512");
  });

  test("stays waiting when the daemon cannot be asked", async () => {
    getBandwidthLimit.mockReset();
    getBandwidthLimit.mockRejectedValue(new ApiError("Cannot reach the daemon.", { kind: "unreachable" }));
    render(
      <Provider store={makeStore()}>
        <GlobalLimit />
      </Provider>
    );
    await flush();
    expect(field()).toBeDisabled();
    expect(screen.getByText("Waiting for the daemon…")).toBeInTheDocument();
  });
});

describe("changing it", () => {
  test("nothing is sent while typing, and Apply sends bytes per second", async () => {
    const { user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockResolvedValue(1 * MB);

    await type(user, "1024");
    expect(setBandwidthLimit).not.toHaveBeenCalled();
    expect(apply()).toBeEnabled();

    await user.click(apply());
    await flush();

    expect(setBandwidthLimit).toHaveBeenCalledTimes(1);
    expect(setBandwidthLimit).toHaveBeenCalledWith(1 * MB);
  });

  test("afterwards shows what the daemon answered, says it was applied, and Apply rests", async () => {
    const { store, user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockResolvedValue(1 * MB);
    await type(user, "1024");
    await user.click(apply());
    await flush();

    expect(selectLimitBps(store.getState())).toBe(1 * MB);
    expect(field()).toHaveValue("1024");
    expect(screen.getByText("Currently 1.0 MB/s, for all downloads together.")).toBeInTheDocument();
    expect(screen.getByRole("status")).toHaveTextContent("Limit applied.");
    expect(apply()).toBeDisabled();
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });

  test("shows the daemon's number, not the one that was asked for, if they differ", async () => {
    const { user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockResolvedValue(1000 * 1024);
    await type(user, "1024");
    await user.click(apply());
    await flush();
    expect(field()).toHaveValue("1000");
  });

  test("Enter in the field applies it", async () => {
    const { user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockResolvedValue(0);
    await type(user, "0");
    await user.type(field(), "{Enter}");
    await flush();
    expect(setBandwidthLimit).toHaveBeenCalledWith(0);
    expect(screen.getByText("Currently no limit.")).toBeInTheDocument();
  });

  test("the same limit again is not a change", async () => {
    const { user } = await setup({ known: 2 * MB });
    await type(user, "2048");
    expect(apply()).toBeDisabled();
    await user.type(field(), "{Enter}");
    expect(setBandwidthLimit).not.toHaveBeenCalled();
  });

  test("the text goes while the change is under way, and a second try is not made", async () => {
    const { user } = await setup({ known: 2 * MB });
    let finish;
    setBandwidthLimit.mockReturnValue(new Promise((resolve) => { finish = resolve; }));
    await type(user, "1024");
    await user.click(apply());

    expect(screen.getByRole("button", { name: "APPLYING…" })).toBeDisabled();
    expect(field()).toBeDisabled();
    await act(async () => finish(1 * MB));
    expect(setBandwidthLimit).toHaveBeenCalledTimes(1);
    expect(field()).toBeEnabled();
  });

  test("editing again takes the confirmation away", async () => {
    const { user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockResolvedValue(1 * MB);
    await type(user, "1024");
    await user.click(apply());
    await flush();
    expect(screen.getByRole("status")).toHaveTextContent("Limit applied.");

    await user.type(field(), "5");
    expect(screen.getByRole("status")).toBeEmptyDOMElement();
  });
});

describe("what cannot be sent", () => {
  test.each([
    ["", "Enter a number of KB/s, or 0 for no limit."],
    ["abc", "Use a whole number of KB/s."],
    ["-1", "Use a whole number of KB/s."],
    ["1.5", "Use a whole number of KB/s."],
    ["99999999999", "The limit cannot be more than 10000000 KB/s."],
  ])("%j is marked, with the reason, and cannot be applied", async (text, reason) => {
    const { user } = await setup({ known: 2 * MB });
    await type(user, text);

    expect(field()).toHaveAttribute("aria-invalid", "true");
    expect(screen.getByText(reason)).toBeInTheDocument();
    expect(apply()).toBeDisabled();
    await user.type(field(), "{Enter}");
    expect(setBandwidthLimit).not.toHaveBeenCalled();
  });

  test("the mark goes when the text is mended", async () => {
    const { user } = await setup({ known: 2 * MB });
    await type(user, "x");
    expect(field()).toHaveAttribute("aria-invalid", "true");
    await type(user, "100");
    expect(field()).toHaveAttribute("aria-invalid", "false");
    expect(apply()).toBeEnabled();
  });
});

describe("when the daemon refuses", () => {
  test("says why, keeps what was typed, and shows what the daemon holds after all", async () => {
    const { store, user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockRejectedValue(new ApiError("The download core did not answer.", { kind: "unavailable", status: 503 }));
    getBandwidthLimit.mockResolvedValue(1 * MB); /* it stored the limit before it told the core */

    await type(user, "1024");
    await user.click(apply());
    await flush();

    expect(screen.getByRole("alert")).toHaveTextContent("The download core did not answer.");
    expect(field()).toHaveValue("1024");
    expect(selectLimitBps(store.getState())).toBe(1 * MB);
    expect(screen.getByText("Currently 1.0 MB/s, for all downloads together.")).toBeInTheDocument();
    expect(screen.getByRole("status")).toBeEmptyDOMElement(); /* not "applied" */
    expect(field()).toBeEnabled();
  });

  test("the message goes when the text is edited", async () => {
    const { user } = await setup({ known: 2 * MB });
    setBandwidthLimit.mockRejectedValue(new ApiError("No.", { kind: "bad_request", status: 400 }));
    await type(user, "1024");
    await user.click(apply());
    await flush();
    expect(screen.getByRole("alert")).toBeInTheDocument();

    await user.type(field(), "5");
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });
});
