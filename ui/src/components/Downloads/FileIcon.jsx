/* FluxDM — Downloads/FileIcon.jsx */
import React from "react";
const TYPES = {
  iso:     {label:"ISO",bg:"#1a2a3a",color:"#4ab3f4"},
  archive: {label:"ZIP",bg:"#2a1a10",color:"#f4a44a"},
  video:   {label:"VID",bg:"#1a2a1a",color:"#4af48a"},
  pdf:     {label:"PDF",bg:"#2a1a1a",color:"#f44a4a"},
  default: {label:"FILE",bg:"#1a1a2a",color:"#9a8af4"},
};
export default function FileIcon({ type="default" }) {
  const t = TYPES[type] || TYPES.default;
  return (
    <div style={{width:36,height:36,borderRadius:6,background:t.bg,
      display:"flex",alignItems:"center",justifyContent:"center",
      fontFamily:"monospace",fontSize:9,fontWeight:700,color:t.color,
      letterSpacing:0.5,flexShrink:0,border:`1px solid ${t.color}22`}}>
      {t.label}
    </div>
  );
}
