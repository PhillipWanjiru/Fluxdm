/* FluxDM — Grabber/SiteCard.jsx
 * What to paste: the sites the grabber's extractor (yt-dlp) is best known for. It reads many more. */
import React from "react";

const SITES = ["YouTube","TikTok","Twitter/X","Instagram","Facebook","1000+ more"];

export default function SiteCard() {
  return (
    <div style={{marginTop:24}}>
      <div style={{fontSize:9,color:"var(--text-dim)",letterSpacing:1,marginBottom:8}}>PAGES IT CAN READ, VIA YT-DLP</div>
      <ul aria-label="Sites" style={{display:"flex",flexWrap:"wrap",gap:8}}>
        {SITES.map(s=>(
          <li key={s} style={{listStyle:"none",padding:"6px 12px",background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,fontSize:10,color:"var(--text-dim)"}}>
            {s}
          </li>
        ))}
      </ul>
    </div>
  );
}
