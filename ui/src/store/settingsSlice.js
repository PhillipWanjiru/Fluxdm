/* FluxDM — settingsSlice.js
 * What the user can change. downloadDir, defaultSegments and theme are kept between runs (store/persist.js). */
import { createSlice } from "@reduxjs/toolkit";
import { DEFAULT_DIRECTORY } from "../model/destination";

export const initialSettings = { globalLimit:0, downloadDir:DEFAULT_DIRECTORY, defaultSegments:8, theme:"dark" };

const slice = createSlice({ name:"settings",
  initialState:initialSettings,
  reducers:{ update:(s,a)=>({...s,...a.payload}) }
});
export const { update } = slice.actions;
export default slice.reducer;
