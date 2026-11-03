/* FluxDM — Downloads/DownloadItem.jsx */
import React, { useState } from "react";
import FileIcon from "./FileIcon";
import StatusBadge from "./StatusBadge";
import SegmentBar from "./SegmentBar";
import { State, percentOf } from "../../model/downloads";
import { formatBytes, formatEta, formatSpeed } from "../../model/format";

const hostOf = (url) => { try { return new URL(url).host; } catch { return "—"; } };

export default function DownloadItem({ dl }) {
  const [expanded, setExpanded] = useState(false);
  const pct = percentOf(dl);
  const running = dl.state === State.DOWNLOADING;
  const failed  = dl.state === State.FAILED;
  return (
    <div data-testid={`download-${dl.id}`} onClick={()=>setExpanded(!expanded)} style={{
      background:"var(--bg-panel)",border:`1px solid ${expanded?"rgba(74,244,138,0.25)":"var(--border)"}`,
      borderRadius:8,padding:"14px 16px",marginBottom:8,cursor:"pointer"}}>
      <div style={{display:"flex",alignItems:"center",gap:12}}>
        <FileIcon type={dl.type} />
        <div style={{flex:1,minWidth:0}}>
          <div style={{display:"flex",justifyContent:"space-between",marginBottom:6}}>
            <span style={{fontSize:11,color:"var(--text-main)",overflow:"hidden",textOverflow:"ellipsis",whiteSpace:"nowrap",maxWidth:"60%"}}>{dl.name}</span>
            <div style={{display:"flex",gap:8,alignItems:"center",flexShrink:0}}>
              <StatusBadge state={dl.state} />
              <span style={{fontSize:10,color:"var(--text-dim)"}}>{dl.totalSize>0?formatBytes(dl.totalSize):"size unknown"}</span>
            </div>
          </div>
          <SegmentBar pct={pct} segments={dl.segments} state={dl.state} />
          <div style={{display:"flex",justifyContent:"space-between",marginTop:4,gap:12}}>
            <span style={{fontSize:9,color:failed?"var(--red)":"var(--green)",overflow:"hidden",textOverflow:"ellipsis",whiteSpace:"nowrap"}}>
              {running?formatSpeed(dl.speedBps):failed?(dl.error||"failed"):dl.state}
            </span>
            <span style={{fontSize:9,color:"var(--text-dim)",flexShrink:0}}>
              {running&&dl.etaSeconds!==null?`${formatEta(dl.etaSeconds)} left · `:""}{pct}%
            </span>
          </div>
        </div>
      </div>
      {expanded&&(
        <div style={{marginTop:14,paddingTop:14,borderTop:"1px solid var(--border)",display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:12}}>
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
    </div>
  );
}
