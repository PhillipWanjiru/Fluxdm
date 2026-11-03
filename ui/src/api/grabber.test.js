import { client } from "./client";
import { detect, grab } from "./grabber";
import { installFakeDaemon } from "../testing/fakeDaemon";

const PAGE = "https://www.example.com/watch?v=abc&list=xyz";

describe("api/grabber", () => {
  test("detect sends the page as a query parameter, which is what the daemon reads", async () => {
    const daemon = installFakeDaemon(client, {
      "POST /grabber/detect": () => ({ data: { site: "generic", title: "A clip", duration: 61, formats: [{ id: 18, ext: "mp4", height: 360, abr: null }] } }),
    });
    const meta = await detect(PAGE);
    expect(daemon.calls[0]).toMatchObject({ method: "POST", path: "/grabber/detect", query: { url: PAGE }, body: null });
    expect(meta).toMatchObject({ title: "A clip", formats: [{ id: "18", ext: "mp4", height: 360 }] });
  });

  test("detect allows for a slow page", async () => {
    const daemon = installFakeDaemon(client, { "POST /grabber/detect": () => ({ data: { formats: [] } }) });
    await detect(PAGE);
    expect(daemon.calls[0].timeout).toBeGreaterThan(30_000);
  });

  test("a failure that comes back as a normal answer is still a failure", async () => {
    installFakeDaemon(client, { "POST /grabber/detect": () => ({ data: { error: "ERROR: Unsupported URL" } }) });
    await expect(detect(PAGE)).rejects.toMatchObject({ name: "ApiError", kind: "grabber", message: "ERROR: Unsupported URL" });
  });

  test("a page without formats gives an empty list", async () => {
    installFakeDaemon(client, { "POST /grabber/detect": () => ({ data: { site: "generic", title: "x" } }) });
    expect((await detect(PAGE)).formats).toEqual([]);
  });

  test("grab sends formatId as a query parameter, waits as long as it takes and can be abandoned", async () => {
    const daemon = installFakeDaemon(client, { "POST /grabber/grab": () => ({ data: { status: "ok", title: "A clip", url: PAGE } }) });
    const controller = new AbortController();
    const result = await grab(PAGE, "18", { signal: controller.signal });
    expect(result).toMatchObject({ status: "ok", title: "A clip" });
    expect(daemon.calls[0]).toMatchObject({ path: "/grabber/grab", query: { url: PAGE, formatId: "18" }, timeout: 0 });
    expect(daemon.calls[0].signal).toBe(controller.signal);
  });

  test("grab asks for the best format unless told otherwise", async () => {
    const daemon = installFakeDaemon(client, { "POST /grabber/grab": () => ({ data: { status: "ok" } }) });
    await grab(PAGE);
    expect(daemon.calls[0].query.formatId).toBe("best");
  });

  test("an answer that is not an object is refused", async () => {
    installFakeDaemon(client, { "POST /grabber/grab": () => ({ data: "" }) });
    await expect(grab(PAGE)).rejects.toMatchObject({ kind: "protocol" });
  });
});
