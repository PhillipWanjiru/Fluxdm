/* FluxDM — store/connectionSlice.js
 * Whether the UI is in touch with the daemon (link), whether the daemon is in touch with its core (core), and
 * whether the last attempt to load the list worked (syncError, "" when it did). See model/connection.js.
 *
 *   linkChanged    the push feed connected, lost its connection, or was refused
 *   coreChanged    the daemon reported its core up or down
 *   listLoaded     the list of downloads was loaded
 *   listFailed     it could not be, and why
 */
import { createSlice } from "@reduxjs/toolkit";
import { Core, Link } from "../model/connection";

export const initialConnection = { link: Link.CONNECTING, core: Core.UNKNOWN, coreReason: "", syncError: "" };

const slice = createSlice({
  name: "connection",
  initialState: initialConnection,
  reducers: {
    linkChanged: (state, action) => {
      state.link = action.payload;
      /* Without a link nothing is known about the core, and nothing is known about the list either. */
      if (action.payload !== Link.LIVE) {
        state.core = Core.UNKNOWN;
        state.coreReason = "";
        state.syncError = "";
      }
    },
    coreChanged: (state, action) => {
      const { up, reason } = action.payload;
      state.core = up ? Core.UP : Core.DOWN;
      state.coreReason = up ? "" : reason || "";
    },
    listLoaded: (state) => {
      state.syncError = "";
    },
    listFailed: (state, action) => {
      state.syncError = action.payload;
    },
  },
});

export const { linkChanged, coreChanged, listLoaded, listFailed } = slice.actions;
export default slice.reducer;

export const selectConnection = (state) => state.connection;
