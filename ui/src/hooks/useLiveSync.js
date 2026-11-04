/* FluxDM — hooks/useLiveSync.js
 * Keeps the store in step with the daemon for as long as the component that calls it is mounted (store/liveSync.js).
 * One call, at the top of the app: the downloads, the connection state and the core's state are shared by every view. */
import { useEffect } from "react";
import { useStore } from "react-redux";
import { startLiveSync } from "../store/liveSync";

export function useLiveSync() {
  const store = useStore();
  useEffect(() => startLiveSync(store), [store]);
}
