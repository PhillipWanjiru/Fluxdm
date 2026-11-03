import { client } from "./client";
import { cancel, getAll, getOne, pause, resume, setLimit, start } from "./downloads";
import { installFakeDaemon } from "../testing/fakeDaemon";
import { daemonDownload } from "../testing/fixtures";
import { State } from "../model/downloads";

describe("api/downloads", () => {
  test("getAll reads the list as models and leaves out entries without an id", async () => {
    installFakeDaemon(client, {
      "GET /downloads": () => ({ data: [daemonDownload({ id: 3, status: 3 }), { url: "no id" }, null, daemonDownload({ id: 4, status: 0 })] }),
    });
    const list = await getAll();
    expect(list.map((d) => [d.id, d.state])).toEqual([[3, State.COMPLETE], [4, State.QUEUED]]);
  });

  test("getAll refuses an answer that is not a list", async () => {
    installFakeDaemon(client, { "GET /downloads": () => ({ data: { error: "not a list" } }) });
    await expect(getAll()).rejects.toMatchObject({ name: "ApiError", kind: "protocol" });
  });

  test("getOne asks for /downloads/{id}, the endpoint the daemon has", async () => {
    const daemon = installFakeDaemon(client, { "GET /downloads/:id": ({ params }) => ({ data: daemonDownload({ id: Number(params.id) }) }) });
    expect((await getOne(7)).id).toBe(7);
    expect(daemon.calls.map((c) => `${c.method} ${c.path}`)).toEqual(["GET /downloads/7"]);
  });

  test("start sends the fields the daemon reads and nothing else", async () => {
    const daemon = installFakeDaemon(client, { "POST /downloads": ({ body }) => ({ data: daemonDownload({ ...body, id: 9, status: 0 }) }) });
    const created = await start({
      url: "https://example.com/a.zip", filePath: "~/Downloads/a.zip", segments: 4,
      id: 1, status: 3, downloaded: 99, errorMessage: "forged",
    });
    expect(daemon.calls[0].body).toEqual({ url: "https://example.com/a.zip", filePath: "~/Downloads/a.zip", segments: 4 });
    expect(created).toMatchObject({ id: 9, state: State.QUEUED, name: "a.zip" });
  });

  test("start shows the daemon's own words when it refuses", async () => {
    installFakeDaemon(client, { "POST /downloads": () => ({ status: 409, data: { error: "another download is already using /home/u/a.zip" } }) });
    await expect(start({ url: "https://example.com/a.zip", filePath: "/home/u/a.zip" })).rejects.toMatchObject({
      kind: "conflict", status: 409, message: "another download is already using /home/u/a.zip",
    });
  });

  test("pause and resume post to the download and return its new state", async () => {
    const daemon = installFakeDaemon(client, {
      "POST /downloads/:id/pause": ({ params }) => ({ data: daemonDownload({ id: Number(params.id), status: 2 }) }),
      "POST /downloads/:id/resume": ({ params }) => ({ data: daemonDownload({ id: Number(params.id), status: 1 }) }),
    });
    expect((await pause(5)).state).toBe(State.PAUSED);
    expect((await resume(5)).state).toBe(State.DOWNLOADING);
    expect(daemon.calls.map((c) => `${c.method} ${c.path}`)).toEqual(["POST /downloads/5/pause", "POST /downloads/5/resume"]);
  });

  test("cancel deletes the download", async () => {
    const daemon = installFakeDaemon(client, { "DELETE /downloads/:id": () => ({ status: 200 }) });
    await expect(cancel(5)).resolves.toBeUndefined();
    expect(daemon.calls.map((c) => `${c.method} ${c.path}`)).toEqual(["DELETE /downloads/5"]);
  });

  test("setLimit puts bytesPerSec", async () => {
    const daemon = installFakeDaemon(client, {
      "PUT /downloads/:id/limit": ({ params, body }) => ({ data: daemonDownload({ id: Number(params.id), bandwidthLimit: body.bytesPerSec }) }),
    });
    expect((await setLimit(5, 500000)).bandwidthLimit).toBe(500000);
    expect(daemon.calls[0].body).toEqual({ bytesPerSec: 500000 });
  });
});
