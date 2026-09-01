-- V001__init.sql — first migration.
-- Creates the schema_version table that gates all subsequent migrations.
-- The messages table + indexes arrive in V002 (stage 4).

CREATE TABLE schema_version (
    version    INTEGER NOT NULL,
    applied_at INTEGER NOT NULL
);

INSERT INTO schema_version(version, applied_at) VALUES (1, CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER));
