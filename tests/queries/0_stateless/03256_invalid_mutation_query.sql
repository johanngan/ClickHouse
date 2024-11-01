DROP TABLE IF EXISTS t;
DROP TABLE IF EXISTS t2;

CREATE TABLE t (x int) ENGINE = MergeTree() ORDER BY ();

DELETE FROM t WHERE y in (SELECT y FROM t); -- { serverError 47 }
DELETE FROM t WHERE x in (SELECT y FROM t); -- { serverError 47 }
DELETE FROM t WHERE x IN (SELECT * FROM t2); -- { serverError 60 }
ALTER TABLE t DELETE WHERE x in (SELECT y FROM t); -- { serverError 47 }
ALTER TABLE t UPDATE x = 1 WHERE x IN (SELECT y FROM t); -- { serverError 47 }

DELETE FROM t WHERE x IN (SELECT foo FROM bar) SETTINGS validate_mutation_query = 0;

ALTER TABLE t ADD COLUMN y int;
DELETE FROM t WHERE y in (SELECT y FROM t);

CREATE TABLE t2 (x int) ENGINE = MergeTree() ORDER BY ();
DELETE FROM t WHERE x IN (SELECT * FROM t2);

DROP TABLE t;
DROP TABLE t2;
