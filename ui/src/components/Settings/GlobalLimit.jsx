/* FluxDM — Settings/GlobalLimit.jsx
 * The limit on all downloads together. It is the daemon's, so the field shows what the daemon holds (read when the
 * app connects and when this opens), and a change is only a change once the daemon has accepted it: it is sent with
 * Apply, never while typing, because every keystroke would otherwise set a speed limit on live downloads. */
import React, { useEffect, useState } from "react";
import { useDispatch, useSelector } from "react-redux";
import { limitToKb, parseLimit } from "../../model/bandwidth";
import { formatSpeed } from "../../model/format";
import { loadLimit, saveLimit } from "../../store/bandwidthActions";
import { selectLimitBps } from "../../store/bandwidthSlice";

export default function GlobalLimit() {
  const dispatch = useDispatch();
  const limit = useSelector(selectLimitBps); /* null: the daemon has not said yet */
  const [draft, setDraft] = useState(null); /* what was typed; null while the field shows what the daemon holds */
  const [busy, setBusy] = useState(false);
  const [refusal, setRefusal] = useState("");
  const [applied, setApplied] = useState(false);

  useEffect(() => {
    dispatch(loadLimit());
  }, [dispatch]);

  const known = limit !== null;
  const typed = draft !== null;
  const parsed = typed ? parseLimit(draft) : null;
  const invalid = Boolean(parsed && parsed.error);
  const canApply = known && !busy && typed && !invalid && parsed.bps !== limit;

  const edit = (value) => {
    setDraft(value);
    setRefusal("");
    setApplied(false);
  };

  const apply = async (event) => {
    event.preventDefault();
    if (!canApply) return;
    setBusy(true);
    setRefusal("");
    setApplied(false);
    try {
      await dispatch(saveLimit(parsed.bps));
      setDraft(null);
      setApplied(true);
    } catch (error) {
      setRefusal(error.message);
    } finally {
      setBusy(false);
    }
  };

  let help = "Waiting for the daemon…";
  if (invalid) help = parsed.error;
  else if (known) help = limit === 0 ? "Currently no limit." : `Currently ${formatSpeed(limit)}, for all downloads together.`;

  return (
    <form onSubmit={apply} noValidate aria-label="Global bandwidth limit">
      <label htmlFor="global-limit" style={{fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6}}>
        GLOBAL BANDWIDTH LIMIT (KB/s, 0 = unlimited)
      </label>
      <div style={{display:"flex",gap:8}}>
        <input id="global-limit" inputMode="numeric" autoComplete="off" spellCheck={false}
          value={typed ? draft : known ? String(limitToKb(limit)) : ""} placeholder={known ? "" : "—"}
          disabled={!known || busy} onChange={(e) => edit(e.target.value)}
          aria-invalid={invalid} aria-describedby="global-limit-help"
          style={{flex:1,background:"var(--bg-panel)",border:`1px solid ${invalid?"var(--red)":"var(--border)"}`,borderRadius:6,padding:"9px 12px",color:"#fff",fontFamily:"monospace",fontSize:12,outline:"none",minWidth:0}} />
        <button type="submit" disabled={!canApply}
          style={{padding:"0 18px",background:canApply?"rgba(74,244,138,0.1)":"var(--bg-panel)",border:`1px solid ${canApply?"var(--green)":"var(--border)"}`,borderRadius:6,color:canApply?"var(--green)":"var(--text-dim)",fontSize:11,fontWeight:700,letterSpacing:1,cursor:canApply?"pointer":"default",fontFamily:"monospace"}}>
          {busy ? "APPLYING…" : "APPLY"}
        </button>
      </div>
      <div id="global-limit-help" style={{fontSize:9,color:invalid?"var(--red)":"var(--text-dim)",marginTop:6}}>{help}</div>
      {refusal && (
        <div role="alert" style={{marginTop:8,padding:"8px 10px",background:"rgba(244,74,74,0.08)",border:"1px solid var(--red)",borderRadius:6,color:"var(--red)",fontSize:11,wordBreak:"break-word"}}>
          {refusal}
        </div>
      )}
      <div role="status" style={{fontSize:10,color:"var(--green)",marginTop:6,minHeight:12}}>{applied ? "Limit applied." : ""}</div>
    </form>
  );
}
