/* FluxDM — api/grabber.js */
import { client } from "./client";
export const detect = (url)            => client.post("/grabber/detect", { url });
export const grab   = (url, formatId)  => client.post("/grabber/grab",   { url, formatId });
