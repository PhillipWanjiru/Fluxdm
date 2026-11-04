/* FluxDM — hooks/useSpeedHistory.js
 * The speed of all running downloads together, taken once a second for the last `windowSize` seconds, for the
 * graph. It is read from the store, which the live feed keeps current (store/liveSync.js): nothing is fetched. */
import { useEffect, useState } from "react";
import { useStore } from "react-redux";
import { selectTotalSpeed } from "../store/downloadsSlice";

export function useSpeedHistory(windowSize = 60) {
  const store = useStore();
  const [history, setHistory] = useState(() => Array.from({ length: windowSize }, (_, t) => ({ t, speed: 0 })));

  useEffect(() => {
    let t = windowSize;
    const timer = setInterval(() => {
      const sample = { t: t++, speed: selectTotalSpeed(store.getState()) };
      setHistory((previous) => [...previous.slice(1), sample]);
    }, 1000);
    return () => clearInterval(timer);
  }, [store, windowSize]);

  return history;
}
