-- 002_sessions_and_characters.sql

CREATE TABLE IF NOT EXISTS sessions (
    token         CHAR(32) PRIMARY KEY,       -- hex-encoded random bytes
    account_id    INT NOT NULL REFERENCES accounts(id),
    issued_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    expires_at    TIMESTAMPTZ NOT NULL
);

CREATE TABLE IF NOT EXISTS characters (
    id            SERIAL PRIMARY KEY,
    account_id    INT NOT NULL REFERENCES accounts(id),
    name          VARCHAR(32) UNIQUE NOT NULL,
    pos_x         REAL NOT NULL DEFAULT 0,
    pos_y         REAL NOT NULL DEFAULT 0,
    pos_z         REAL NOT NULL DEFAULT 0,
    yaw           REAL NOT NULL DEFAULT 0,
    inventory     JSONB NOT NULL DEFAULT '[]',
    last_saved_at TIMESTAMPTZ
);
