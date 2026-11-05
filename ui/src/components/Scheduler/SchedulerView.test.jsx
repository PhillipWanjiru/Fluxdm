import React from "react";
import { act, fireEvent, render, screen, within } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Provider } from "react-redux";
import { makeStore } from "../../store";
import { received } from "../../store/downloadsSlice";
import SchedulerView, { REFRESH_MS } from "./SchedulerView";
import { createTask, deleteTask, getTasks } from "../../api/scheduler";
import { ApiError } from "../../api/errors";
import { fromDaemon } from "../../model/downloads";
import { formatWhen, parseLocalTime } from "../../model/tasks";
import { daemonDownload } from "../../testing/fixtures";

jest.mock("../../api/scheduler");

const NOW = new Date(2026, 10, 5, 12, 0, 0); /* Thursday 5 November 2026, noon, local time */
const secs = (offset = 0) => Math.floor(NOW.getTime() / 1000) + offset;

const dl = (id, status, name, over = {}) =>
  fromDaemon(daemonDownload({ id, status, speedBps: 0, filePath: `/d/${name}`, url: `https://example.com/${name}`, ...over }));
const task = (over) => ({ id: 1, downloadId: 2, triggerAt: secs(3600), repeatSecs: 0, enabled: true, ...over });

/* one download in each state */
const downloads = () => [
  dl(1, 1, "running.iso"),
  dl(2, 2, "paused.iso"),
  dl(3, 0, "queued.iso"),
  dl(4, 3, "done.iso", { downloaded: 1000 }),
  dl(5, 4, "failed.iso", { errorMessage: "connection reset by peer" }),
];

/* The clock stands still at NOW, so what the cards say about "in 1m 00s" is exact. Timers are real, which is what
 * user-event is built for; the few tests about the ten-second refresh switch to fake ones (withFakeTimers). */
let clock;
beforeEach(() => {
  clock = jest.spyOn(Date, "now").mockReturnValue(NOW.getTime());
  getTasks.mockReset();
  createTask.mockReset();
  deleteTask.mockReset();
});
afterEach(() => {
  jest.useRealTimers();
  clock.mockRestore();
});

function withFakeTimers() {
  jest.useFakeTimers("modern");
  jest.setSystemTime(NOW);
}

/* Renders the view over a store holding `list`; the first read of the schedule answers `tasks`, or fails with `error`. */
async function setup({ list = downloads(), tasks = [], error = null } = {}) {
  if (error) getTasks.mockRejectedValue(error);
  else getTasks.mockResolvedValue(tasks);
  const store = makeStore();
  store.dispatch(received(list));
  const user = userEvent.setup();
  const view = render(
    <Provider store={store}>
      <SchedulerView />
    </Provider>
  );
  await flush();
  return { store, user, ...view };
}

/* Lets every promise that is waiting run. */
const flush = () => act(async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); });
const tick = (ms) => act(async () => { jest.advanceTimersByTime(ms); for (let i = 0; i < 20; i++) await Promise.resolve(); });

const cards = () => within(screen.getByRole("list", { name: "Scheduled tasks" })).getAllByRole("listitem");
const whenField = () => screen.getByLabelText("START AT");
const pick = async (user, name) => user.selectOptions(screen.getByLabelText("DOWNLOAD"), name);
const submit = (user) => user.click(screen.getByRole("button", { name: "SCHEDULE" }));

