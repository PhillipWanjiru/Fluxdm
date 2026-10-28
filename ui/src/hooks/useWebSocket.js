/* FluxDM — hooks/useWebSocket.js */
import { useEffect, useRef } from "react";
export function useWebSocket(url, onMessage) {
  const ws = useRef(null);
  useEffect(() => {
    ws.current = new WebSocket(url);
    ws.current.onmessage = e => onMessage(JSON.parse(e.data));
    return () => ws.current?.close();
  }, [url]);
  return ws;
}
