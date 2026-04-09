-- Stored routines for the sample schema.

CREATE OR REPLACE FUNCTION greet(target TEXT)
RETURNS TEXT AS $$
    SELECT 'hello, ' || target;
$$ LANGUAGE SQL IMMUTABLE;

CREATE OR REPLACE FUNCTION find_user_by_email(needle TEXT)
RETURNS TABLE(id INTEGER, name TEXT) AS $$
    SELECT id, name
      FROM users
     WHERE email = needle;
$$ LANGUAGE SQL STABLE;
