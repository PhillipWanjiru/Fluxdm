import { BYTES_PER_KB, MAX_LIMIT_KB, limitToKb, parseLimit } from "./bandwidth";

describe("parseLimit", () => {
  test("turns KB/s into the bytes per second the daemon takes", () => {
    expect(parseLimit("0")).toEqual({ bps: 0 });
    expect(parseLimit("1")).toEqual({ bps: 1024 });
    expect(parseLimit("2048")).toEqual({ bps: 2097152 });
    expect(parseLimit("  512  ")).toEqual({ bps: 524288 });
    expect(parseLimit("007")).toEqual({ bps: 7168 });
    expect(parseLimit(String(MAX_LIMIT_KB))).toEqual({ bps: MAX_LIMIT_KB * BYTES_PER_KB });
  });

  test.each([
    ["", "Enter a number"],
    ["   ", "Enter a number"],
    [undefined, "Enter a number"],
    ["-5", "whole number"],
    ["1.5", "whole number"],
    ["1e3", "whole number"],
    ["12 34", "whole number"],
    ["abc", "whole number"],
    ["+7", "whole number"],
    [String(MAX_LIMIT_KB + 1), "cannot be more than"],
    ["99999999999999999999", "cannot be more than"],
  ])("%j is refused", (text, words) => {
    const result = parseLimit(text);
    expect(result.bps).toBeUndefined();
    expect(result.error).toContain(words);
  });

  test("what it accepts, the daemon accepts: a whole, non-negative number of bytes within a long", () => {
    const { bps } = parseLimit(String(MAX_LIMIT_KB));
    expect(Number.isSafeInteger(bps)).toBe(true);
    expect(bps).toBeLessThan(2 ** 63);
  });
});

describe("limitToKb", () => {
  test("is the inverse of parseLimit for what the field itself sets", () => {
    for (const kb of [0, 1, 512, 2048, 100000]) expect(limitToKb(parseLimit(String(kb)).bps)).toBe(kb);
  });

  test("rounds what another client set", () => {
    expect(limitToKb(1500000)).toBe(1465);
    expect(limitToKb(1536)).toBe(2);
    expect(limitToKb(100)).toBe(0);
  });
});
