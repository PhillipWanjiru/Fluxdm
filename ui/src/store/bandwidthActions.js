/* FluxDM — store/bandwidthActions.js
 * Reading and changing the limit on all downloads together (api/stats.js). */
import { getBandwidthLimit, setBandwidthLimit } from "../api/stats";
import { limitKnown } from "./bandwidthSlice";

/* Asks the daemon what the limit is. Resolves to whether it knows now; a failure is not thrown, because there is
 * nothing to do about it here: the limit stays as it was, and is read again on the next connection. */
export const loadLimit = () => async (dispatch) => {
  try {
    dispatch(limitKnown(await getBandwidthLimit()));
    return true;
  } catch {
    return false;
  }
};

/* Sets the limit, in bytes per second. Rejects with the daemon's reason, fit to show.
 *
 * The daemon stores a new limit before it tells the core (BandwidthService), so a refusal does not say which of the
 * two limits it holds: it is asked, and the store shows what it answers. */
export const saveLimit = (bytesPerSec) => async (dispatch) => {
  try {
    dispatch(limitKnown(await setBandwidthLimit(bytesPerSec)));
  } catch (error) {
    await dispatch(loadLimit());
    throw error;
  }
};
