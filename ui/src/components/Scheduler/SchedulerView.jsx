/* FluxDM — Scheduler/SchedulerView.jsx */
import React, { useEffect, useState } from "react";
import { getTasks, createTask, deleteTask } from "../../api/scheduler";
import TaskCard from "./TaskCard";
export default function SchedulerView() {
  const [tasks,setTasks] = useState([]);
  useEffect(()=>{ getTasks().then(r=>setTasks(r.data)).catch(()=>{}); },[]);
  return (
    <div style={{padding:24}}>
      <div style={{fontSize:14,fontWeight:700,letterSpacing:1,marginBottom:20,color:"var(--green)"}}>SCHEDULER</div>
      {tasks.length===0&&<div style={{color:"var(--text-dim)",fontSize:11}}>No scheduled tasks.</div>}
      {tasks.map(t=><TaskCard key={t.id} task={t} onDelete={()=>deleteTask(t.id).then(()=>setTasks(ts=>ts.filter(x=>x.id!==t.id)))}/>)}
    </div>
  );
}
