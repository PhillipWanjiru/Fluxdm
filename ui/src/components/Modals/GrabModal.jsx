/* FluxDM — Modals/GrabModal.jsx
 * Which format of a video to fetch, and the wait while the grabber fetches it.
 *
 * The grabber (the Python service behind the daemon) downloads the whole video before it answers, which takes as
 * long as the download does, so the dialog stays open and says so. "Stop waiting" lets go of the answer; it cannot
 * stop the grabber, which finishes what it started: the dialog says that too. */
import React, { useEffect, useState } from "react";
import FormatPicker from "../Grabber/FormatPicker";
import { BEST, formatChoices } from "../../model/formats";
import { formatDuration } from "../../model/format";

const button = { flex: 1, padding: 10, borderRadius: 6, cursor: "pointer", fontFamily: "inherit", fontSize: 12 };

/* title, site, duration: what the page is, from the detection. formats: its formats (api/grabber.js, detect).
 * busy: the grabber is fetching. error: why the last try failed. */
export default function GrabModal({ title, site, duration, formats, busy = false, error = "", onGrab, onStopWaiting, onClose }) {
  const [selected, setSelected] = useState(BEST);
  const choices = formatChoices(formats);

  useEffect(() => {
    const onKey = (e) => { if (e.key === "Escape" && !busy) onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [busy, onClose]);

  return (
    <div style={{position:"fixed",inset:0,background:"rgba(0,0,0,0.75)",display:"flex",alignItems:"center",justifyContent:"center",zIndex:100}}>
      <div role="dialog" aria-modal="true" aria-labelledby="grab-title" aria-describedby="grab-about"
        style={{background:"#111",border:"1px solid var(--border)",borderRadius:10,padding:24,width:460,maxWidth:"calc(100vw - 32px)",fontFamily:"monospace"}}>
        <div id="grab-title" style={{color:"#fff",fontSize:14,fontWeight:700,letterSpacing:1,marginBottom:6}}>CHOOSE A FORMAT</div>
        <div id="grab-about" style={{fontSize:11,color:"var(--text-dim)",marginBottom:16,wordBreak:"break-word"}}>
          <span style={{color:"var(--text-main)"}}>{title || "Untitled"}</span>
          {site ? ` · ${site}` : ""}
          {duration ? ` · ${formatDuration(duration)}` : ""}
        </div>

        <div style={{maxHeight:240,overflowY:"auto",marginBottom:14}}>
          <FormatPicker choices={choices} selected={selected} onSelect={setSelected} disabled={busy} autoFocus />
        </div>

        <div style={{fontSize:9,color:"var(--text-dim)",marginBottom:14,lineHeight:1.5}}>
          The grabber saves the file in its own download folder (by default ~/Downloads); it does not appear in the list of downloads.
        </div>

        {busy && (
          <div role="status" style={{marginBottom:14,padding:"8px 10px",background:"rgba(74,244,138,0.06)",border:"1px solid var(--green)",borderRadius:6,color:"var(--green)",fontSize:11,lineHeight:1.5}}>
            Downloading in the grabber. This takes as long as the video does. If you stop waiting, the grabber still finishes it.
          </div>
        )}
        {error && (
          <div role="alert" style={{marginBottom:14,padding:"8px 10px",background:"rgba(244,74,74,0.08)",border:"1px solid var(--red)",borderRadius:6,color:"var(--red)",fontSize:11,wordBreak:"break-word"}}>
            {error}
          </div>
        )}

        <div style={{display:"flex",gap:10}}>
          {busy
            ? <button type="button" onClick={onStopWaiting} style={{...button,background:"#0a0a0a",border:"1px solid var(--border)",color:"var(--text-dim)"}}>Stop waiting</button>
            : <button type="button" onClick={onClose} style={{...button,background:"#0a0a0a",border:"1px solid var(--border)",color:"var(--text-dim)"}}>Cancel</button>}
          <button type="button" disabled={busy} onClick={() => onGrab(selected)}
            style={{...button,background:"rgba(74,244,138,0.1)",border:"1px solid var(--green)",color:"var(--green)",cursor:busy?"wait":"pointer",opacity:busy?0.6:1}}>
            {busy ? "Downloading…" : "Download"}
          </button>
        </div>
      </div>
    </div>
  );
}
