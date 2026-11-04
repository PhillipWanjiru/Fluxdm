/* FluxDM — Downloads/DownloadItem.jsx
 * One download: how far it is, what can be done to it in its state, and, opened, where it came from and went.
 * What it can do is the daemon's own rule (api/DownloadController): pause a running or queued download, resume a
 * paused one, retry a failed one, remove any. Each waits for the daemon's answer, and a refusal is shown here. */
import React, { useState } from "react";
import { useDispatch } from "react-redux";
import FileIcon from "./FileIcon";
import StatusBadge from "./StatusBadge";
import SegmentBar from "./SegmentBar";
import ConfirmModal from "../Modals/ConfirmModal";
import { State, percentOf } from "../../model/downloads";
import { formatBytes, formatEta, formatSpeed } from "../../model/format";
import { pauseDownload, removeDownload, resumeDownload } from "../../store/downloadActions";

const hostOf = (url) => { try { return new URL(url).host; } catch { return "—"; } };

/* The one action each state offers besides removing. A finished download offers none. */
const PRIMARY = {
  [State.DOWNLOADING]: { label: "Pause",  run: pauseDownload },
  [State.QUEUED]:      { label: "Pause",  run: pauseDownload },
  [State.PAUSED]:      { label: "Resume", run: resumeDownload },
  [State.FAILED]:      { label: "Retry",  run: resumeDownload },
};

function ActionButton({ label, name, danger, disabled, onClick }) {
  const color = danger ? "var(--red)" : "var(--green)";
  return (
    <button type="button" aria-label={`${label} ${name}`} disabled={disabled} onClick={onClick}
      style={{padding:"5px 10px",background:"var(--bg-hover)",border:`1px solid ${color}55`,borderRadius:5,color,fontSize:10,
        letterSpacing:0.5,cursor:disabled?"default":"pointer",opacity:disabled?0.5:1,fontFamily:"inherit"}}>
      {label}
    </button>
  );
}

export default function DownloadItem({ dl }) {
  const dispatch = useDispatch();
  const [expanded, setExpanded] = useState(false);
  const [asking, setAsking]     = useState(false); /* the confirmation of "remove" is open */
  const [busy, setBusy]         = useState(false); /* a call to the daemon is on its way */
  const [problem, setProblem]   = useState("");

  const act = async (run) => {
    setBusy(true);
    setProblem("");
    try {
      await dispatch(run(dl.id));
    } catch (error) {
      setProblem(error.message);
    } finally {
      setBusy(false);
    }
  };

  const pct      = percentOf(dl);
  const running  = dl.state === State.DOWNLOADING;
  const failed   = dl.state === State.FAILED;
  const complete = dl.state === State.COMPLETE;
  const primary  = PRIMARY[dl.state];
  const detailsId = `details-${dl.id}`;

  return (
    <div data-testid={`download-${dl.id}`} style={{
      background:"var(--bg-panel)",border:`1px solid ${expanded?"rgba(74,244,138,0.25)":"var(--border)"}`,
      borderRadius:8,padding:"14px 16px",marginBottom:8}}>
      <div style={{display:"flex",alignItems:"center",gap:12}}>
        <FileIcon type={dl.type} />
        <div style={{flex:1,minWidth:0}}>
          <div style={{display:"flex",justifyContent:"space-between",gap:8,marginBottom:6}}>
            <button type="button" onClick={()=>setExpanded(!expanded)} aria-expanded={expanded} aria-controls={detailsId} title={dl.name}
              style={{background:"none",border:"none",padding:0,fontSize:11,color:"var(--text-main)",fontFamily:"inherit",cursor:"pointer",
                textAlign:"left",overflow:"hidden",textOverflow:"ellipsis",whiteSpace:"nowrap",minWidth:0}}>
              {dl.name}
            </button>
            <div style={{display:"flex",gap:8,alignItems:"center",flexShrink:0}}>
              <StatusBadge state={dl.state} />
              <span style={{fontSize:10,color:"var(--text-dim)"}}>{dl.totalSize>0?formatBytes(dl.totalSize):"size unknown"}</span>
            </div>
          </div>
          <SegmentBar pct={pct} segments={dl.segments} state={dl.state} label={`${dl.name} progress`} />
          <div style={{display:"flex",justifyContent:"space-between",marginTop:4,gap:12}}>
            <span style={{fontSize:9,color:failed?"var(--red)":"var(--green)",overflow:"hidden",textOverflow:"ellipsis",whiteSpace:"nowrap"}}>
              {running?formatSpeed(dl.speedBps):failed?(dl.error||"failed"):dl.state}
            </span>
            <span style={{fontSize:9,color:"var(--text-dim)",flexShrink:0}}>
              {running&&dl.etaSeconds!==null?`${formatEta(dl.etaSeconds)} left · `:""}{pct}%
            </span>
          </div>
        </div>
        <div role="group" aria-label={`Actions for ${dl.name}`} style={{display:"flex",flexDirection:"column",gap:6,flexShrink:0}}>
          {primary&&<ActionButton label={primary.label} name={dl.name} disabled={busy} onClick={()=>act(primary.run)}/>}
          <ActionButton label="Remove" name={dl.name} danger disabled={busy} onClick={()=>setAsking(true)}/>
        </div>
      </div>

      {problem&&(
        <div role="alert" style={{marginTop:10,padding:"8px 10px",background:"rgba(244,74,74,0.08)",border:"1px solid var(--red)",
          borderRadius:6,color:"var(--red)",fontSize:10,wordBreak:"break-word"}}>
          {problem}
        </div>
      )}

      {expanded&&(
        <div id={detailsId} style={{marginTop:14,paddingTop:14,borderTop:"1px solid var(--border)",display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:12}}>
          {[["SEGMENTS",dl.segments+"×"],["SOURCE",hostOf(dl.url)],["DOWNLOADED",formatBytes(dl.downloaded)],["ID",dl.id]].map(([k,v])=>(
            <div key={k}><div style={{fontSize:9,color:"var(--text-dim)",marginBottom:3}}>{k}</div><div style={{fontSize:11,color:"#888"}}>{v}</div></div>
          ))}
          <div style={{gridColumn:"1 / -1"}}>
            <div style={{fontSize:9,color:"var(--text-dim)",marginBottom:3}}>SAVED AS</div>
            <div style={{fontSize:11,color:"#888",wordBreak:"break-all"}}>{dl.filePath}</div>
          </div>
          {failed&&dl.error&&(
            <div style={{gridColumn:"1 / -1"}}>
              <div style={{fontSize:9,color:"var(--text-dim)",marginBottom:3}}>WHY IT FAILED</div>
              <div style={{fontSize:11,color:"var(--red)",wordBreak:"break-word"}}>{dl.error}</div>
            </div>
          )}
        </div>
      )}

      {asking&&(
        <ConfirmModal
          title={complete?"Remove from the list?":"Remove this download?"}
          message={complete
            ?`${dl.name} is removed from the list. The file it saved stays where it is.`
            :`${dl.name} stops, and what was downloaded of it so far is deleted.`}
          confirmLabel="Remove"
          onCancel={()=>setAsking(false)}
          onConfirm={()=>{ setAsking(false); act(removeDownload); }}
        />
      )}
    </div>
  );
}