describe("the list of tasks", () => {
  test("says there is nothing when there is nothing", async () => {
    await setup({ tasks: [] });
    expect(screen.getByText("No scheduled tasks.")).toBeInTheDocument();
  });

  test("says it is loading until the daemon has answered", () => {
    getTasks.mockReturnValue(new Promise(() => {}));
    render(
      <Provider store={makeStore()}>
        <SchedulerView />
      </Provider>
    );
    expect(screen.getByText("Loading…")).toBeInTheDocument();
    expect(screen.queryByText("No scheduled tasks.")).not.toBeInTheDocument();
  });

  test("shows each task with its download, when and how often: the soonest first, the finished last", async () => {
    await setup({
      tasks: [
        task({ id: 1, downloadId: 2, triggerAt: secs(3600), repeatSecs: 86400 }),
        task({ id: 2, downloadId: 5, triggerAt: secs(60) }),
        task({ id: 3, downloadId: 2, triggerAt: secs(-100), enabled: false }),
        task({ id: 4, downloadId: 99, triggerAt: secs(7200) }),
      ],
    });

    expect(cards().map((c) => c.dataset.testid)).toEqual(["task-2", "task-1", "task-4", "task-3"]);

    const [soonest, daily, gone, finished] = cards();
    expect(soonest).toHaveTextContent("failed.iso");
    expect(soonest).toHaveTextContent("/d/failed.iso");
    expect(soonest).toHaveTextContent("WAITING");
    expect(soonest).toHaveTextContent(`Starts ${formatWhen(secs(60))} (in 1m 00s)`);
    expect(soonest).not.toHaveTextContent("Every");

    expect(daily).toHaveTextContent("paused.iso");
    expect(daily).toHaveTextContent("in 1h 00m");
    expect(daily).toHaveTextContent("Every day");

    expect(finished).toHaveTextContent("FINISHED");
    expect(finished).toHaveTextContent(`Ran ${formatWhen(secs(-100))}`);
    expect(within(finished).queryByText(/in \d/)).not.toBeInTheDocument();

    expect(gone).toHaveTextContent("Download #99");
    expect(gone).toHaveTextContent("No longer in the list of downloads");
  });

  test("a task whose time has come is due until the daemon has run it", async () => {
    await setup({ tasks: [task({ triggerAt: secs(-5) })] });
    expect(cards()[0]).toHaveTextContent("DUE");
    expect(cards()[0]).toHaveTextContent("starting now");
  });

  test("is read again every ten seconds, which is how a task that has run shows as finished", async () => {
    withFakeTimers();
    await setup({ tasks: [task({ triggerAt: secs(5) })] });
    expect(cards()[0]).toHaveTextContent("WAITING");

    getTasks.mockResolvedValue([task({ triggerAt: secs(5), enabled: false })]);
    await tick(REFRESH_MS);

    expect(getTasks).toHaveBeenCalledTimes(2);
    expect(cards()[0]).toHaveTextContent("FINISHED");
  });

  test("says when the schedule cannot be read, and recovers when it can", async () => {
    withFakeTimers();
    await setup({ error: new ApiError("Cannot reach the FluxDM daemon at http://127.0.0.1:7171.", { kind: "unreachable" }) });
    expect(screen.getByRole("alert")).toHaveTextContent("Could not load the schedule: Cannot reach the FluxDM daemon");
    expect(screen.queryByText("No scheduled tasks.")).not.toBeInTheDocument();
    expect(screen.queryByText("Loading…")).not.toBeInTheDocument();

    getTasks.mockResolvedValue([task()]);
    await tick(REFRESH_MS);

    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(cards()).toHaveLength(1);
  });

  test("keeps what it showed when a later read fails, and says so", async () => {
    withFakeTimers();
    await setup({ tasks: [task()] });
    getTasks.mockRejectedValue(new ApiError("The daemon did not answer in time.", { kind: "timeout" }));
    await tick(REFRESH_MS);

    expect(screen.getByRole("alert")).toHaveTextContent("Could not refresh the schedule: The daemon did not answer in time.");
    expect(cards()).toHaveLength(1);
  });

  test("stops reading when it is closed", async () => {
    withFakeTimers();
    const { unmount } = await setup({ tasks: [] });
    unmount();
    await tick(REFRESH_MS * 3);
    expect(getTasks).toHaveBeenCalledTimes(1);
  });
});

