/* FluxDM — src/App.jsx
 * The shell: the sidebar, the banner that says when the daemon is out of reach, and the view on show.
 *
 * The download list stays mounted while another view is on show (hidden, not removed), so it keeps its tab and the
 * speed graph its history. The scheduler and the settings read what they show from the daemon when they open, so they
 * are mounted only while on show. */
import React, { useState } from "react";
import Sidebar from "./components/Sidebar/Sidebar";
import DownloadList from "./components/Downloads/DownloadList";
import SchedulerView from "./components/Scheduler/SchedulerView";
import SettingsView from "./components/Settings/SettingsView";
import ConnectionBanner from "./components/Shared/ConnectionBanner";
import { useBandwidthSync } from "./hooks/useBandwidthSync";
import { useLiveSync } from "./hooks/useLiveSync";

const VIEWS = [
  { id: "downloads", label: "Downloads", Component: DownloadList, keepMounted: true },
  { id: "scheduler", label: "Scheduler", Component: SchedulerView },
  { id: "settings", label: "Settings", Component: SettingsView },
];

export default function App() {
  const [section, setSection] = useState("downloads");
  useLiveSync();
  useBandwidthSync();
  return (
    <div style={{display:"flex",height:"100vh",background:"var(--bg-base)"}}>
      <Sidebar views={VIEWS} active={section} onNavigate={setSection} />
      <main style={{flex:1,overflow:"hidden",display:"flex",flexDirection:"column"}}>
        <ConnectionBanner />
        {VIEWS.map(({ id, Component, keepMounted }) => {
          const shown = id === section;
          if (!shown && !keepMounted) return null;
          return (
            <div key={id} data-view={id} hidden={!shown} style={{flex:1,minHeight:0}}>
              <Component />
            </div>
          );
        })}
      </main>
    </div>
  );
}
