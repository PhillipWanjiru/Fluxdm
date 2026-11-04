/* FluxDM — src/App.jsx */
import React, { useState } from "react";
import Sidebar from "./components/Sidebar/Sidebar";
import DownloadList from "./components/Downloads/DownloadList";
import ConnectionBanner from "./components/Shared/ConnectionBanner";
import { useLiveSync } from "./hooks/useLiveSync";
export default function App() {
  const [section, setSection] = useState("downloads");
  useLiveSync();
  return (
    <div style={{display:"flex",height:"100vh",background:"var(--bg-base)"}}>
      <Sidebar active={section} onNavigate={setSection} />
      <main style={{flex:1,overflow:"hidden",display:"flex",flexDirection:"column"}}>
        <ConnectionBanner />
        <div style={{flex:1,minHeight:0}}><DownloadList /></div>
      </main>
    </div>
  );
}
