/* FluxDM — downloadsSlice.js */
import { createSlice } from "@reduxjs/toolkit";
const slice = createSlice({ name:"downloads", initialState:[],
  reducers:{ set:(_,a)=>a.payload, upsert:(s,a)=>{ const i=s.findIndex(d=>d.id===a.payload.id); if(i>=0)s[i]={...s[i],...a.payload};else s.push(a.payload); } }
});
export const { set, upsert } = slice.actions;
export default slice.reducer;
