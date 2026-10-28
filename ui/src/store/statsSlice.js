/* FluxDM — statsSlice.js */
import { createSlice } from "@reduxjs/toolkit";
const slice = createSlice({ name:"stats",
  initialState:{ speedBps:0, activeCount:0, totalDownloaded:0 },
  reducers:{ update:(s,a)=>({...s,...a.payload}) }
});
export const { update } = slice.actions;
export default slice.reducer;
