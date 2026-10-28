/* FluxDM — hooks/useDownloads.js */
import { useEffect } from "react";
import { useDispatch } from "react-redux";
import { set } from "../store/downloadsSlice";
import { getAll } from "../api/downloads";
export function useDownloads() {
  const dispatch = useDispatch();
  useEffect(() => {
    getAll().then(r => dispatch(set(r.data)));
    const id = setInterval(() => getAll().then(r => dispatch(set(r.data))), 1000);
    return () => clearInterval(id);
  }, []);
}
