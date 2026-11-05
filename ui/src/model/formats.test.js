import { BEST, describeFormat, formatChoices } from "./formats";

describe("describeFormat", () => {
  test.each([
    [{ id: "137", ext: "mp4", height: 1080, abr: null }, "1080p · MP4"],
    [{ id: "18", ext: "mp4", height: 360, abr: 96.0 }, "360p · MP4 · 96 kbps"],
    [{ id: "140", ext: "m4a", height: null, abr: 129.49 }, "audio only · M4A · 129 kbps"],
    [{ id: "251", ext: "webm", abr: 130.9 }, "audio only · WEBM · 131 kbps"],
    [{ id: "x", ext: "mp4" }, "MP4"],
    [{ id: "x" }, "unknown format"],
  ])("%j is %s", (format, text) => expect(describeFormat(format)).toBe(text));
});

describe("formatChoices", () => {
  const formats = [
    { id: "140", ext: "m4a", height: null, abr: 129 },
    { id: "18", ext: "mp4", height: 360, abr: 96 },
    { id: "137", ext: "mp4", height: 1080, abr: null },
    { id: "sb0", ext: "mhtml", height: 45, abr: null },
    { id: "251", ext: "webm", height: null, abr: 131 },
    { id: "22", ext: "mp4", height: 720, abr: 192 },
  ];

  test("starts with the best single file", () => {
    expect(formatChoices(formats)[0]).toEqual({ id: BEST, label: "Best single file", detail: "video and audio together" });
    expect(formatChoices([])).toHaveLength(1);
  });

  test("lists the page's own formats after it, the tallest first, the audio-only last", () => {
    expect(formatChoices(formats).map((c) => c.id)).toEqual(["best", "137", "22", "18", "251", "140"]);
  });

  test("labels each, and keeps its id to tell equal labels apart", () => {
    const choices = formatChoices(formats);
    expect(choices.find((c) => c.id === "22")).toEqual({ id: "22", label: "720p · MP4 · 192 kbps", detail: "id 22" });
    expect(choices.find((c) => c.id === "140")).toEqual({ id: "140", label: "audio only · M4A · 129 kbps", detail: "id 140" });
  });

  test("leaves out the preview images, entries with no id, and repeats", () => {
    const choices = formatChoices([
      { id: "sb1", ext: "mhtml", height: 90 },
      { id: "", ext: "mp4", height: 480 },
      { ext: "mp4", height: 480 },
      null,
      { id: "18", ext: "mp4", height: 360 },
      { id: "18", ext: "mp4", height: 360 },
      { id: "best", ext: "mp4", height: 2000 },
    ]);
    expect(choices.map((c) => c.id)).toEqual(["best", "18"]);
  });

  test("a numeric id is read as text, which is what the daemon is sent", () => {
    expect(formatChoices([{ id: 18, ext: "mp4", height: 360 }])[1].id).toBe("18");
  });

  test("a list that is not a list gives just the best", () => {
    for (const v of [undefined, null, "x", {}]) expect(formatChoices(v).map((c) => c.id)).toEqual(["best"]);
  });

  test("does not change what it is given", () => {
    const given = [{ id: "140", abr: 1 }, { id: "137", height: 1080 }];
    formatChoices(given);
    expect(given.map((f) => f.id)).toEqual(["140", "137"]);
  });
});
