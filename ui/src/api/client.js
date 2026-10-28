/* FluxDM — api/client.js */
import axios from "axios";
export const client = axios.create({ baseURL:"http://localhost:7171/api", timeout:10000 });
client.interceptors.response.use(r=>r, e=>{ console.error("[FluxDM]",e); return Promise.reject(e); });
