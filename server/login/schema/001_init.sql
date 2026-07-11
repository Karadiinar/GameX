-- 001_init.sql
-- Minimal schema for login server auth. Framing only — no migrations tooling yet.

CREATE TABLE IF NOT EXISTS accounts (
    id            SERIAL PRIMARY KEY,
    username      VARCHAR(32) UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_login_at TIMESTAMPTZ
);

-- Convenience seed row for local testing — dev-only credentials.
-- password_hash is a real Argon2id hash (PHC format) of 'changeme',
-- matching the client's hardcoded dev login in client/src/main.cpp.
INSERT INTO accounts (username, password_hash)
VALUES ('Karadiinar', '$argon2id$v=19$m=65536,t=2,p=1$nzpxLIgOTVXBK2qTF+Rf0A$NgoFiZdY42FigxLa/NIDGAz3I8q41Zn5sLcjVShg/aw')
ON CONFLICT (username) DO NOTHING;
