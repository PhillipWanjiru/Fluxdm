/* FluxDM — Downloads/DownloadItem.jsx */
import React, { useState } from "react";
import FileIcon from "./FileIcon";
import StatusBadge from "./StatusBadge";
import SegmentBar from "./SegmentBar";

function fmt(mb) { return mb>=1000?`${(mb/1024).toFixed(1)} GB`:`${mb} MB`; }

export default function DownloadItem({ dl }) {
  const [expanded, setExpanded] = useState(false);
  const pct = dl.total_size > 0 ? Math.round((dl.downloaded/dl.total_size)*100) : 0;
  return (
    <div onClick={()=>setExpanded(!expanded)} style={{
      background:"var(--bg-panel)",border:`1px solid ${expanded?"rgba(74,244,138,0.25)":"var(--border)"}`,
      borderRadius:8,padding:"14px 16px",marginBottom:8,cursor:"pointer"}}>
      <div style={{display:"flex",alignItems:"center",gap:12}}>
        <FileIcon type={dl.type||"default"} />
        <div style={{flex:1,minWidth:0}}>
          <div style={{display:"flex",justifyContent:"space-between",marginBottom:6}}>
            <span style={{fontSize:11,color:"var(--text-main)",overflow:"hidden",textOverflow:"ellipsis",whiteSpace:"nowrap",maxWidth:"60%"}}>{dl.name}</span>
            <div style={{display:"flex",gap:8,alignItems:"center",flexShrink:0}}>
              <StatusBadge status={dl.status} />
              <span style={{fontSize:10,color:"var(--text-dim)"}}>{fmt(dl.total_size||0)}</span>
            </div>
          </div>
          <SegmentBar pct={pct} segments={dl.segments||8} status={dl.status} />
          <div style={{display:"flex",justifyContent:"space-between",marginTop:4}}>
            <span style={{fontSize:9,color:"var(--green)"}}>{dl.status==="downloading"?`${dl.speed||0} MB/s`:dl.status}</span>
            <span style={{fontSize:9,color:"var(--text-dim)"}}>{pct}%</span>
          </div>
        </div>
      </div>
      {expanded&&(
        <div style={{marginTop:14,paddingTop:14,borderTop:"1px solid var(--border)",display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:12}}>
          {[["SEGMENTS",dl.segments+"×"],["PROTOCOL","HTTP/1.1"],["STATUS",dl.status||"—"],["ID",dl.id]].map(([k,v])=>(
            <div key={k}><div style={{fontSize:9,color:"var(--text-dim)",marginBottom:3}}>{k}</div><div style={{fontSize:11,color:"#888"}}>{v}</div></div>
          ))}
        </div>
      )}
    </div>
  );
}
