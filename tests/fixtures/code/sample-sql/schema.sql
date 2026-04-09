-- Minimal sample schema for Vectis Code mode testing.

CREATE TABLE users (
    id         SERIAL PRIMARY KEY,
    email      TEXT NOT NULL UNIQUE,
    name       TEXT NOT NULL,
    created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE sessions (
    id      SERIAL PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id),
    token   TEXT NOT NULL,
    expires TIMESTAMP NOT NULL
);

CREATE VIEW active_users AS
    SELECT id, email, name
      FROM users
     WHERE created_at > CURRENT_TIMESTAMP - INTERVAL '30 days';
