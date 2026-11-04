/* FluxDM — store/downloadActions.js
 * What the user can do to a download: the daemon call first, then the store from what the daemon answers. Nothing
 * is shown before the daemon has agreed, so the list never claims something the daemon did not do.
 *
 *   await dispatch(pauseDownload(id))     a running or queued download stops (and keeps its partial data)
 *   await dispatch(resumeDownload(id))    a paused download goes on; a failed one is tried again
 *   await dispatch(removeDownload(id))    the download is cancelled and removed; a finished one's file stays
 *
 * A refusal (409 because the state does not allow it, 503 while the core is down, the daemon unreachable) is thrown
 * as the ApiError, for the caller to show. A download the daemon does not know any more (another client removed it)
 * is taken out of the list instead, since that is what is true now. */
import { cancel, pause, resume } from "../api/downloads";
import { removed, upserted } from "./downloadsSlice";

async function act(dispatch, id, call) {
  try {
    await call();
  } catch (error) {
    if (error && error.kind === "not_found") dispatch(removed(id));
    else throw error;
  }
}

export const pauseDownload = (id) => (dispatch) => act(dispatch, id, async () => dispatch(upserted(await pause(id))));

export const resumeDownload = (id) => (dispatch) => act(dispatch, id, async () => dispatch(upserted(await resume(id))));

export const removeDownload = (id) => (dispatch) =>
  act(dispatch, id, async () => {
    await cancel(id);
    dispatch(removed(id));
  });
