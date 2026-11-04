/* FluxDM — Shared/SpeedGraph.jsx
 * `data` is [{ t, speed }] with the speed in bytes per second. The scale follows the highest value shown, so the
 * graph is as readable at 200 KB/s as at 80 MB/s, and an idle line stays flat at the bottom. */
import React from "react";
import { AreaChart, Area, YAxis, Tooltip, ResponsiveContainer } from "recharts";
import { formatSpeed } from "../../model/format";

export default function SpeedGraph({ data=[] }) {
  return (
    <div role="img" aria-label="Combined download speed over the last minute">
      <ResponsiveContainer width="100%" height={60}>
        <AreaChart data={data} margin={{top:0,right:0,bottom:0,left:0}}>
          <defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
            <stop offset="5%" stopColor="#4af48a" stopOpacity={0.15}/>
            <stop offset="95%" stopColor="#4af48a" stopOpacity={0}/>
          </linearGradient></defs>
          <YAxis domain={[0, (max) => Math.max(max, 1024)]} hide/>
          <Tooltip contentStyle={{background:"#111",border:"1px solid #2a2a2a",fontSize:10}}
            formatter={(value) => [formatSpeed(value), "speed"]} labelFormatter={() => ""}/>
          <Area type="monotone" dataKey="speed" stroke="#4af48a" strokeWidth={1.5} fill="url(#g)" dot={false} isAnimationActive={false}/>
        </AreaChart>
      </ResponsiveContainer>
    </div>
  );
}
