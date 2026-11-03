import {
  cleanFileName,
  fileNameFromUrl,
  hasControlChars,
  isAbsoluteDirectory,
  joinPath,
  normalizePath,
  utf8Length,
  validateDownload,
} from "./destination";

const ok = { url: "https://example.com/a.iso", directory: "~/Downloads", fileName: "a.iso" };
const problem = (over) => validateDownload({ ...ok, ...over });

describe("fileNameFromUrl", () => {
  test.each([
    ["https://example.com/pub/ubuntu-24.04.iso", "ubuntu-24.04.iso"],
    ["https://example.com/dir/My%20File%20%281%29.zip?token=abc#frag", "My File (1).zip"],
    ["  https://example.com/a.bin  ", "a.bin"],
    ["https://example.com/dir/", "dir"],
    ["https://example.com/", ""],
    ["https://example.com", ""],
    ["not a url", ""],
    ["", ""],
    ["https://example.com/bad%E0%A4%A.zip", "bad%E0%A4%A.zip"], // a broken escape is kept as written
    ["https://example.com/a%2Fb.zip", "a_b.zip"], // an escaped slash cannot become a path
    ["https://example.com/%2e%2e", ""], // ".." is not a name
  ])("%s gives %j", (url, name) => {
    expect(fileNameFromUrl(url)).toBe(name);
  });
});

describe("cleanFileName and joinPath", () => {
  test("cleanFileName", () => {
    expect(cleanFileName("a/b")).toBe("a_b");
    expect(cleanFileName("a\u0000b\nc")).toBe("a_b_c");
    expect(cleanFileName("  x.txt ")).toBe("x.txt");
    expect(cleanFileName("..")).toBe("");
  });

  test("joinPath puts exactly one slash between folder and name", () => {
    expect(joinPath("~/Downloads", "a.iso")).toBe("~/Downloads/a.iso");
    expect(joinPath("~/Downloads/", "a.iso")).toBe("~/Downloads/a.iso");
    expect(joinPath("/srv//", "a.iso")).toBe("/srv/a.iso");
    expect(joinPath("~", "a.iso")).toBe("~/a.iso");
    expect(joinPath("  ", "a.iso")).toBe("");
  });

  test("utf8Length counts bytes, not characters", () => {
    expect(utf8Length("abc")).toBe(3);
    expect(utf8Length("é")).toBe(2);
    expect(utf8Length("€")).toBe(3);
    expect(utf8Length("😀")).toBe(4);
  });

  test("hasControlChars is the daemon's test: below U+0020, or DEL", () => {
    expect(hasControlChars("plain text é€😀")).toBe(false);
    expect(hasControlChars("tab\there")).toBe(true);
    expect(hasControlChars("nul\u0000")).toBe(true);
    expect(hasControlChars("del\u007f")).toBe(true);
    expect(hasControlChars("\u001f")).toBe(true);
    expect(hasControlChars(" ")).toBe(false); // U+0020 is a space, not a control
    expect(hasControlChars("\u0080\u009f")).toBe(false); // the daemon does not count C1 controls either
  });

  test("normalizePath does what Path.normalize does to an absolute path", () => {
    expect(normalizePath("/a/b/c")).toBe("/a/b/c");
    expect(normalizePath("/a//b/./c/")).toBe("/a/b/c");
    expect(normalizePath("/a/b/../c")).toBe("/a/c");
    expect(normalizePath("/a/../../b")).toBe("/b"); // ".." at the root stays at the root
    expect(normalizePath("/")).toBe("/");
    expect(normalizePath("~/Downloads/a.iso")).toBe("/~/Downloads/a.iso"); // only its length is used
  });

  test("isAbsoluteDirectory", () => {
    for (const dir of ["/", "/srv", "~", "~/", "~/Downloads", "  /srv  "]) expect(isAbsoluteDirectory(dir)).toBe(true);
    for (const dir of ["", "srv", "./srv", "../srv", "~srv", "~user/x"]) expect(isAbsoluteDirectory(dir)).toBe(false);
  });
});

describe("validateDownload", () => {
  test("accepts what the daemon accepts", () => {
    expect(problem({})).toBeNull();
    expect(problem({ directory: "/srv/files" })).toBeNull();
    expect(problem({ directory: "~" })).toBeNull();
    expect(problem({ url: "HTTP://Example.com/a?b=c&d=e#f" })).toBeNull();
    expect(problem({ fileName: "name with spaces (1).iso" })).toBeNull();
  });

  test("is no stricter than the daemon where JavaScript and Java disagree", () => {
    // Java's \s is ASCII white space only; JavaScript's also matches U+00A0, so the daemon takes this address
    expect(problem({ url: "https://example.com/a b.iso" })).toBeNull();
    // the daemon measures the path after normalizing it, so a long detour that comes back is fine
    const detour = `/${"d".repeat(400)}/../${"e".repeat(400)}/../${"f".repeat(400)}/../srv`;
    expect(detour.length).toBeGreaterThan(1023);
    expect(problem({ directory: detour })).toBeNull();
    expect(problem({ directory: `/${"d".repeat(1000)}/x`, fileName: "x".repeat(100) })).toMatchObject({ field: "directory" });
  });

  test.each([
    ["", "Enter the address"],
    ["   ", "Enter the address"],
    ["ftp://example.com/a", "http:// or https://"],
    ["example.com/a", "http:// or https://"],
    ["https://", "http:// or https://"],
    ["https://exa mple.com/a", "no spaces"],
    ["https://example.com/a\nb", "no spaces"],
  ])("the address %j is refused", (url, words) => {
    const p = problem({ url });
    expect(p.field).toBe("url");
    expect(p.message).toContain(words);
  });

  test("an address over 4095 bytes is refused", () => {
    expect(problem({ url: `https://example.com/${"a".repeat(4100)}` })).toMatchObject({ field: "url", message: expect.stringContaining("too long") });
    expect(problem({ url: `https://example.com/${"é".repeat(2100)}` })).toMatchObject({ field: "url" }); // 4200 bytes in 2100 characters
  });

  test.each([
    ["", "Choose a folder"],
    ["Downloads", "absolute path"],
    ["./here", "absolute path"],
    ["~user/files", "absolute path"],
    ["/srv/\u0007bell", "control characters"],
  ])("the folder %j is refused", (directory, words) => {
    const p = problem({ directory });
    expect(p.field).toBe("directory");
    expect(p.message).toContain(words);
  });

  test.each([
    ["", "Name the file"],
    ["a/b.iso", "cannot contain /"],
    ["a\u0000b", "cannot contain /"],
    ["..", "not a file name"],
    ["é".repeat(200), "too long"],
  ])("the file name %j is refused", (fileName, words) => {
    const p = problem({ fileName });
    expect(p.field).toBe("fileName");
    expect(p.message).toContain(words);
  });

  test("a path over 1023 bytes is refused", () => {
    expect(problem({ directory: `/${"d".repeat(1000)}`, fileName: "x".repeat(100) })).toMatchObject({ field: "directory", message: expect.stringContaining("too long") });
  });

  test("reports the address before the folder, and the folder before the name", () => {
    expect(problem({ url: "", directory: "", fileName: "" }).field).toBe("url");
    expect(problem({ directory: "", fileName: "" }).field).toBe("directory");
  });
});
