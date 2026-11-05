import { formatBytes, formatDuration, formatEta, formatSpeed } from "./format";

describe("formatBytes", () => {
  test.each([
    [0, "0 B"],
    [1023, "1023 B"],
    [1024, "1.0 KB"],
    [1536, "1.5 KB"],
    [10 * 1024, "10.0 KB"],
    [150 * 1024, "150 KB"],
    [5 * 1024 * 1024, "5.0 MB"],
    [1.5 * 1024 ** 3, "1.5 GB"],
    [3 * 1024 ** 4, "3.0 TB"],
    [2048 * 1024 ** 4, "2048 TB"], // there is no bigger unit
  ])("%d bytes is %s", (bytes, text) => {
    expect(formatBytes(bytes)).toBe(text);
  });

  test("steps up instead of showing 1024 of a unit", () => {
    expect(formatBytes(1024 * 1024 - 1)).toBe("1.0 MB");
    expect(formatBytes(1024 ** 3 - 100)).toBe("1.0 GB");
  });

  test("a value that is not a size is a dash", () => {
    for (const v of [-1, NaN, undefined, "abc", Infinity]) expect(formatBytes(v)).toBe("—");
  });

  test("speeds carry a per-second suffix", () => {
    expect(formatSpeed(2 * 1024 * 1024)).toBe("2.0 MB/s");
    expect(formatSpeed(0)).toBe("0 B/s");
  });
});

describe("formatEta", () => {
  test.each([
    [0, "0s"],
    [45, "45s"],
    [59.6, "1m 00s"],
    [185, "3m 05s"],
    [3600, "1h 00m"],
    [7620, "2h 07m"],
    [90000, "1d 01h"],
  ])("%d seconds is %s", (seconds, text) => {
    expect(formatEta(seconds)).toBe(text);
  });

  test("unknown is a dash", () => {
    for (const v of [null, undefined, -1, NaN, "soon"]) expect(formatEta(v)).toBe("—");
  });
});

describe("formatDuration", () => {
  test.each([
    [0, "0:00"],
    [9, "0:09"],
    [185, "3:05"],
    [3599, "59:59"],
    [3600, "1:00:00"],
    [7620, "2:07:00"],
    [36000, "10:00:00"],
    [61.4, "1:01"],
  ])("%d seconds is %s", (seconds, text) => {
    expect(formatDuration(seconds)).toBe(text);
  });

  test("unknown is a dash", () => {
    for (const v of [null, undefined, -1, NaN, "long"]) expect(formatDuration(v)).toBe("—");
  });
});
