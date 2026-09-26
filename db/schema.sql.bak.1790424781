-- FluxDM — db/schema.sql
-- Master schema applied on first run
--
-- Note: this file is a reference/documentation copy. The daemon does not
-- execute this at startup — Spring's spring.jpa.hibernate.ddl-auto=update
-- generates the live schema directly from the JPA entities in
-- daemon/src/main/java/com/fluxdm/model/. Keep this in sync by hand
-- whenever an entity's fields change.

CREATE TABLE IF NOT EXISTS downloads (
    id            INTEGER PRIMARY KEY,
    url           TEXT    NOT NULL,
    file_path     TEXT    NOT NULL,
    total_size    INTEGER DEFAULT 0,
    downloaded    INTEGER DEFAULT 0,
    segments      INTEGER DEFAULT 8,
    status        INTEGER DEFAULT 0,   -- 0=pending 1=running 2=paused 3=done 4=error
    priority      INTEGER DEFAULT 5,
    checksum_type INTEGER DEFAULT 0,
    checksum      TEXT,
    error_message TEXT,                -- last RSP_ERROR text from fluxdm-core, if any
    created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now')),
    updated_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
);

-- Per-segment progress (individual byte ranges within one download) is
-- NOT currently persisted. fluxdm-core tracks per-segment state
-- in-memory and only reports aggregate progress (total downloaded bytes
-- across all segments) over IPC — there is no per-segment breakdown in
-- the wire protocol today. A `segments` table existed here previously
-- with a matching JPA entity, but neither was ever wired to anything:
-- no repository was written and no code ever created a row. It's
-- removed rather than left as an unused stub. Re-add it (on both the
-- IPC protocol side and here) if a future UI needs to visualize
-- individual segment progress bars rather than just the aggregate.

CREATE TABLE IF NOT EXISTS categories (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT    NOT NULL UNIQUE,
    save_path    TEXT,
    extensions   TEXT    -- comma-separated: .mp4,.mkv
);

CREATE TABLE IF NOT EXISTS settings (
    key          TEXT PRIMARY KEY,
    value        TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_downloads_status ON downloads(status);
