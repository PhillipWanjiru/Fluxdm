/* FluxDM — store/index.js */
import { configureStore } from "@reduxjs/toolkit";
import downloadsReducer from "./downloadsSlice";
import settingsReducer, { initialSettings } from "./settingsSlice";
import statsReducer     from "./statsSlice";
import connectionReducer from "./connectionSlice";
import { loadSettings, persistSettings } from "./persist";

/* A function so a test can have a store of its own. */
export const makeStore = (preloadedState) => configureStore({
  reducer: { downloads:downloadsReducer, settings:settingsReducer, stats:statsReducer, connection:connectionReducer },
  preloadedState,
});

export const store = makeStore({ settings: { ...initialSettings, ...loadSettings() } });
persistSettings(store);
