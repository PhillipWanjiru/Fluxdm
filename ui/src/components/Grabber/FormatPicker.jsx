/* FluxDM — Grabber/FormatPicker.jsx
 * One of the formats of a page (model/formats.js, formatChoices), chosen with real radio buttons, so the keyboard
 * and screen readers work as they do everywhere: arrow keys move, the label is read. */
import React from "react";

export default function FormatPicker({ choices, selected, onSelect, disabled = false, autoFocus = false, name = "grab-format" }) {
  return (
    <div role="radiogroup" aria-label="Format" style={{ display: "flex", flexDirection: "column", gap: 4 }}>
      {choices.map((choice) => {
        const on = selected === choice.id;
        return (
          <label
            key={choice.id}
            style={{
              display: "flex", alignItems: "center", gap: 10, padding: "8px 12px", borderRadius: 6, fontSize: 11,
              cursor: disabled ? "default" : "pointer", opacity: disabled && !on ? 0.5 : 1,
              background: on ? "rgba(74,244,138,0.1)" : "var(--bg-hover)",
              border: `1px solid ${on ? "var(--green)" : "var(--border)"}`,
            }}
          >
            <input
              type="radio"
              name={name}
              value={choice.id}
              checked={on}
              disabled={disabled}
              onChange={() => onSelect(choice.id)}
              autoFocus={autoFocus && on}
              style={{ accentColor: "var(--green)" }}
            />
            <span style={{ color: on ? "var(--green)" : "var(--text-main)" }}>{choice.label}</span>
            <span style={{ marginLeft: "auto", fontSize: 9, color: "var(--text-dim)" }}>{choice.detail}</span>
          </label>
        );
      })}
    </div>
  );
}
