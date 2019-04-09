CREATE DATABASE IF NOT EXISTS test;
DROP TABLE IF EXISTS test.table_for_dict;
DROP TABLE IF EXISTS test.table;
DROP DICTIONARY IF EXISTS test.dict1;
DROP DICTIONARY IF EXISTS test.dict_with_ranges;
DROP DICTIONARY IF EXISTS test.dict_with_complex_key;

-- It used for loading in dictionaries via ClickHouse source
CREATE TABLE test.table_for_dict
 (
    first_column UInt8,
    second_column UInt8,
    third_column UInt8,
    fourth_column UInt8
 )
ENGINE = TinyLog;

INSERT INTO TABLE test.table_for_dict VALUES (1, 11, 111, 1111), (2, 22, 222, 2222), (3, 33, 333, 3333);

-- Check that dictionary name doesn't intersect with tables names
CREATE DICTIONARY test.table_for_dict -- { serverError 57 }
 (
    second UInt8 DEFAULT 1
 )
PRIMARY KEY first_column
SOURCE(FILE(PATH '', FORMAT 'TabSeparated'))
LAYOUT(FLAT())
LIFETIME(MIN 1, MAX 10);

CREATE DICTIONARY test.dict1
 (
    second_column UInt8 DEFAULT 1,
    third_column UInt8 DEFAULT 1
 )
PRIMARY KEY first_column
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', PASSWORD '', DB 'test', TABLE 'table_for_dict'))
LAYOUT(FLAT())
LIFETIME(MIN 1, MAX 10);

-- Can't create dictionary if other dictionary with the same name already exists.
CREATE DICTIONARY test.dict1 -- { serverError 447 }
 (
    second UInt8 DEFAULT 1
 )
PRIMARY KEY first_column
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', PASSWORD '', DB 'test', TABLE 'table_for_dict'))
LAYOUT(FLAT())
LIFETIME(MIN 1, MAX 10);

SHOW CREATE DICTIONARY test.dict1;
SHOW CREATE test.dict1; -- { serverError 107 }

SELECT
    name, database, definition_type,
    origin, type, key,
    attribute.names, attribute.types,
    query_count, hit_rate, element_count,
    load_factor, source, last_exception
FROM system.dictionaries WHERE database = 'test';

CREATE TABLE test.table
 (
    first_column UInt64,
    second_column UInt8,
    third_column UInt8
 )
ENGINE = Dictionary(test.dict1);

SHOW CREATE test.table;
SELECT * FROM test.table; -- Here might be a race because of asynchronous loading of values in the dictionary

-- Test Dictionaries with ranges

TRUNCATE TABLE test.table_for_dict;
INSERT INTO TABLE test.table_for_dict VALUES (1, 1, 2, 5), (1, 2, 4, 7), (1, 3, 9, 10), (2, 1, 50, 100), (2, 2, 200, 250);

CREATE DICTIONARY test.dict_with_ranges
 (
    second_column UInt8 DEFAULT 1,
    third_column UInt8 DEFAULT 1,
    fourth_column UInt8 DEFAULT 1
 )
PRIMARY KEY first_column
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', DB 'test', TABLE 'table_for_dict'))
LAYOUT(RANGE_HASHED())
RANGE(MIN third_column, MAX fourth_column)
LIFETIME(MIN 10, MAX 100);

SHOW CREATE DICTIONARY test.dict_with_ranges;

SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(1), 2);
SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(1), 5);
SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(1), 6);
SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(1), 10);
SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(1), 70);
SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(2), 150);
SELECT dictGetUInt8('test.dict_with_ranges', 'second_column', toUInt64(2), 220);

-- Check that dictionary with RANGE_HASHED LAYOUT contains RANGE section in definition

DROP DICTIONARY test.dict_with_ranges;

CREATE DICTIONARY test.dict_with_ranges -- { serverError 448 }
 (
    second_column UInt8 DEFAULT 1
 )
PRIMARY KEY first_column
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', DB 'test', TABLE 'table_for_dict'))
LAYOUT(RANGE_HASHED())
RANGE(MIN second)
LIFETIME(MIN 10, MAX 100);

CREATE DICTIONARY test.dict_with_ranges -- { serverError 36 }
 (
    second_column UInt8 DEFAULT 1
 )
PRIMARY KEY first_column
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', DB 'test', TABLE 'table_for_dict'))
LAYOUT(RANGE_HASHED())
LIFETIME(MIN 10, MAX 100);


-- Check composite key part
DROP DICTIONARY IF EXISTS test.dict_with_complex_key;

-- Check with complex_key_hashed layout
CREATE DICTIONARY test.dict_with_complex_key
 (
    third_column UInt8 DEFAULT 1
 )
COMPOSITE KEY first_column UInt8, second_column UInt8
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', DB 'test', TABLE 'table_for_dict'))
LAYOUT(COMPLEX_KEY_HASHED())
LIFETIME(MIN 10, MAX 100);

SELECT dictGetUInt8('test.dict_with_complex_key', 'third_column', tuple(toUInt8(2), toUInt8(2)));

DROP DICTIONARY IF EXISTS test.dict_with_complex_key;

-- Check with complex_key_cache layout
CREATE DICTIONARY test.dict_with_complex_key
 (
    third_column UInt8 DEFAULT 1
 )
COMPOSITE KEY first_column UInt8, second_column UInt8
SOURCE(CLICKHOUSE(HOST 'localhost', PORT '9000', USER 'default', DB 'test', TABLE 'table_for_dict'))
LAYOUT(COMPLEX_KEY_CACHE(SIZE_IN_CELLS 1000))
LIFETIME(MIN 10, MAX 100);

-- TODO(s-mx): next query get queryCancelation error. bad
-- SELECT dictGetUInt8('test.dict_with_complex_key', 'third_column', tuple(toUInt8(2), toUInt8(2)));

-- TODO(s-mx): add here following checks:
-- 1) ip-trie
-- 2) dict functions
-- 3) ...

DROP TABLE test.table_for_dict;
DROP TABLE test.table;
DROP DICTIONARY test.dict1;
DROP DICTIONARY IF EXISTS test.dict_with_ranges;
DROP DICTIONARY IF EXISTS test.dict_with_complex_key;
