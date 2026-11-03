/* FluxDM — api/downloads.js
 * The daemon's /api/downloads, in terms of the UI model (model/downloads.js): every call that returns a
 * download returns it already converted. A failed call rejects with an ApiError. */
import { client } from "./client";
import { ApiError } from "./errors";
import { fromDaemon } from "../model/downloads";

/* What POST /api/downloads reads (service/DownloadService.validated); anything else is left out. */
const START_FIELDS = ["url", "filePath", "segments", "priority", "checksum", "checksumType", "bandwidthLimit"];

const one = (response) => fromDaemon(response.data);

export async function getAll() {
  const { data } = await client.get("/downloads");
  if (!Array.isArray(data)) {
    throw new ApiError("The daemon sent something other than a list of downloads.", { kind: "protocol" });
  }
  return data.filter((d) => d && Number.isInteger(d.id)).map(fromDaemon);
}

export const getOne = async (id) => one(await client.get(`/downloads/${id}`));

/* request: { url, filePath, segments?, priority?, checksum?, checksumType?, bandwidthLimit? } */
export async function start(request) {
  const body = {};
  for (const field of START_FIELDS) if (request[field] !== undefined) body[field] = request[field];
  return one(await client.post("/downloads", body));
}

export const pause = async (id) => one(await client.post(`/downloads/${id}/pause`));

/* Also what retries a failed download. */
export const resume = async (id) => one(await client.post(`/downloads/${id}/resume`));

/* Stops the download, deletes its partial data and removes it from the list; a finished download's file is
 * never touched. */
export const cancel = async (id) => {
  await client.delete(`/downloads/${id}`);
};

/* bytesPerSec 0 removes the limit. */
export const setLimit = async (id, bytesPerSec) => one(await client.put(`/downloads/${id}/limit`, { bytesPerSec }));
