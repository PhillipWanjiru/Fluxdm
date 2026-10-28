/* FluxDM — hooks/useSpeedData.js */
import { useState, useEffect } from "react";
export function useSpeedData(windowSize = 30) {
  const [data, setData] = useState(() => Array.from({length:windowSize},(_,i)=>({t:i,speed:0})));
  useEffect(() => {
    const id = setInterval(() => {
      setData(prev => [...prev.slice(1), { t: prev[prev.length-1].t+1, speed: Math.random()*20 }]);
    }, 500);
    return () => clearInterval(id);
  }, []);
  return data;
}
