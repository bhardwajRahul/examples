-- V002__messages.sql — durable message storage (spec §10).
-- This migration creates the messages table plus the indexes that make
-- the reservation hot-path (oldest READY by available_at) cheap.

CREATE TABLE messages (
    id                  TEXT PRIMARY KEY,
    queue_name          TEXT NOT NULL,
    original_queue_name TEXT,
    payload_json        TEXT NOT NULL,
    state               INTEGER NOT NULL,
    created_at_ms       INTEGER NOT NULL,
    available_at_ms     INTEGER NOT NULL,
    updated_at_ms       INTEGER NOT NULL,
    attempts            INTEGER NOT NULL DEFAULT 0,
    max_attempts        INTEGER NOT NULL,
    receipt_token       TEXT,
    reserved_until_ms   INTEGER,
    last_error          TEXT,
    dead_lettered_at_ms INTEGER
);

CREATE INDEX idx_messages_available
    ON messages(queue_name, state, available_at_ms, created_at_ms);

CREATE INDEX idx_messages_reserved
    ON messages(state, reserved_until_ms);

CREATE INDEX idx_messages_original_queue
    ON messages(original_queue_name, state);

-- Bump the single-row schema_version to 2. V001 inserted version=1; we
-- update in place rather than INSERT, so the table holds exactly one row.
UPDATE schema_version
SET version = 2,
    applied_at = CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER);