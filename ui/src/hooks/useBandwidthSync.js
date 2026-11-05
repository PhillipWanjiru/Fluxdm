/* FluxDM — hooks/useBandwidthSync.js
 * Reads the limit on all downloads together from the daemon whenever the app has a live link to it: at start, and
 * again after every reconnect (the daemon may have been restarted, or the limit changed by another client).
 * One call, at the top of the app. */
import { useEffect } from "react";
import { useDispatch, useSelector } from "react-redux";
import { Link } from "../model/connection";
import { loadLimit } from "../store/bandwidthActions";

export function useBandwidthSync() {
  const dispatch = useDispatch();
  const live = useSelector((state) => state.connection.link === Link.LIVE);
  useEffect(() => {
    if (live) dispatch(loadLimit());
  }, [live, dispatch]);
}
