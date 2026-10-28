/* FluxDM — api/downloads.js */
import { client } from "./client";
export const getAll    = ()     => client.get("/downloads");
export const start     = (data) => client.post("/downloads", data);
export const pause     = (id)   => client.post(`/downloads/${id}/pause`);
export const resume    = (id)   => client.post(`/downloads/${id}/resume`);
export const cancel    = (id)   => client.delete(`/downloads/${id}`);
export const getStatus = (id)   => client.get(`/downloads/${id}/status`);
