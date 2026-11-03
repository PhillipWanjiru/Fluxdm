/* FluxDM — store/index.js */
import { configureStore } from "@reduxjs/toolkit";
import downloadsReducer from "./downloadsSlice";
import settingsReducer  from "./settingsSlice";
import statsReducer     from "./statsSlice";

/* A function so a test can have a store of its own. */
export const makeStore = (preloadedState) => configureStore({
  reducer: { downloads:downloadsReducer, settings:settingsReducer, stats:statsReducer },
  preloadedState,
});
export const store = makeStore();