describe("the form", () => {
  test("offers the downloads that can be resumed: the paused and the failed", async () => {
    await setup();
    const select = screen.getByLabelText("DOWNLOAD");
    const names = within(select).getAllByRole("option").map((o) => o.textContent);
    expect(names).toEqual(["Choose a download…", "failed.iso (failed)", "paused.iso (paused)"]); /* newest first, as the list */
  });

  test("says what to do when there is nothing to offer", async () => {
    await setup({ list: [dl(1, 1, "running.iso"), dl(4, 3, "done.iso")] });
    const select = screen.getByLabelText("DOWNLOAD");
    expect(select).toBeDisabled();
    expect(within(select).getByRole("option", { name: "Nothing to schedule" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "SCHEDULE" })).toBeDisabled();
    expect(screen.getByText(/Pause a download/)).toBeInTheDocument();
  });

  test("makes a task, shows it, and clears itself", async () => {
    const { user } = await setup();
    createTask.mockImplementation(async (t) => ({ ...t, id: 11, enabled: true }));

    await pick(user, "paused.iso (paused)");
    fireEvent.change(whenField(), { target: { value: "2026-11-06T02:00" } });
    await user.selectOptions(screen.getByLabelText("REPEAT"), "Every day");
    await submit(user);
    await flush();

    expect(createTask).toHaveBeenCalledTimes(1);
    expect(createTask).toHaveBeenCalledWith({ downloadId: 2, triggerAt: parseLocalTime("2026-11-06T02:00"), repeatSecs: 86400 });
    expect(cards()).toHaveLength(1);
    expect(cards()[0]).toHaveTextContent("paused.iso");
    expect(cards()[0]).toHaveTextContent("Every day");
    expect(screen.getByLabelText("DOWNLOAD")).toHaveValue("");
    expect(whenField()).toHaveValue("");
    expect(screen.getByLabelText("REPEAT")).toHaveValue("0");
  });

  test("a task that runs once is made with no repeat", async () => {
    const { user } = await setup();
    createTask.mockImplementation(async (t) => ({ ...t, id: 11, enabled: true }));
    await pick(user, "failed.iso (failed)");
    fireEvent.change(whenField(), { target: { value: "2026-11-05T23:30" } });
    await submit(user);
    await flush();
    expect(createTask).toHaveBeenCalledWith({ downloadId: 5, triggerAt: parseLocalTime("2026-11-05T23:30"), repeatSecs: 0 });
  });

  test("the picker does not offer a time that has passed", async () => {
    await setup();
    expect(whenField()).toHaveAttribute("min", "2026-11-05T12:00");
  });

  test("says what is wrong beside the field, and asks the daemon nothing", async () => {
    const { user } = await setup();

    await submit(user);
    expect(screen.getByLabelText("DOWNLOAD")).toHaveAttribute("aria-invalid", "true");
    expect(screen.getByText("Choose a download.")).toBeInTheDocument();

    await pick(user, "paused.iso (paused)");
    expect(screen.getByLabelText("DOWNLOAD")).toHaveAttribute("aria-invalid", "false");
    expect(screen.getByText("Choose when to start it.")).toBeInTheDocument();
    expect(whenField()).toHaveAttribute("aria-invalid", "true");

    fireEvent.change(whenField(), { target: { value: "2026-11-05T11:00" } });
    expect(screen.getByText("Choose a time in the future.")).toBeInTheDocument();

    fireEvent.change(whenField(), { target: { value: "2026-11-05T13:00" } });
    expect(whenField()).toHaveAttribute("aria-invalid", "false");
    expect(createTask).not.toHaveBeenCalled();
  });

  test("a time that was in the future when typed but is not any more is not sent", async () => {
    const { user } = await setup();
    await pick(user, "paused.iso (paused)");
    fireEvent.change(whenField(), { target: { value: "2026-11-05T12:30" } });
    clock.mockReturnValue(new Date(2026, 10, 5, 12, 31, 0).getTime()); /* the user took their time */
    await submit(user);
    expect(createTask).not.toHaveBeenCalled();
    expect(screen.getByText("Choose a time in the future.")).toBeInTheDocument();
  });

  test("shows what the daemon refuses, and keeps what was typed", async () => {
    const { user } = await setup();
    createTask.mockRejectedValue(new ApiError("The daemon is not ready (is fluxdm-core running?).", { kind: "unavailable", status: 503 }));
    await pick(user, "paused.iso (paused)");
    fireEvent.change(whenField(), { target: { value: "2026-11-06T02:00" } });
    await submit(user);
    await flush();

    expect(screen.getByRole("alert")).toHaveTextContent("The daemon is not ready");
    expect(screen.getByLabelText("DOWNLOAD")).toHaveValue("2");
    expect(whenField()).toHaveValue("2026-11-06T02:00");
    expect(screen.getByRole("button", { name: "SCHEDULE" })).toBeEnabled();
    expect(screen.queryByRole("list", { name: "Scheduled tasks" })).not.toBeInTheDocument();
  });

  test("a second click while it is being made does not make it twice", async () => {
    const { user } = await setup();
    let finish;
    createTask.mockReturnValue(new Promise((resolve) => { finish = resolve; }));
    await pick(user, "paused.iso (paused)");
    fireEvent.change(whenField(), { target: { value: "2026-11-06T02:00" } });
    await submit(user);
    expect(screen.getByRole("button", { name: "SCHEDULING…" })).toBeDisabled();
    fireEvent.submit(screen.getByRole("form", { name: "NEW SCHEDULE" }));
    expect(createTask).toHaveBeenCalledTimes(1);

    await act(async () => finish({ id: 1, downloadId: 2, triggerAt: secs(3600), repeatSecs: 0, enabled: true }));
    expect(cards()).toHaveLength(1);
  });

  test("a read that was under way does not make the new task disappear", async () => {
    let answer;
    getTasks.mockReturnValue(new Promise((resolve) => { answer = resolve; }));
    const store = makeStore();
    store.dispatch(received(downloads()));
    const user = userEvent.setup();
    render(
      <Provider store={store}>
        <SchedulerView />
      </Provider>
    );
    createTask.mockImplementation(async (t) => ({ ...t, id: 11, enabled: true }));

    await pick(user, "paused.iso (paused)");
    fireEvent.change(whenField(), { target: { value: "2026-11-06T02:00" } });
    await submit(user);
    await flush();
    expect(cards()).toHaveLength(1);

    await act(async () => answer([])); /* the first read, which began before the task existed, answers late */
    expect(cards()).toHaveLength(1);
  });
});

