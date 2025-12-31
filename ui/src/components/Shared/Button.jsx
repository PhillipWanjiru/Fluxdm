/* FluxDM — Shared/Button.jsx */
import React from "react";
export default function Button({ children, variant="default", onClick, style={} }) {
  const colors = { default:"var(--text-dim)", primary:"var(--green)", danger:"var(--red)" };
  return (
    <button onClick={onClick} style={{
      padding:"7px 14px",background:"var(--bg-hover)",
      border:`1px solid ${colors[variant]}`,borderRadius:6,
      color:colors[variant],fontSize:11,cursor:"pointer",
      fontFamily:"inherit",...style
    }}>{children}</button>
  );
}
