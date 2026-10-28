/* FluxDM — Downloads/DownloadList.jsx */
import React, { useState } from "react";
import { useSelector } from "react-redux";
import DownloadItem from "./DownloadItem";
import StatCard from "../Shared/StatCard";
import SpeedGraph from "../Shared/SpeedGraph";
import { useDownloads } from "../../hooks/useDownloads";
import { useSpeedData } from "../../hooks/useSpeedData";
import AddDownloadModal from "../Modals/AddDownloadModal";

export default function DownloadList() {
  const [tab, setTab]         = useState("active");
  const [showAdd, setShowAdd] = useState(false);
  const downloads             = useSelector(s => s.downloads);
  const speedData             = useSpeedData();
  useDownloads();

  const active    = downloads.filter(d=>d.status==="downloading"||d.status==="paused");
  const queued    = downloads.filter(d=>d.status==="queued");
  const complete  = downloads.filter(d=>d.status==="complete");
  const filtered  = tab==="active"?active:tab==="queued"?queued:complete;
  const totalSpd  = speedData[speedData.length-1]?.speed||0;

  return (
    <div style={{display:"flex",flexDirection:"column",height:"100%"}}>
      {showAdd&&<AddDownloadModal onClose={()=>setShowAdd(false)}/>}
      <div style={{padding:"0 20px",height:52,display:"flex",alignItems:"center",justifyContent:"space-between",borderBottom:"1px solid var(--border)"}}>
        <div style={{display:"flex",gap:20}}>
          {["active","queued","complete"].map(t=>(
            <button key={t} onClick={()=>setTab(t)} style={{background:"none",border:"none",
              color:tab===t?"#fff":"var(--text-dim)",fontSize:11,cursor:"pointer",fontFamily:"inherit",
              letterSpacing:1,padding:"0 0 14px",borderBottom:`2px solid ${tab===t?"var(--green)":"transparent"}`,textTransform:"uppercase"}}>
              {t}
            </button>
          ))}
        </div>
        <button onClick={()=>setShowAdd(true)} style={{padding:"8px 16px",background:"var(--green)",border:"none",borderRadius:6,color:"#000",fontSize:11,fontWeight:700,cursor:"pointer",fontFamily:"inherit",letterSpacing:1}}>
          + ADD URL
        </button>
      </div>

      <div style={{margin:"16px 20px 0",background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:8,padding:"12px 16px"}}>
        <div style={{display:"flex",justifyContent:"space-between",marginBottom:8}}>
          <span style={{fontSize:9,color:"var(--text-dim)",letterSpacing:1}}>LIVE SPEED</span>
          <span style={{fontSize:10,color:"var(--green)"}}>↓ {totalSpd.toFixed(1)} MB/s</span>
        </div>
        <SpeedGraph data={speedData}/>
      </div>

      <div style={{display:"grid",gridTemplateColumns:"repeat(4,1fr)",gap:10,margin:"12px 20px 0"}}>
        <StatCard label="ACTIVE"     value={active.length}   color="var(--green)"/>
        <StatCard label="QUEUED"     value={queued.length}   color="var(--amber)"/>
        <StatCard label="COMPLETED"  value={complete.length} color="var(--blue)"/>
        <StatCard label="TOTAL"      value={downloads.length}color="var(--purple)"/>
      </div>

      <div style={{flex:1,overflowY:"auto",padding:"12px 20px 20px"}}>
        {filtered.length===0&&<div style={{textAlign:"center",padding:40,color:"var(--text-dim)",fontSize:11}}>NO DOWNLOADS IN THIS VIEW</div>}
        {filtered.map(d=><DownloadItem key={d.id} dl={d}/>)}
      </div>
    </div>
  );
}
