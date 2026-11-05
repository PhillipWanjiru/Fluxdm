/* FluxDM — store/bandwidthSlice.js
 * The limit on all downloads together, as the daemon holds it (bytes per second, 0 for none). The daemon owns it:
 * it is read when the app connects and after every change, and kept nowhere else. null is "not known yet". */
import { createSlice } from "@reduxjs/toolkit";

const slice = createSlice({
  name: "bandwidth",
  initialState: { limitBps: null },
  reducers: {
    limitKnown: (state, action) => {
      state.limitBps = action.payload;
    },
  },
});

export const { limitKnown } = slice.actions;
export const selectLimitBps = (state) => state.bandwidth.limitBps;
export default slice.reducer;
