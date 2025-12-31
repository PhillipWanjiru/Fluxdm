/* FluxDM — api/scheduler.js */
import { client } from "./client";
export const getTasks   = ()     => client.get("/scheduler");
export const createTask = (data) => client.post("/scheduler", data);
export const deleteTask = (id)   => client.delete(`/scheduler/${id}`);
