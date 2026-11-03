/* FluxDM — api/scheduler.js */
import { client } from "./client";
import { ApiError } from "./errors";
import { taskFromDaemon, taskToDaemon } from "../model/tasks";

export async function getTasks() {
  const { data } = await client.get("/scheduler");
  if (!Array.isArray(data)) throw new ApiError("The daemon sent something other than a list of tasks.", { kind: "protocol" });
  return data.map(taskFromDaemon);
}

/* task: { downloadId, triggerAt (epoch seconds), repeatSecs?, enabled? } */
export const createTask = async (task) => taskFromDaemon((await client.post("/scheduler", taskToDaemon(task))).data);

export const deleteTask = async (id) => {
  await client.delete(`/scheduler/${id}`);
};
