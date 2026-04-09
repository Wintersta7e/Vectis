CREATE TABLE users (
    id    SERIAL PRIMARY KEY,
    name  TEXT NOT NULL
);

CREATE VIEW user_names AS
    SELECT name FROM users;
