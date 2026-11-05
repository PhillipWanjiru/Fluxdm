/* FluxDM — Grabber/VideoGrabber.jsx
 * Finds the formats of a video page and has the grabber fetch one.
 *
 * Two calls, both slow, both through the daemon to the Python service (api/grabber.js): detecting reads the page, up
 * to a minute; grabbing downloads the whole video before it answers. The Python service does one thing at a time,
 * so while either is under way nothing else is started. Each can be given up on (the page stops waiting), and what
 * went wrong is shown, never swallowed.
 *
 * This view stays mounted when the user looks at another one, so a grab that is under way is not forgotten. */
import React, { useCallback, useEffect, useRef, useState } from "react";
import { detect, grab } from "../../api/grabber";
import GrabModal from "../Modals/GrabModal";
import SiteCard from "./SiteCard";
import { validateUrl } from "../../model/destination";

const Phase = Object.freeze({ IDLE: "idle", DETECTING: "detecting", CHOOSING: "choosing", GRABBING: "grabbing" });

const box = (color, tint) => ({ marginBottom: 16, padding: "10px 12px", background: tint, border: `1px solid ${color}`, borderRadius: 6, color, fontSize: 11, lineHeight: 1.5, wordBreak: "break-word" });

export default function VideoGrabber() {
  const [url, setUrl] = useState("");
  const [phase, setPhase] = useState(Phase.IDLE);
  const [found, setFound] = useState(null); /* what detection found: { address, meta } */
  const [problem, setProblem] = useState(""); /* why the address was not looked up, or the lookup failed */
  const [grabProblem, setGrabProblem] = useState(""); /* why the last grab failed, shown in the dialog */
  const [result, setResult] = useState(null); /* { kind: "saved" | "stopped", title } */
  const request = useRef(null); /* the AbortController of the call under way */

  const stopWaiting = useCallback(() => request.current?.abort(), []);
  useEffect(() => stopWaiting, [stopWaiting]); /* leaving for good: nobody is waiting any more */

  const busy = phase === Phase.DETECTING || phase === Phase.GRABBING;

  const lookUp = async (event) => {
    event.preventDefault();
    if (busy) return;
    const address = url.trim();
    setResult(null);
    if (!address) {
      setProblem("Paste the address of a video page.");
      return;
    }
    const wrong = validateUrl(address);
    if (wrong) {
      setProblem(wrong.message);
      return;
    }
    setProblem("");
    setPhase(Phase.DETECTING);
    const controller = new AbortController();
    request.current = controller;
    try {
      const meta = await detect(address, { signal: controller.signal });
      setFound({ address, meta });
      setGrabProblem("");
      setPhase(Phase.CHOOSING);
    } catch (error) {
      if (error.kind !== "cancelled") setProblem(error.message);
      setPhase(Phase.IDLE);
    } finally {
      if (request.current === controller) request.current = null;
    }
  };

  const fetchFormat = async (formatId) => {
    if (phase !== Phase.CHOOSING) return;
    const { address, meta } = found;
    setGrabProblem("");
    setPhase(Phase.GRABBING);
    const controller = new AbortController();
    request.current = controller;
    try {
      const done = await grab(address, formatId, { signal: controller.signal });
      setResult({ kind: "saved", title: done.title || meta.title || address });
      setFound(null);
      setPhase(Phase.IDLE);
    } catch (error) {
      if (error.kind === "cancelled") {
        setResult({ kind: "stopped", title: meta.title || address });
        setFound(null);
        setPhase(Phase.IDLE);
      } else {
        setGrabProblem(error.message);
        setPhase(Phase.CHOOSING);
      }
    } finally {
      if (request.current === controller) request.current = null;
    }
  };

  const closeDialog = () => { setFound(null); setPhase(Phase.IDLE); };

  return (
    <div style={{ padding: 24, height: "100%", overflowY: "auto" }}>
      <h2 style={{ fontSize: 14, fontWeight: 700, letterSpacing: 1, marginBottom: 20, color: "var(--green)" }}>VIDEO GRABBER</h2>

      <form onSubmit={lookUp} noValidate aria-label="Look up a video page" style={{ marginBottom: 16 }}>
        <label htmlFor="grab-url" style={{ fontSize: 10, color: "var(--text-dim)", letterSpacing: 1, display: "block", marginBottom: 6 }}>
          ADDRESS OF A VIDEO PAGE
        </label>
        <div style={{ display: "flex", gap: 10 }}>
          <input
            id="grab-url"
            value={url}
            onChange={(e) => { setUrl(e.target.value); setProblem(""); }}
            placeholder="https://"
            spellCheck={false}
            aria-invalid={Boolean(problem)}
            aria-describedby={problem ? "grab-url-problem" : undefined}
            style={{ flex: 1, background: "var(--bg-panel)", border: `1px solid ${problem ? "var(--red)" : "var(--border)"}`, borderRadius: 6, padding: "10px 14px", color: "#fff", fontSize: 12, fontFamily: "monospace", outline: "none" }}
          />
          <button
            type="submit"
            disabled={busy}
            style={{ padding: "10px 20px", background: "var(--green)", border: "none", borderRadius: 6, color: "#000", fontWeight: 700, cursor: busy ? "wait" : "pointer", opacity: busy ? 0.6 : 1, fontFamily: "monospace" }}
          >
            {phase === Phase.DETECTING ? "DETECTING…" : "DETECT"}
          </button>
        </div>
        {problem && (
          <div id="grab-url-problem" role="alert" style={{ ...box("var(--red)", "rgba(244,74,74,0.08)"), marginTop: 10, marginBottom: 0 }}>
            {problem}
          </div>
        )}
      </form>

      {/* what is going on, for everyone; always there, so that a screen reader announces what appears in it */}
      <div role="status" aria-live="polite">
        {phase === Phase.DETECTING && (
          <div style={box("var(--text-dim)", "var(--bg-panel)")}>
            Reading the page for its formats. This can take up to a minute.{" "}
            <button type="button" onClick={stopWaiting}
              style={{ background: "none", border: "none", padding: 0, color: "var(--green)", cursor: "pointer", fontFamily: "inherit", fontSize: "inherit", textDecoration: "underline" }}>
              Cancel
            </button>
          </div>
        )}
        {result?.kind === "saved" && (
          <div style={box("var(--green)", "rgba(74,244,138,0.06)")}>
            Saved “{result.title}” in the grabber’s download folder (by default ~/Downloads).
          </div>
        )}
        {result?.kind === "stopped" && (
          <div style={box("var(--amber)", "rgba(244,196,74,0.06)")}>
            Stopped waiting for “{result.title}”. The grabber may still finish it: look in its download folder (by default ~/Downloads).
          </div>
        )}
      </div>

      {phase === Phase.IDLE && !result && <SiteCard />}

      {found && (phase === Phase.CHOOSING || phase === Phase.GRABBING) && (
        <GrabModal
          title={found.meta.title}
          site={found.meta.site}
          duration={found.meta.duration}
          formats={found.meta.formats}
          busy={phase === Phase.GRABBING}
          error={grabProblem}
          onGrab={fetchFormat}
          onStopWaiting={stopWaiting}
          onClose={closeDialog}
        />
      )}
    </div>
  );
}
