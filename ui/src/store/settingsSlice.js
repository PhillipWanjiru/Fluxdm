/* FluxDM — settingsSlice.js */
import { createSlice } from "@reduxjs/toolkit";
const slice = createSlice({ name:"settings",
  initialState:{ globalLimit:0, defaultSegments:8, theme:"dark" },
  reducers:{ update:(s,a)=>({...s,...a.payload}) }
});
export const { update } = slice.actions;
export default slice.reducer;
