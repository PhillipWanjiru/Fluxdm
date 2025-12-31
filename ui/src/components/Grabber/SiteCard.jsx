/* FluxDM — Grabber/SiteCard.jsx */
import React from "react";
const SITES = ["YouTube","TikTok","Twitter/X","Instagram","Facebook","1000+ more"];
export default function SiteCard() {
  return (
    <div style={{display:"flex",flexWrap:"wrap",gap:8,marginTop:20}}>
      {SITES.map(s=>(
        <div key={s} style={{padding:"6px 12px",background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,fontSize:10,color:"var(--text-dim)"}}>
          {s}
        </div>
      ))}
    </div>
  );
}
