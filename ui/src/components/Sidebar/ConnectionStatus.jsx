/* FluxDM — Sidebar/ConnectionStatus.jsx
 * Whether the daemon is there, in a few words that are always shown (the banner above the views, which says more,
 * appears only when something is wrong). Colour is never all there is to it: the words say the same. The sentence
 * the banner would show is the tooltip. */
import React from "react";
import { useSelector } from "react-redux";
import { summarizeConnection } from "../../model/connection";
import { selectConnection } from "../../store/connectionSlice";

const COLOR = { ok: "var(--green)", warning: "var(--amber)", error: "var(--red)" };

export default function ConnectionStatus() {
  const { kind, label, message } = summarizeConnection(useSelector(selectConnection));
  return (
    <div style={{padding:"12px 14px",borderTop:"1px solid var(--border)"}}>
      <div style={{fontSize:9,color:"var(--text-dim)",marginBottom:6}}>DAEMON</div>
      <div data-testid="daemon-status" data-kind={kind} title={message} style={{display:"flex",alignItems:"center",gap:8,fontSize:10,color:COLOR[kind]}}>
        <span aria-hidden="true" style={{width:7,height:7,borderRadius:"50%",background:COLOR[kind],flexShrink:0}} />
        <span>{label}</span>
      </div>
    </div>
  );
}
