/* FluxDM — Shared/StatCard.jsx */
import React from "react";
export default function StatCard({ label, value, color="var(--green)" }) {
  return (
    <div style={{background:"var(--bg-panel)",border:"1px solid var(--border)",borderRadius:6,padding:"10px 14px"}}>
      <div style={{fontSize:9,color:"var(--text-dim)",letterSpacing:1,marginBottom:4}}>{label}</div>
      <div style={{fontSize:20,fontWeight:700,color}}>{value}</div>
    </div>
  );
}
