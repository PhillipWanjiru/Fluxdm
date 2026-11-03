import { client } from "./client";
import { createTask, deleteTask, getTasks } from "./scheduler";
import { installFakeDaemon } from "../testing/fakeDaemon";
import { taskFromDaemon } from "../model/tasks";

describe("api/scheduler", () => {
  test("getTasks reads the tasks", async () => {
    installFakeDaemon(client, {
      "GET /scheduler": () => ({ data: [{ id: 1, downloadId: 7, triggerAt: 1790000000, repeatSecs: 86400, enabled: true }, { id: 2, downloadId: null, triggerAt: 5, enabled: false }] }),
    });
    expect(await getTasks()).toEqual([
      { id: 1, downloadId: 7, triggerAt: 1790000000, repeatSecs: 86400, enabled: true },
      { id: 2, downloadId: null, triggerAt: 5, repeatSecs: 0, enabled: false },
    ]);
  });

  test("createTask posts exactly the fields of a task", async () => {
    const daemon = installFakeDaemon(client, { "POST /scheduler": ({ body }) => ({ data: { ...body, id: 3 } }) });
    const task = await createTask({ downloadId: 7, triggerAt: 1790000000, junk: true });
    expect(daemon.calls[0].body).toEqual({ downloadId: 7, triggerAt: 1790000000, repeatSecs: 0, enabled: true });
    expect(task.id).toBe(3);
  });

  test("deleteTask deletes it", async () => {
    const daemon = installFakeDaemon(client, { "DELETE /scheduler/:id": () => ({ status: 200 }) });
    await deleteTask(3);
    expect(daemon.calls[0]).toMatchObject({ method: "DELETE", path: "/scheduler/3" });
  });

  test("a task with odd numbers is read safely", () => {
    expect(taskFromDaemon({ id: "4", downloadId: "9", triggerAt: -1, repeatSecs: "x" })).toEqual({ id: 4, downloadId: 9, triggerAt: 0, repeatSecs: 0, enabled: true });
    expect(taskFromDaemon(null).enabled).toBe(true);
  });
});
