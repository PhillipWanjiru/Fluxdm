-- FluxDM — 002_scheduler.sql
CREATE TABLE IF NOT EXISTS schedule_tasks (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    download_id  INTEGER REFERENCES downloads(id) ON DELETE CASCADE,
    trigger_at   INTEGER NOT NULL,   -- unix timestamp
    repeat_secs  INTEGER DEFAULT 0,  -- 0 = one-shot
    enabled      INTEGER DEFAULT 1
);