describe("deleting a task", () => {
  test("asks first, and does nothing if the answer is no", async () => {
    const { user } = await setup({ tasks: [task({ id: 1, downloadId: 2 })] });
    await user.click(screen.getByRole("button", { name: "Delete schedule for paused.iso" }));

    const dialog = screen.getByRole("alertdialog", { name: "Delete this schedule?" });
    expect(dialog).toHaveTextContent("paused.iso will no longer be started by it. The download itself is not touched.");
    await user.click(within(dialog).getByRole("button", { name: "Cancel" }));

    expect(screen.queryByRole("alertdialog")).not.toBeInTheDocument();
    expect(deleteTask).not.toHaveBeenCalled();
    expect(cards()).toHaveLength(1);
  });

  test("deletes it when confirmed, and the card goes", async () => {
    const { user } = await setup({ tasks: [task({ id: 1, downloadId: 2 }), task({ id: 2, downloadId: 5, triggerAt: secs(7200) })] });
    deleteTask.mockResolvedValue(undefined);
    await user.click(screen.getByRole("button", { name: "Delete schedule for paused.iso" }));
    await user.click(within(screen.getByRole("alertdialog")).getByRole("button", { name: "Delete" }));
    await flush();

    expect(deleteTask).toHaveBeenCalledWith(1);
    expect(cards().map((c) => c.dataset.testid)).toEqual(["task-2"]);
    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
  });

  test("says why it could not, and keeps the card", async () => {
    const { user } = await setup({ tasks: [task()] });
    deleteTask.mockRejectedValue(new ApiError("The daemon did not answer in time.", { kind: "timeout" }));
    await user.click(screen.getByRole("button", { name: "Delete schedule for paused.iso" }));
    await user.click(within(screen.getByRole("alertdialog")).getByRole("button", { name: "Delete" }));
    await flush();

    expect(screen.getByRole("alert")).toHaveTextContent("The daemon did not answer in time.");
    expect(cards()).toHaveLength(1);
    expect(screen.getByRole("button", { name: "Delete schedule for paused.iso" })).toBeEnabled();
  });

  test("a task that is already gone is simply gone", async () => {
    const { user } = await setup({ tasks: [task()] });
    deleteTask.mockRejectedValue(new ApiError("The daemon does not know that.", { kind: "not_found", status: 404 }));
    await user.click(screen.getByRole("button", { name: "Delete schedule for paused.iso" }));
    await user.click(within(screen.getByRole("alertdialog")).getByRole("button", { name: "Delete" }));
    await flush();

    expect(screen.queryByRole("alert")).not.toBeInTheDocument();
    expect(screen.getByText("No scheduled tasks.")).toBeInTheDocument();
  });

  test("a task of a download that was removed is named by its number in the question", async () => {
    const { user } = await setup({ tasks: [task({ downloadId: 99 })] });
    await user.click(screen.getByRole("button", { name: "Delete schedule for Download #99" }));
    expect(screen.getByRole("alertdialog")).toHaveTextContent("Download #99 will no longer be started by it.");
  });
});
