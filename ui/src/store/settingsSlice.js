/* FluxDM — settingsSlice.js
 * What the user can change here, all of it kept between runs (store/persist.js). The limit on all downloads together
 * is not among it: the daemon owns that (store/bandwidthSlice.js). */
import { createSlice } from "@reduxjs/toolkit";
import { DEFAULT_DIRECTORY } from "../model/destination";

export const initialSettings = { downloadDir:DEFAULT_DIRECTORY, defaultSegments:8, theme:"dark" };

const slice = createSlice({ name:"settings",
  initialState:initialSettings,
  reducers:{ update:(s,a)=>({...s,...a.payload}) }
});
export const { update } = slice.actions;
export default slice.reducer;
