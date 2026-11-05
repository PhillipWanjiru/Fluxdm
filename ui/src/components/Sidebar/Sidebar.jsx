/* FluxDM — Sidebar/Sidebar.jsx
 * The way between the views, and, always in sight, how fast things are going and whether the daemon is there. */
import React from "react";
import NavItem from "./NavItem";
import BandwidthMeter from "./BandwidthMeter";
import ConnectionStatus from "./ConnectionStatus";

/* views: [{ id, label }], in the order shown. active: the id of the one on show. */
export default function Sidebar({ views, active, onNavigate }) {
  return (
    <div style={{width:200,background:"var(--bg-panel)",borderRight:"1px solid var(--border)",display:"flex",flexDirection:"column"}}>
      <div style={{padding:"20px 16px",borderBottom:"1px solid var(--border)"}}>
        <div style={{fontSize:18,fontWeight:700,letterSpacing:2,color:"var(--green)"}}>FLUX<span style={{color:"#fff"}}>DM</span></div>
      </div>
      <nav aria-label="Main" style={{padding:"12px 8px",flex:1}}>
        {views.map(({id,label})=><NavItem key={id} id={id} label={label} active={active===id} onClick={()=>onNavigate(id)} />)}
      </nav>
      <BandwidthMeter />
      <ConnectionStatus />
    </div>
  );
}
