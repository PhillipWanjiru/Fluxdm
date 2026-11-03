/* FluxDM — Modals/AddDownloadModal.jsx
 * Asks for an address and where to save it. The daemon needs both (POST /api/downloads: url and filePath), and
 * refuses a path that another open download already uses, so what it answers is shown here, not swallowed. */
import React, { useEffect, useState } from "react";
import { useDispatch } from "react-redux";
import { start } from "../../api/downloads";
import { useSettings } from "../../hooks/useSettings";
import { upserted } from "../../store/downloadsSlice";
import { SEGMENT_CHOICES, fileNameFromUrl, joinPath, validateDownload } from "../../model/destination";

const label = {fontSize:10,color:"var(--text-dim)",letterSpacing:1,display:"block",marginBottom:6};
const field = (bad) => ({width:"100%",background:"#0a0a0a",border:`1px solid ${bad?"var(--red)":"var(--border)"}`,borderRadius:6,padding:"10px 12px",color:"#fff",fontSize:12,fontFamily:"inherit",boxSizing:"border-box",outline:"none"});

export default function AddDownloadModal({ onClose }) {
  const dispatch = useDispatch();
  const { settings } = useSettings();
  const [url,setUrl]             = useState("");
  const [fileName,setFileName]   = useState("");
  const [nameEdited,setEdited]   = useState(false);
  const [directory,setDirectory] = useState(settings.downloadDir);
  const [segs,setSegs]           = useState(settings.defaultSegments);
  const [tried,setTried]         = useState(false);
  const [busy,setBusy]           = useState(false);
  const [refusal,setRefusal]     = useState("");

  useEffect(() => {
    const onKey = (e) => { if (e.key === "Escape" && !busy) onClose(); };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [busy, onClose]);

  const changeUrl = (value) => {
    setUrl(value); setRefusal("");
    if (!nameEdited) setFileName(fileNameFromUrl(value));   /* until the user names the file themselves */
  };

  const problem = validateDownload({ url, directory, fileName });
  const shown   = tried ? problem : null;
  const target  = joinPath(directory, fileName.trim());

  const submit = async (e) => {
    e.preventDefault();
    setTried(true);
    if (problem || busy) return;
    setBusy(true); setRefusal("");
    try {
      dispatch(upserted(await start({ url: url.trim(), filePath: target, segments: segs })));
      onClose();
    } catch (err) {
      setRefusal(err.message);
      setBusy(false);
    }
  };

  return (
    <div style={{position:"fixed",inset:0,background:"rgba(0,0,0,0.75)",display:"flex",alignItems:"center",justifyContent:"center",zIndex:100}}>
      <form onSubmit={submit} noValidate role="dialog" aria-modal="true" aria-labelledby="add-title"
        style={{background:"#111",border:"1px solid var(--border)",borderRadius:12,padding:28,width:520,fontFamily:"monospace"}}>
        <div style={{display:"flex",justifyContent:"space-between",alignItems:"center",marginBottom:20}}>
          <span id="add-title" style={{color:"#fff",fontSize:14,fontWeight:700,letterSpacing:1}}>NEW DOWNLOAD</span>
          <button type="button" onClick={onClose} aria-label="Close" style={{background:"none",border:"none",color:"var(--text-dim)",fontSize:18,cursor:"pointer"}}>×</button>
        </div>

        <label htmlFor="add-url" style={label}>URL</label>
        <input id="add-url" value={url} onChange={e=>changeUrl(e.target.value)} placeholder="https://" autoFocus spellCheck={false}
          aria-invalid={shown?.field==="url"} style={{...field(shown?.field==="url"),marginBottom:14}}/>

        <label htmlFor="add-dir" style={label}>SAVE TO FOLDER</label>
        <input id="add-dir" value={directory} onChange={e=>setDirectory(e.target.value)} spellCheck={false}
          aria-invalid={shown?.field==="directory"} style={{...field(shown?.field==="directory"),marginBottom:14}}/>

        <label htmlFor="add-name" style={label}>FILE NAME</label>
        <input id="add-name" value={fileName} onChange={e=>{setFileName(e.target.value);setEdited(true);}} spellCheck={false}
          aria-invalid={shown?.field==="fileName"} style={field(shown?.field==="fileName")}/>
        <div style={{fontSize:9,color:"var(--text-dim)",margin:"6px 0 14px",wordBreak:"break-all",minHeight:11}}>{target&&fileName.trim()?`→ ${target}`:""}</div>

        <span style={label}>SEGMENTS</span>
        <div role="group" aria-label="Segments" style={{display:"flex",gap:4,marginBottom:20}}>
          {SEGMENT_CHOICES.map(n=>(
            <button key={n} type="button" onClick={()=>setSegs(n)} aria-pressed={segs===n} style={{flex:1,padding:"8px 0",background:segs===n?"rgba(74,244,138,0.1)":"#0a0a0a",border:`1px solid ${segs===n?"var(--green)":"var(--border)"}`,borderRadius:6,color:segs===n?"var(--green)":"var(--text-dim)",fontSize:12,cursor:"pointer",fontFamily:"inherit"}}>{n}×</button>
          ))}
        </div>

        {(shown||refusal)&&(
          <div role="alert" style={{marginBottom:16,padding:"10px 12px",background:"rgba(244,74,74,0.08)",border:"1px solid var(--red)",borderRadius:6,color:"var(--red)",fontSize:11}}>
            {shown?shown.message:refusal}
          </div>
        )}

        <div style={{display:"flex",gap:10}}>
          <button type="button" onClick={onClose} disabled={busy} style={{flex:1,padding:11,background:"#0a0a0a",border:"1px solid var(--border)",borderRadius:6,color:"var(--text-dim)",fontSize:12,cursor:"pointer",fontFamily:"inherit"}}>CANCEL</button>
          <button type="submit" disabled={busy} style={{flex:2,padding:11,background:"rgba(74,244,138,0.1)",border:"1px solid var(--green)",borderRadius:6,color:"var(--green)",fontSize:12,fontWeight:700,cursor:busy?"wait":"pointer",fontFamily:"inherit",letterSpacing:1}}>{busy?"STARTING…":"START DOWNLOAD"}</button>
        </div>
      </form>
    </div>
  );
}
