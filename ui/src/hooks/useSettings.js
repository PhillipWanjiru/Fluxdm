/* FluxDM — hooks/useSettings.js */
import { useSelector, useDispatch } from "react-redux";
import { update } from "../store/settingsSlice";
export function useSettings() {
  const settings = useSelector(s => s.settings);
  const dispatch = useDispatch();
  return { settings, update: d => dispatch(update(d)) };
}
