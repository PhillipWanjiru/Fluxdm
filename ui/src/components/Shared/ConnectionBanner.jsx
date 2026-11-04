/* FluxDM — Shared/ConnectionBanner.jsx
 * One line above the views when the UI has lost the daemon, or the daemon has lost its download core (what to say
 * is decided in model/connection.js). It waits a moment before appearing, so a start-up, or a daemon that is being
 * restarted, does not flash it; once it is showing, a change of message shows at once. */
import React, { useEffect, useState } from "react";
import { useSelector } from "react-redux";
import { describeConnection } from "../../model/connection";
import { selectConnection } from "../../store/connectionSlice";

const COLOR = { warning: "var(--amber)", error: "var(--red)" };

export default function ConnectionBanner({ graceMs = 1000 }) {
  const notice = describeConnection(useSelector(selectConnection));
  const message = notice ? notice.message : null;
  const [visible, setVisible] = useState(false);

  useEffect(() => {
    if (!message) {
      setVisible(false);
      return undefined;
    }
    const timer = setTimeout(() => setVisible(true), graceMs);
    return () => clearTimeout(timer);
  }, [message, graceMs]);

  /* The region is always there, empty, so that a screen reader announces what appears in it. */
  return (
    <div role="status" aria-live="polite">
      {visible && notice && (
        <div
          data-kind={notice.kind}
          style={{
            padding: "8px 20px",
            fontSize: 11,
            color: COLOR[notice.kind],
            background: "var(--bg-panel)",
            borderBottom: `1px solid ${COLOR[notice.kind]}`,
          }}
        >
          {notice.message}
        </div>
      )}
    </div>
  );
}
