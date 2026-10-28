/* FluxDM — store/index.js */
import { configureStore } from "@reduxjs/toolkit";
import downloadsReducer from "./downloadsSlice";
import settingsReducer  from "./settingsSlice";
import statsReducer     from "./statsSlice";
export const store = configureStore({
  reducer: { downloads:downloadsReducer, settings:settingsReducer, stats:statsReducer }
});
