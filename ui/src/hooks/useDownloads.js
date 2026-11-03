/* FluxDM — hooks/useDownloads.js
 * Keeps the store in step with the daemon by asking for the whole list every second. Replaced by the push
 * feed (STOMP over SockJS) in a later commit of this series. */
import { useEffect } from "react";
import { useDispatch } from "react-redux";
import { received } from "../store/downloadsSlice";
import { getAll } from "../api/downloads";

export function useDownloads() {
  const dispatch = useDispatch();
  useEffect(() => {
    const load = () => getAll().then((list) => dispatch(received(list))).catch(() => {});
    load();
    const timer = setInterval(load, 1000);
    return () => clearInterval(timer);
  }, [dispatch]);
}
