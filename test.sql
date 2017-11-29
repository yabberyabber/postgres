--No index heap
create table noIndex as select s, md5(random()::text) from generate_Series(1,5000) s; 

--B tree code
create table btIndex as select s, md5(random()::text) from generate_Series(1,5) s; 
CREATE INDEX idx_bt ON btIndex (md5);

--skiplist code
create table slIndex as select s, md5(random()::text) from generate_Series(1,5) s; 
CREATE INDEX idx_sl ON slIndex USING hash(md5);


--clean up
DROP TABLE slIndex;
DROP TABLE btIndex;
DROP TABLE noIndex;