-- sqlite3 will deduce the library extension based on the current OS
.load sqlite3_pcre2

CREATE TABLE strings (
    id    INTEGER PRIMARY KEY,
    value TEXT
);

INSERT INTO strings VALUES (1, 'foo');
INSERT INTO strings VALUES (2, 'bar');
INSERT INTO strings VALUES (3, 'baz');

SELECT * FROM strings WHERE REGEXP('^(foo|bar|baz)$', value);
-- 1|foo
-- 2|bar
-- 3|baz

SELECT * FROM strings WHERE IREGEXP('^(FOO|BAR|BAZ)$', value);
-- 1|foo
-- 2|bar
-- 3|baz

-- You can also use this syntax for the REGEXP function,
-- but not IREGEXP:
SELECT * FROM strings WHERE value REGEXP 'foo';
-- 1|foo

SELECT REGEXP_INFO('cache_in_use');
-- 2
SELECT IREGEXP_INFO('cache_in_use');
-- 1
