/* FluxDM — store/downloadsSlice.js
 * The downloads the daemon knows about, as the UI model (model/downloads.js), newest first.
 *
 * The actions take what the model produced, never the daemon's raw JSON:
 *   received       the whole list (first load, a catch-up after a reconnect, and the periodic check; progress that is
 *                  further along in the store than in the list is kept, see mergeSnapshot)
 *   upserted       one download the daemon just returned (added, paused, resumed, limited)
 *   removed        one download the user removed
 *   eventReceived  one push event from /topic/downloads
 */
import { createEntityAdapter, createSelector, createSlice } from "@reduxjs/toolkit";
import { TABS, applyEvent, mergeSnapshot } from "../model/downloads";

const adapter = createEntityAdapter({ sortComparer: (a, b) => b.id - a.id });

const slice = createSlice({
  name: "downloads",
  initialState: adapter.getInitialState(),
  reducers: {
    received: (state, action) => {
      adapter.setAll(state, action.payload.map((d) => mergeSnapshot(state.entities[d.id], d)));
    },
    upserted: (state, action) => {
      adapter.upsertOne(state, action.payload);
    },
    removed: (state, action) => {
      adapter.removeOne(state, action.payload);
    },
    eventReceived: (state, action) => {
      const event = action.payload;
      if (!event || !event.id) return; /* id 0 is about the daemon and its core, not a download */
      if (event.type === "cancelled") {
        adapter.removeOne(state, event.id);
        return;
      }
      const current = state.entities[event.id];
      const next = applyEvent(current, event);
      if (next && next !== current) adapter.setOne(state, next);
    },
  },
});

export const { received, upserted, removed, eventReceived } = slice.actions;
export default slice.reducer;

/* ── Selectors ────────────────────────────────────────────────────────────────────────────────────────────── */

const selectors = adapter.getSelectors((state) => state.downloads);
export const selectAllDownloads = selectors.selectAll;
export const selectDownloadById = selectors.selectById;

/* How many downloads each tab shows, and the total. */
export const selectCounts = createSelector(selectAllDownloads, (downloads) => {
  const counts = { total: downloads.length };
  for (const [tab, states] of Object.entries(TABS)) {
    counts[tab] = downloads.filter((d) => states.includes(d.state)).length;
  }
  return counts;
});

/* The downloads one tab shows. */
export const selectTab = createSelector(
  [selectAllDownloads, (_state, tab) => tab],
  (downloads, tab) => downloads.filter((d) => (TABS[tab] || []).includes(d.state))
);
