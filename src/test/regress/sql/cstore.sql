-- create two fake cstore AM instances (no implementation)
CREATE COLUMN STORE ACCESS METHOD store_am_one HANDLER cstore_dummy_handler;
CREATE COLUMN STORE ACCESS METHOD store_am_two HANDLER cstore_dummy_handler;

-- column-level column store definition

-- missing USING clause
CREATE TABLE test_columnar_single_missing (
    a INT,
    b INT COLUMN STORE foo,
    c INT
);

-- missing column store name
CREATE TABLE test_columnar_single_missing (
    a INT,
    b INT COLUMN STORE USING store_am_one,
    c INT
);

-- missing USING and column store name
CREATE TABLE test_columnar_single_missing (
    a INT,
    b INT COLUMN STORE,
    c INT
);

-- missing column store name
CREATE TABLE test_columnar_single_missing (
    a INT,
    b INT COLUMN STORE USING store_am_one,
    c INT
);

-- unknown name of a column store AM
CREATE TABLE test_columnar_single_missing (
    a INT,
    b INT COLUMN STORE foo USING no_store_am,
    c INT
);

-- conflicting column store name
CREATE TABLE test_columnar_single_conflict (
    a INT,
    b INT COLUMN STORE foo USING store_am_one,
    c INT COLUMN STORE foo USING store_am_one,
    d INT
);

-- correct definition (single store) 
CREATE TABLE test_columnar_single_ok (
    a INT,
    b INT COLUMN STORE foo1 USING store_am_one,
    c INT
);

\d test_columnar_single_ok

-- correct definition (two stores)
CREATE TABLE test_columnar_single_ok2 (
    a INT,
    b INT COLUMN STORE foo1 USING store_am_one,
    c INT COLUMN STORE foo2 USING store_am_two,
    d INT
);

\d test_columnar_single_ok2

-- table-level column store definition

-- no column list
CREATE TABLE test_columnar_multi_missing (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo USING store_am_one
);

-- empty column list
CREATE TABLE test_columnar_multi_missing (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo USING store_am_one ()
);

-- invalid column in store
CREATE TABLE test_columnar_multi_missing (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo USING store_am_one (z)
);

-- unknown name of a column store AM
CREATE TABLE test_columnar_multi_missing (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo USING no_store_am (b,c)
);

-- conflicting column store name
CREATE TABLE test_columnar_multi_conflict (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo USING store_am_one (a,b),
    COLUMN STORE foo USING store_am_one (c,d)
);

-- overlapping list of columns
CREATE TABLE test_columnar_multi_conflict2 (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo1 USING store_am_one (a,b),
    COLUMN STORE foo2 USING store_am_two (b,c,d)
);

-- correct definition (single store) 
CREATE TABLE test_columnar_multi_ok (
    a INT,
    b INT,
    c INT,
    COLUMN STORE foo USING store_am_one (a,b)
);

\d test_columnar_multi_ok

-- correct definition (two stores)
CREATE TABLE test_columnar_multi_ok2 (
    a INT,
    b INT,
    c INT,
    d INT,
    COLUMN STORE foo1 USING store_am_one (a,b),
    COLUMN STORE foo2 USING store_am_one (c,d)
);

\d test_columnar_multi_ok2

-- combination of column-level and table-level column stores

-- conflicting column store name
CREATE TABLE test_columnar_multi_conflict (
    a INT,
    b INT COLUMN STORE foo USING store_am_one,
    c INT,
    d INT,
    COLUMN STORE foo USING store_am_one (c,d)
);

-- overlapping list of columns
CREATE TABLE test_columnar_combi_conflict2 (
    a INT,
    b INT COLUMN STORE foo USING store_am_one,
    c INT,
    d INT,
    COLUMN STORE foo2 USING store_am_two (b,c,d)
);

-- correct definition (two stores)
CREATE TABLE test_columnar_combi_ok (
    a INT,
    b INT COLUMN STORE foo USING store_am_one,
    c INT,
    d INT,
    COLUMN STORE foo2 USING store_am_one (c,d)
);

\d test_columnar_combi_ok

-- test cleanup
CREATE TABLE cstore_oids AS
SELECT cststoreid
  FROM pg_cstore JOIN pg_class ON (pg_cstore.cstrelid = pg_class.oid)
 WHERE relname IN ('test_columnar_single_ok',
                   'test_columnar_single_ok2',
                   'test_columnar_multi_ok',
                   'test_columnar_multi_ok2',
                   'test_columnar_combi_ok');

CREATE TABLE cstore_oids_2 AS
SELECT pg_class.oid
  FROM pg_class JOIN cstore_oids ON (pg_class.oid = cstore_oids.cststoreid);

DROP TABLE test_columnar_single_ok;
DROP TABLE test_columnar_single_ok2;
DROP TABLE test_columnar_multi_ok;
DROP TABLE test_columnar_multi_ok2;
DROP TABLE test_columnar_combi_ok;

-- should return 0
SELECT COUNT(*) FROM pg_class WHERE oid IN (SELECT cststoreid FROM cstore_oids);
SELECT COUNT(*) FROM pg_class WHERE oid IN (SELECT oid FROM cstore_oids_2);

SELECT COUNT(*) FROM pg_cstore WHERE cststoreid IN (SELECT oid FROM cstore_oids);

SELECT COUNT(*) FROM pg_attribute WHERE attrelid IN (SELECT cststoreid FROM cstore_oids);
SELECT COUNT(*) FROM pg_attribute WHERE attrelid IN (SELECT oid FROM cstore_oids_2);

DROP TABLE cstore_oids;
DROP TABLE cstore_oids_2;

-- INHERITANCE

-- parent table with two column stores
CREATE TABLE parent_table (
    a INT,
    b INT COLUMN STORE foo1 USING store_am_one,
    c INT,
    d INT,
    e INT,
    COLUMN STORE foo2 USING store_am_two (d,e)
);

-- child table with two separate column stores
CREATE TABLE child_table_1 (
    f INT,
    g INT COLUMN STORE foo1c USING store_am_one,
    h INT,
    i INT,
    COLUMN STORE foo2c USING store_am_two(h,i)
) INHERITS (parent_table);
-- FIXME BUG: should have cstores foo1 and foo2
\d child_table_1

-- child table with two column stores - one modifying, one redefining the parent
CREATE TABLE child_table_2 (
    f INT,
    g INT COLUMN STORE foo1c USING store_am_one, -- new column store
    h INT,
    i INT,
    COLUMN STORE foo2c USING store_am_two(b,h,i) -- redefines the parent colstore
) INHERITS (parent_table);

\d child_table_2

-- child table with a single column store of the whole table
CREATE TABLE child_table_3 (
    f INT,
    g INT,
    h INT,
    i INT,
    COLUMN STORE foo1 USING store_am_one(a,b,c,d,e,f,g,h,i)
) INHERITS (parent_table);

\d child_table_3

--- FIXME -- add tests with multiple inheritance

DROP TABLE parent_table CASCADE;

--- delete the fake cstore AM records
-- FIXME -- this should be a DROP command
DELETE FROM pg_cstore_am WHERE cstamname IN ('store_am_one', 'store_am_two');
