import {
  Phase,
  REPEAT_CHOICES,
  SCHEDULABLE_STATES,
  describeRepeat,
  formatWhen,
  parseLocalTime,
  phaseOf,
  sortTasks,
  toLocalInput,
  validateTask,
} from "./tasks";
import { State } from "./downloads";

const task = (over) => ({ id: 1, downloadId: 7, triggerAt: 1000, repeatSecs: 0, enabled: true, ...over });

describe("describeRepeat", () => {
  test.each([
    [0, "Once"],
    [86400, "Every day"],
    [604800, "Every week"],
    [3600, "Every hour"],
    [172800, "Every 2 days"],
    [7200, "Every 2 hours"],
    [1209600, "Every 2 weeks"],
    [90, "Every 90 seconds"],
    [120, "Every 2 minutes"],
  ])("%d seconds is %s", (secs, text) => expect(describeRepeat(secs)).toBe(text));

  test("nothing that is not a positive number repeats", () => {
    for (const secs of [-5, NaN, undefined, null, "x"]) expect(describeRepeat(secs)).toBe("Once");
  });

  test("every choice the form offers reads back as its own label", () => {
    for (const { secs, label } of REPEAT_CHOICES) expect(describeRepeat(secs)).toBe(label);
  });
});

describe("phaseOf", () => {
  test("a task waits until its time, is due from then until the daemon has run it, and is finished after", () => {
    expect(phaseOf(task({ triggerAt: 1001 }), 1000)).toBe(Phase.WAITING);
    expect(phaseOf(task({ triggerAt: 1000 }), 1000)).toBe(Phase.DUE);
    expect(phaseOf(task({ triggerAt: 900 }), 1000)).toBe(Phase.DUE);
    expect(phaseOf(task({ triggerAt: 900, enabled: false }), 1000)).toBe(Phase.FINISHED);
    expect(phaseOf(task({ triggerAt: 5000, enabled: false }), 1000)).toBe(Phase.FINISHED);
  });
});

describe("sortTasks", () => {
  test("the ones that will still run come first, soonest first; then the finished, the latest first", () => {
    const sorted = sortTasks([
      task({ id: 1, triggerAt: 300, enabled: false }),
      task({ id: 2, triggerAt: 900 }),
      task({ id: 3, triggerAt: 100, enabled: false }),
      task({ id: 4, triggerAt: 500 }),
      task({ id: 5, triggerAt: 500 }),
    ]);
    expect(sorted.map((t) => t.id)).toEqual([4, 5, 2, 1, 3]);
  });

  test("does not change what it is given", () => {
    const given = [task({ id: 2, triggerAt: 9 }), task({ id: 1, triggerAt: 1 })];
    sortTasks(given);
    expect(given.map((t) => t.id)).toEqual([2, 1]);
  });
});

describe("what can be scheduled", () => {
  test("a paused or a failed download: resuming one has a meaning", () => {
    expect([...SCHEDULABLE_STATES]).toEqual([State.PAUSED, State.FAILED]);
  });
});

describe("parseLocalTime", () => {
  const local = (...parts) => Math.floor(new Date(...parts).getTime() / 1000);

  test("reads the text of a datetime-local input as the user's own time", () => {
    expect(parseLocalTime("2026-11-06T02:00")).toBe(local(2026, 10, 6, 2, 0));
    expect(parseLocalTime("2026-11-06T02:00:30")).toBe(local(2026, 10, 6, 2, 0, 30));
    expect(parseLocalTime("2028-02-29T23:59")).toBe(local(2028, 1, 29, 23, 59));
  });

  test.each([
    ["nothing", ""],
    ["undefined", undefined],
    ["a date without a time", "2026-11-06"],
    ["a space instead of T", "2026-11-06 02:00"],
    ["30 February", "2026-02-30T02:00"],
    ["29 February of a year that has none", "2027-02-29T02:00"],
    ["month 13", "2026-13-01T02:00"],
    ["hour 24", "2026-11-06T24:00"],
    ["minute 60", "2026-11-06T02:60"],
    ["a year before 100", "0050-01-01T00:00"],
    ["words", "tomorrow"],
  ])("%s is not a time", (_what, text) => expect(parseLocalTime(text)).toBeNull());
});

describe("toLocalInput", () => {
  test("is the text parseLocalTime reads back, to the minute", () => {
    const ms = new Date(2026, 10, 6, 2, 5, 40).getTime();
    expect(toLocalInput(ms)).toBe("2026-11-06T02:05");
    expect(parseLocalTime(toLocalInput(ms))).toBe(Math.floor(new Date(2026, 10, 6, 2, 5).getTime() / 1000));
  });

  test("pads every part", () => {
    expect(toLocalInput(new Date(2026, 0, 2, 3, 4).getTime())).toBe("2026-01-02T03:04");
  });
});

describe("formatWhen", () => {
  test("is the date and time in the user's own format", () => {
    const secs = Math.floor(new Date(2026, 10, 6, 2, 0).getTime() / 1000);
    const text = formatWhen(secs);
    expect(text).toContain("2026");
    expect(text).toMatch(/2:00|02:00/);
  });
});

describe("validateTask", () => {
  const now = new Date(2026, 10, 5, 12, 0, 0).getTime();
  const ok = { downloadId: 7, when: "2026-11-06T02:00", nowMs: now };

  test("a download in the future is fine", () => {
    expect(validateTask(ok)).toBeNull();
  });

  test("a download must be chosen", () => {
    expect(validateTask({ ...ok, downloadId: NaN })).toMatchObject({ field: "download" });
    expect(validateTask({ ...ok, downloadId: undefined })).toMatchObject({ field: "download" });
    expect(validateTask({ ...ok, downloadId: "7" })).toMatchObject({ field: "download" });
  });

  test("a time must be given, and be one", () => {
    expect(validateTask({ ...ok, when: "" })).toEqual({ field: "when", message: "Choose when to start it." });
    expect(validateTask({ ...ok, when: "2026-02-30T02:00" })).toEqual({ field: "when", message: "That is not a valid date and time." });
  });

  test("the time must be after now: the same minute is too late, the next second is not", () => {
    const noon = new Date(2026, 10, 5, 12, 0, 0).getTime();
    expect(validateTask({ ...ok, when: "2026-11-05T12:00", nowMs: noon })).toMatchObject({ message: "Choose a time in the future." });
    expect(validateTask({ ...ok, when: "2026-11-05T12:00", nowMs: noon - 1000 })).toBeNull();
    expect(validateTask({ ...ok, when: "2026-11-05T11:59", nowMs: noon })).toMatchObject({ field: "when" });
  });

  test("the download is checked first", () => {
    expect(validateTask({ downloadId: NaN, when: "", nowMs: now })).toMatchObject({ field: "download" });
  });
});
