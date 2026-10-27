# Copyright (c) 2025 OceanBase.
# Licensed under the Apache License, Version 2.0.
"""Exercise the production SQL engine against the deterministic AI mock."""
import json
import os
import unittest
import urllib.request

import pymysql


class AIFunctionsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = pymysql.connect(host=os.getenv('SEEKDB_HOST', '127.0.0.1'),
                                 port=int(os.getenv('SEEKDB_PORT', '2881')),
                                 user='root', charset='utf8mb4', autocommit=True)
        cls.mock = os.getenv('AI_MOCK_URL', 'http://127.0.0.1:8000')
        with cls.db.cursor() as cursor:
            cursor.execute('CREATE DATABASE IF NOT EXISTS ai_local_test')
            cursor.execute('USE ai_local_test')
            for name, kind, path, provider in [
                ('local_completion', 'completion', '/v1/chat/completions', 'openai'),
                ('local_embedding', 'dense_embedding', '/v1/embeddings', 'openai'),
                ('local_rerank', 'rerank', '/rerank', 'cohere'),
                ('local_ollama', 'completion', '/api/generate', 'oLlAmA'),
                ('local_ollama_embedding', 'dense_embedding', '/api/embed', 'ollama'),
            ]:
                for statement in ['DROP_AI_MODEL_ENDPOINT', 'DROP_AI_MODEL']:
                    try:
                        cursor.execute('CALL DBMS_AI_SERVICE.%s(%%s)' % statement, (name,))
                    except pymysql.Error as exc:
                        if exc.args[0] not in (11112, 11118):
                            raise
                cursor.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL(%s,%s)',
                               (name, json.dumps({'type': kind, 'model_name': name})))
                cursor.execute('CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT(%s,%s)',
                               (name, json.dumps({'ai_model_name': name, 'url': cls.mock + path,
                                                  'access_key': '' if provider.lower() == 'ollama' else 'local-test-key',
                                                  'provider': provider})))

    @classmethod
    def tearDownClass(cls):
        cls.db.close()

    def setUp(self):
        self.db.ping(reconnect=True)
        self.sql('USE ai_local_test')
        self.control()

    def tearDown(self):
        self.control()

    def control(self, **options):
        request = urllib.request.Request(self.mock + '/control', json.dumps(options).encode(),
                                         {'Content-Type': 'application/json'})
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.load(response)

    def requests(self):
        with urllib.request.urlopen(self.mock + '/requests', timeout=5) as response:
            return json.load(response)

    def sql(self, query, args=None):
        with self.db.cursor() as cursor:
            cursor.execute(query, args)
            return cursor.fetchall()

    def scalar(self, query, args=None):
        try:
            return self.sql(query, args)[0][0]
        except pymysql.Error as error:
            if not error.args or not isinstance(error.args[0], int) or error.args[0] == 0 or error.args[0] >= 2000 and error.args[0] <= 2055:
                raise AssertionError('database connection failed instead of returning a SQL error') from error
            with self.db.cursor() as cursor:
                cursor.execute('SELECT 1')
                assert cursor.fetchone()[0] == 1
            raise

    def complete(self, query, args=None):
        return json.loads(self.scalar(query, args))['text']

    def test_prompt_json_nested_and_repeated(self):
        text = self.complete("SELECT AI_COMPLETE('local_completion', AI_PROMPT('{0}|{0}|{1}|{2}',"
                             "JSON_ARRAY(1,2), JSON_OBJECT('k','v'), AI_PROMPT('你好{0}','世界')))")
        self.assertEqual('[1, 2]|[1, 2]|{"k": "v"}|你好世界', text)
        self.assertEqual(1, len(self.requests()))

    def test_prompt_lob_and_repetition(self):
        self.sql('DROP TABLE IF EXISTS prompt_lob')
        self.sql('CREATE TABLE prompt_lob (id INT PRIMARY KEY, template LONGTEXT, value LONGTEXT)')
        value = '长文本\\\n\"' * 20000
        self.sql('INSERT INTO prompt_lob VALUES (1,%s,%s)', ('{0}{0}{0}', value))
        result = self.complete("SELECT AI_COMPLETE('local_completion',AI_PROMPT(template,value)) FROM prompt_lob")
        self.assertEqual(value * 3, result)

    def test_completion_configs(self):
        self.assertEqual('plain', self.complete("SELECT AI_COMPLETE('local_completion','plain')"))
        self.assertEqual('null config', self.complete("SELECT AI_COMPLETE('local_completion','null config',NULL)"))
        self.complete("SELECT AI_COMPLETE('local_completion','configured',JSON_OBJECT('temperature',0.2,'max_tokens',17))")
        body = self.requests()[-1]['body']
        self.assertEqual(0.2, body['temperature'])
        self.assertEqual(17, body['max_tokens'])

    def test_completion_response_validation(self):
        for mode in ['empty', 'content_object', 'root_array']:
            with self.subTest(mode=mode):
                self.control(mode=mode)
                with self.assertRaises(pymysql.Error) as raised:
                    self.scalar("SELECT AI_COMPLETE('local_completion','invalid')")

    def test_prompt_bad_indices(self):
        for template in ['{1}', '{9999999999999999999999999}', '{-1}', '{}']:
            with self.subTest(template=template), self.assertRaises(pymysql.Error):
                self.scalar("SELECT AI_COMPLETE('local_completion',AI_PROMPT(%s,'x'))", (template,))

    def test_ollama_native_protocols(self):
        self.assertEqual('ollama', self.complete("SELECT AI_COMPLETE('local_ollama','ollama')"))
        request = self.requests()[0]['body']
        self.assertIs(request['stream'], False)
        vector = json.loads(self.scalar("SELECT AI_EMBED('local_ollama_embedding','input',4)"))
        self.assertEqual(4, len(vector))
        self.assertEqual('/api/embed', self.requests()[-1]['path'])

    def test_embedding_dimensions(self):
        for dimension in [1, 4, 8]:
            self.assertEqual(dimension, len(json.loads(self.scalar(
                "SELECT AI_EMBED('local_embedding','input',%s)", (dimension,)))))
        self.assertEqual(4, len(json.loads(self.scalar("SELECT AI_EMBED('local_embedding','input')"))))
        for value in [0, -1, None, 1.5]:
            with self.subTest(value=value), self.assertRaises(pymysql.Error):
                self.scalar("SELECT AI_EMBED('local_embedding','input',%s)", (value,))

    def test_embedding_batch_mapping_null_filter_and_limit(self):
        self.sql('DROP TABLE IF EXISTS embed_rows')
        self.sql('CREATE TABLE embed_rows (id INT PRIMARY KEY, text VARCHAR(100))')
        with self.db.cursor() as cursor:
            cursor.executemany('INSERT INTO embed_rows VALUES (%s,%s)',
                               [(i, None if i % 7 == 0 else '文档-%d' % i) for i in range(150)])
        self.control()
        rows = self.sql("SELECT id, AI_EMBED('local_embedding',text,4) FROM embed_rows "
                        "WHERE id >= 5 ORDER BY id LIMIT 130")
        requests = self.requests()
        self.assertTrue(any(len(x['body']['input']) > 1 for x in requests), requests)
        submitted = [text for req in requests for text in req['body']['input']]
        self.assertNotIn(None, submitted)
        # Compare with a separate scalar call; the mock deliberately returns each batch backwards.
        for i, encoded in rows:
            if i % 7 == 0:
                self.assertIsNone(encoded)
            else:
                self.assertEqual(encoded, self.scalar("SELECT AI_EMBED('local_embedding',%s,4)", ('文档-%d' % i,)))

    def test_embedding_bad_responses(self):
        for mode in ['dimension', 'element', 'missing', 'out_of_range', 'wrong_type', 'root_array']:
            with self.subTest(mode=mode):
                self.control(mode=mode)
                with self.assertRaises(pymysql.Error) as raised:
                    self.scalar("SELECT AI_EMBED('local_embedding','input',4)")

    def test_rerank_global_sort_stability_and_objects(self):
        documents = ['document-%d' % i for i in range(47)]
        ranked = json.loads(self.scalar("SELECT AI_RERANK('local_rerank','query',%s)", (json.dumps(documents),)))
        expected = sorted(range(len(documents)), key=lambda i: (-(sum(documents[i].encode()) % 5), i))
        self.assertEqual(expected, [item['index'] for item in ranked])
        self.assertEqual(3, len(self.requests()))
        objects = [{'id': 1000 + i, 'body': value} for i, value in enumerate(documents)]
        self.control()
        result = json.loads(self.scalar("SELECT AI_RERANK('local_rerank','query',%s,'body')", (json.dumps(objects),)))
        self.assertEqual([objects[i]['id'] for i in expected], [obj['id'] for obj in result])
        self.assertTrue(all('_model_score' in obj for obj in result))
        self.assertEqual(3, len(self.requests()))

    def test_rerank_rejects_incomplete_duplicate_and_out_of_range(self):
        for mode in ['duplicate', 'missing', 'out_of_range']:
            with self.subTest(mode=mode):
                self.control(mode=mode)
                with self.assertRaises(pymysql.Error) as raised:
                    self.scalar("SELECT AI_RERANK('local_rerank','query','[\"a\",\"b\"]')")

    def test_http_retry(self):
        self.control(failures=1, status=429)
        self.assertEqual('retry', self.complete("SELECT AI_COMPLETE('local_completion','retry')"))
        self.assertEqual(2, len(self.requests()))


if __name__ == '__main__':
    unittest.main(verbosity=2)
