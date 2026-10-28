/* FluxDM — src/App.jsx */
import React, { useState } from "react";
import Sidebar from "./components/Sidebar/Sidebar";
import DownloadList from "./components/Downloads/DownloadList";
export default function App() {
  const [section, setSection] = useState("downloads");
  return (
    <div style={{display:"flex",height:"100vh",background:"var(--bg-base)"}}>
      <Sidebar active={section} onNavigate={setSection} />
      <main style={{flex:1,overflow:"hidden"}}><DownloadList /></main>
    </div>
  );
}
