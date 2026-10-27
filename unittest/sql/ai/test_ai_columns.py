# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0.
"""Additional transactional, concurrency and recovery acceptance tests.

Run on the host with SEEKDB_PORT and AI_MOCK_CONTROL_URL pointing at the isolated
local test container; model URLs resolve inside the database's network namespace.
"""
import json
import os
import subprocess
import time
import unittest
import urllib.request

import pymysql


class AIColumnsTest(unittest.TestCase):
    @classmethod
    def connection(cls, autocommit=True):
        return pymysql.connect(host=os.getenv('SEEKDB_HOST', '127.0.0.1'),
                               port=int(os.getenv('SEEKDB_PORT', '2882')),
                               user='root', charset='utf8mb4', autocommit=autocommit)

    @classmethod
    def setUpClass(cls):
        cls.db = cls.connection()
        cls.other = cls.connection()
        cls.control_url = os.environ['AI_MOCK_CONTROL_URL'].rstrip('/')
        cls.container = os.getenv('SEEKDB_TEST_CONTAINER', 'seekdb-ai-test')
        cls.base = os.getenv('SEEKDB_TEST_BASE_DIR', '/tmp/ai-competition-instance-v3')
        cls.sql('CREATE DATABASE IF NOT EXISTS ai_column_local')
        for name, kind, path, model in [
            ('local_column_model', 'completion', '/v1/chat/completions', 'column-mock'),
            ('local_column_embed', 'dense_embedding', '/v1/embeddings', 'column-embed'),
        ]:
            for method in ['DROP_AI_MODEL_ENDPOINT', 'DROP_AI_MODEL']:
                try:
                    cls.sql('CALL DBMS_AI_SERVICE.%s(%%s)' % method, (name,))
                except pymysql.Error as error:
                    if error.args[0] not in (11112, 11118):
                        raise
            cls.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                    (name, json.dumps({'type': kind, 'model_name': model})))
            cls.sql('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                    (name, json.dumps({'ai_model_name': name, 'provider': 'openai',
                                       'url': 'http://127.0.0.1:8000' + path,
                                       'access_key': 'local-test-key', 'request_model_name': model})))

    @classmethod
    def tearDownClass(cls):
        cls.db.close()
        cls.other.close()

    @classmethod
    def sql(cls, query, args=None, other=False):
        with (cls.other if other else cls.db).cursor() as cursor:
            cursor.execute(query, args)
            return cursor.fetchall()

    @classmethod
    def scalar(cls, query, args=None, other=False):
        return cls.sql(query, args, other)[0][0]

    def control(self, **config):
        request = urllib.request.Request(self.control_url + '/control',
                                         json.dumps(config).encode(), {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.load(response)

    def requests(self):
        with urllib.request.urlopen(self.control_url + '/requests', timeout=5) as response:
            return json.load(response)

    def until(self, predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while True:
            if predicate():
                return
            if time.monotonic() >= deadline:
                self.fail('timed out waiting for asynchronous AI work')
            time.sleep(0.03)

    def tasks(self):
        return self.scalar("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task "
                           "WHERE definition LIKE '%local_column_%'")

    def setUp(self):
        self.db.ping(reconnect=True)
        self.other.ping(reconnect=True)
        self.control()
        self.sql('SET SESSION ai_column_strict_read=OFF')
        self.sql('DROP TABLE IF EXISTS ai_column_local.docs')

    def tearDown(self):
        self.db.rollback()
        self.sql('SET SESSION ai_column_strict_read=OFF')
        self.control()
        self.until(lambda: self.tasks() == 0)

    def table(self, definition=None):
        self.sql('CREATE TABLE ai_column_local.docs(id INT PRIMARY KEY, status VARCHAR(20), '
                 'content VARCHAR(200), ' + (definition or
                 "result VARCHAR(200) AI COLUMN(AI_COMPLETE('local_column_model',content)) "
                 "WITH(max_retries=0)") + ')')

    def ready(self, row_id=1, value='mock:first'):
        self.until(lambda: self.scalar('SELECT result FROM ai_column_local.docs WHERE id=%s', (row_id,)) == value)
        self.until(lambda: self.tasks() == 0)

    def test_transaction_visibility_and_rollback(self):
        self.table()
        self.db.begin()
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.assertEqual(1, self.tasks())
        self.assertEqual(0, self.scalar("SELECT COUNT(*) FROM oceanbase.__all_ai_column_task "
                                       "WHERE definition LIKE '%local_column_%'", other=True))
        self.assertEqual(0, self.scalar('SELECT COUNT(*) FROM ai_column_local.docs', other=True))
        self.db.rollback()
        self.assertEqual(0, self.tasks())
        self.assertEqual([], self.requests())

    def test_ready_null_is_independent_of_result(self):
        self.table("result VECTOR(4) AI COLUMN(AI_EMBED('local_column_embed',content,4)) WITH(max_retries=0)")
        self.sql('INSERT INTO ai_column_local.docs(id,content) VALUES(1,NULL)')
        self.until(lambda: self.tasks() == 0)
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.assertIsNone(self.scalar('SELECT result FROM ai_column_local.docs'))
        self.assertEqual([], self.requests())

    def test_strict_read_cannot_be_bypassed_by_constant_filter(self):
        self.table()
        self.control(block=True)
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.until(lambda: len(self.requests()) == 1)
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.assertEqual(((1,),), self.sql('SELECT id FROM ai_column_local.docs'))
        for query in ['SELECT result FROM ai_column_local.docs WHERE id=999',
                      'SELECT result FROM ai_column_local.docs WHERE 0=1']:
            with self.subTest(query=query), self.assertRaises(pymysql.Error) as raised:
                self.sql(query)
            self.assertEqual(11120, raised.exception.args[0])

    def test_invalid_typed_results_are_terminal(self):
        for result_type, mode in [('JSON', 'invalid_json'), ('BIGINT', 'invalid_integer'), ('VARCHAR(4)', 'invalid_integer')]:
            with self.subTest(result_type=result_type):
                self.sql('DROP TABLE IF EXISTS ai_column_local.docs')
                self.table("result %s AI COLUMN(AI_COMPLETE('local_column_model',content)) WITH(max_retries=0)" % result_type)
                self.control(mode=mode)
                self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
                self.until(lambda: self.tasks() == 0)
                self.assertIsNone(self.scalar('SELECT result FROM ai_column_local.docs'))
                self.sql('SET SESSION ai_column_strict_read=ON')
                with self.assertRaises(pymysql.Error) as raised:
                    self.sql('SELECT result FROM ai_column_local.docs')
                self.assertEqual(11120, raised.exception.args[0])
                self.sql('SET SESSION ai_column_strict_read=OFF')

    def test_insert_ignore_upsert_and_replace(self):
        self.table()
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.ready()
        self.control()
        self.sql("INSERT IGNORE INTO ai_column_local.docs(id,content) VALUES(1,'ignored')")
        self.assertEqual('mock:first', self.scalar('SELECT result FROM ai_column_local.docs'))
        self.assertEqual(0, self.tasks())
        self.assertEqual([], self.requests())
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'second') "
                 "ON DUPLICATE KEY UPDATE content=VALUES(content)")
        self.ready(value='mock:second')
        self.sql("REPLACE INTO ai_column_local.docs(id,content) VALUES(1,'third')")
        self.ready(value='mock:third')

    def test_definition_change_and_drop_cleanup(self):
        self.table()
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.ready()
        self.sql("ALTER TABLE ai_column_local.docs MODIFY result VARCHAR(200) AI COLUMN("
                 "AI_COMPLETE('local_column_model',CONCAT('new:',content))) WITH(max_retries=0)")
        self.ready(value='mock:new:first')
        self.sql('ALTER TABLE ai_column_local.docs DROP COLUMN result')
        self.until(lambda: self.tasks() == 0)
        self.assertEqual(0, self.scalar("SELECT COUNT(*) FROM oceanbase.__all_column c "
                                       "JOIN oceanbase.__all_table t ON c.table_id=t.table_id "
                                       "JOIN oceanbase.__all_database d ON d.database_id=t.database_id "
                                       "WHERE d.database_name='ai_column_local' AND t.table_name='docs' "
                                       "AND c.column_name LIKE '__ai_%'"))

    def test_failed_requests_release_resources(self):
        self.table("result VARCHAR(200) AI COLUMN(AI_COMPLETE('local_column_model',content)) WITH(max_retries=1)")

        def usage():
            text = subprocess.check_output(['docker', 'exec', self.container, 'bash', '-c',
                'pid=$(cat "$1/run/seekdb.pid"); '
                'ls "/proc/$pid/fd" | wc -l; '
                "awk '/VmRSS/ {print $2}' \"/proc/$pid/status\"", '_', self.base], text=True)
            return tuple(map(int, text.split()))

        self.control(mode='disconnect')
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(0,'warmup')")
        self.until(lambda: self.tasks() == 0)
        before_fds, before_rss = usage()
        for start, mode in [(1, 'disconnect'), (9, 'root_array'), (17, 'empty')]:
            self.control(mode=mode)
            with self.db.cursor() as cursor:
                cursor.executemany('INSERT INTO ai_column_local.docs(id,content) VALUES(%s,%s)',
                                   [(i, 'fault:' + str(i)) for i in range(start, start + 8)])
            self.until(lambda: self.tasks() == 0)
            self.assertEqual(16, len(self.requests()))
            self.assertEqual(1, self.scalar('SELECT 1'))
        after_fds, after_rss = usage()
        print('resource check: fd %d -> %d; RSS KiB %d -> %d' %
              (before_fds, after_fds, before_rss, after_rss), flush=True)
        self.assertLessEqual(after_fds - before_fds, 12)
        self.assertLess(after_rss - before_rss, 256 * 1024)
        self.assertEqual(25, self.scalar('SELECT COUNT(*) FROM ai_column_local.docs WHERE result IS NULL'))

    def test_structured_prompt_and_lob_snapshot(self):
        self.sql("CREATE TABLE ai_column_local.docs(id INT PRIMARY KEY, content LONGTEXT, "
                 "result LONGTEXT AI COLUMN(AI_COMPLETE('local_column_model',"
                 "AI_PROMPT('你好 {0} / {0}',content))) WITH(max_retries=0))")
        content = "Unicode 文本 \\ ' \" " * 5000
        self.sql('INSERT INTO ai_column_local.docs(id,content) VALUES(1,%s)', (content,))
        expected = 'mock:你好 ' + content + ' / ' + content
        self.ready(value=expected)
        self.assertEqual(1, len(self.requests()))
        self.assertEqual(expected[5:], self.requests()[0]['body']['messages'][-1]['content'])

    def test_drop_cancels_inflight_tasks(self):
        for drop in ('ALTER TABLE ai_column_local.docs DROP COLUMN result',
                     'DROP TABLE ai_column_local.docs'):
            with self.subTest(drop=drop):
                self.sql('DROP TABLE IF EXISTS ai_column_local.docs')
                self.table()
                self.control(block=True)
                self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
                self.until(lambda: len(self.requests()) == 1)
                self.sql(drop)
                self.assertEqual(0, self.tasks())
                self.control(block=False, clear=False)
                self.until(lambda: all(item.get('done') for item in self.requests()))
                self.assertEqual(1, self.scalar('SELECT 1'))

    def test_delete_reinsert_rejects_late_response(self):
        self.table()
        self.control(block=True, block_text='first')
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.until(lambda: len(self.requests()) == 1)
        self.sql('DELETE FROM ai_column_local.docs WHERE id=1')
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'new')")
        self.ready(value='mock:new')
        self.control(block=False, clear=False)
        self.until(lambda: all(item.get('done') for item in self.requests()))
        self.assertEqual('mock:new', self.scalar('SELECT result FROM ai_column_local.docs'))
        self.sql('SET SESSION ai_column_strict_read=ON')
        self.assertEqual('mock:new', self.scalar('SELECT result FROM ai_column_local.docs'))

    def test_http_timeout_reaches_failed_state(self):
        self.table()
        self.control(block=True)
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.until(lambda: len(self.requests()) == 1)
        self.until(lambda: self.tasks() == 0, timeout=40)
        self.assertEqual(1, len(self.requests()))
        self.assertIsNone(self.scalar('SELECT result FROM ai_column_local.docs'))
        self.sql('SET SESSION ai_column_strict_read=ON')
        with self.assertRaises(pymysql.Error) as raised:
            self.sql('SELECT result FROM ai_column_local.docs')
        self.assertEqual(11120, raised.exception.args[0])

    def test_http_failure_retries_are_bounded(self):
        for status in (429, 500, 503):
            with self.subTest(status=status):
                self.sql('DROP TABLE IF EXISTS ai_column_local.docs')
                self.table("result VARCHAR(200) AI COLUMN(AI_COMPLETE('local_column_model',content)) WITH(max_retries=1)")
                self.control(failures=10, status=status)
                self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
                self.until(lambda: self.tasks() == 0)
                self.assertEqual(2, len(self.requests()))
                self.assertIsNone(self.scalar('SELECT result FROM ai_column_local.docs'))

    def test_graceful_restart_recovers_inflight_task(self):
        self.table()
        self.control(block=True)
        self.sql("INSERT INTO ai_column_local.docs(id,content) VALUES(1,'first')")
        self.until(lambda: len(self.requests()) == 1)
        self.db.close()
        self.other.close()
        started = time.monotonic()
        subprocess.run(['docker', 'exec', self.container, 'bash', '-c',
                        'pid=$(cat "$1/run/seekdb.pid"); kill -TERM "$pid"; '
                        'for i in {1..100}; do state=$(ps -p "$pid" -o stat= || true); '
                        'case "$state" in ""|Z*) exit 0;; esac; sleep 0.05; done; exit 1',
                        '_', self.base], check=True, timeout=10)
        self.assertLess(time.monotonic() - started, 8)
        self.control(block=False, clear=False)
        subprocess.run(['docker', 'exec', '-d', self.container, 'bash', '-c',
                        'exec "$1" --base-dir="$2" --port=2881 --nodaemon > /tmp/ai-recovery-console.log 2>&1',
                        '_', os.environ['SEEKDB_TEST_BINARY'], self.base], check=True)

        def reconnect():
            try:
                self.__class__.db = self.connection()
                self.__class__.other = self.connection()
                return True
            except pymysql.Error:
                return False
        self.until(reconnect, timeout=30)
        self.ready()
        self.assertEqual(2, len(self.requests()))


if __name__ == '__main__':
    unittest.main(verbosity=2)
