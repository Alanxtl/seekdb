#!/usr/bin/env python3
# Copyright (c) 2026 OceanBase.
# Licensed under the Apache License, Version 2.0.
"""Run the AI acceptance suites against an isolated local Docker instance.

The container must mount this checkout at /workspace, contain the freshly built
seekdb, and provide obclient/mysqltest. Port 8000 belongs to its network namespace.
This runner never changes expected results or exclusions.
"""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
MYSQL_CASES = {
    'ai_function': ['ai_prompt', 'ai_model_ddl', 'ai_model_endpoint_ddl',
                    'ai_model_lower_case_name_mysql0', 'ai_model_lower_case_name_mysql1',
                    'ai_function_enhancement'],
    'ai_column': ['ai_column_ddl', 'ai_column_t1_dml', 'ai_column_worker',
                  'ai_column_retry_strict', 'ai_column_vector_index'],
    'update': ['update_ignore'],
    'delete': ['delete_from_mysql'],
    'geometry': ['geometry_generated_col_mysql'],
    'trx': ['repeatable_read_transaction'],
    'vector_index': ['vector_index_rr_snapshot'],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--container', default='seekdb-ai-test')
    parser.add_argument('--binary', required=True, help='fresh seekdb executable inside the container')
    parser.add_argument('--base-dir', default='/tmp/ai-competition-instance-v3')
    parser.add_argument('--host-port', type=int, default=2882)
    parser.add_argument('--logdir', type=Path, default=ROOT / 'build/ai-acceptance' /
                        datetime.datetime.now().strftime('%Y%m%d-%H%M%S'))
    args = parser.parse_args()
    args.logdir.mkdir(parents=True, exist_ok=True)
    docker = ['docker', 'exec', args.container]
    sdk = '/sdk/u01/obclient/bin/'
    c_logs = '/tmp/ai-acceptance-' + str(os.getpid())
    subprocess.run(docker + ['mkdir', '-p', c_logs], check=True)
    ip = subprocess.check_output(['docker', 'inspect', '-f',
        '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}', args.container], text=True).strip()
    control = 'http://' + ip + ':8000'
    try:
        urllib.request.urlopen(control + '/health', timeout=3).close()
    except OSError:
        subprocess.run(['docker', 'exec', '-d', args.container, 'python3',
            '/workspace/tools/deploy/mysql_test/test_suite/ai_function/include/ai_function_mock.py'], check=True)
        deadline = time.monotonic() + 10
        while True:
            try:
                urllib.request.urlopen(control + '/health', timeout=1).close()
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
    # Existing suite helpers connect as admin to inspect server metadata.
    subprocess.run(docker + [sdk + 'obclient', '--no-defaults', '-h127.0.0.1', '-P2881', '-uroot', '-e',
        "CREATE USER IF NOT EXISTS admin IDENTIFIED BY ''; GRANT ALL PRIVILEGES ON *.* TO admin WITH GRANT OPTION"], check=True)
    with tempfile.NamedTemporaryFile(mode='w') as prologue:
        prologue.write('let $OBMYSQL_MS0 = 127.0.0.1;\nlet $OBMYSQL_PORT = 2881;\nlet $OBMYSQL_PWD = ;\n')
        prologue.flush()
        subprocess.run(['docker', 'cp', prologue.name, args.container + ':' + c_logs + '/prologue.inc'], check=True)
    records = []

    def run(name, command, env=None):
        path = args.logdir / (name + '.log')
        started = time.monotonic()
        with path.open('w') as output:
            result = subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT)
        record = {'name': name, 'exit_code': result.returncode,
                  'seconds': round(time.monotonic() - started, 3), 'log': str(path.resolve())}
        records.append(record)
        print(name + ': ' + ('PASS' if result.returncode == 0 else 'FAIL'), flush=True)

    for suite, cases in MYSQL_CASES.items():
        for case in cases:
            run(suite + '.' + case, ['docker', 'exec', '-w', '/workspace/tools/deploy', args.container,
                sdk + 'mysqltest', '--no-defaults', '-h127.0.0.1', '-P2881', '-uroot', '-Dtest',
                '--prologue=' + c_logs + '/prologue.inc', '--logdir=' + c_logs,
                '--test-file=mysql_test/test_suite/' + suite + '/t/' + case + '.test',
                '--result-file=mysql_test/test_suite/' + suite + '/r/mysql/' + case + '.result', '--tail-lines=50'])
    run('python.ai_functions', ['docker', 'exec', '-e', 'PYTHONPATH=/workspace/build/test-deps', args.container,
        'python3', '/workspace/unittest/sql/ai/test_ai_functions.py'])
    env = dict(os.environ, AI_MOCK_CONTROL_URL=control, SEEKDB_TEST_CONTAINER=args.container,
               SEEKDB_PORT=str(args.host_port), SEEKDB_TEST_BASE_DIR=args.base_dir, SEEKDB_TEST_BINARY=args.binary)
    run('python.ai_columns', ['python3', 'unittest/sql/ai/test_ai_columns.py'], env)
    subprocess.run(['docker', 'cp', args.container + ':' + c_logs, str(args.logdir / 'mysqltest')], check=True)
    digest = subprocess.check_output(docker + ['sha256sum', args.binary], text=True).split()[0]
    report = {'branch': subprocess.check_output(['git', 'branch', '--show-current'], cwd=ROOT, text=True).strip(),
              'base_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
              'binary_sha256': digest, 'container': args.container, 'binary': args.binary,
              'mysqltest_files': sum(map(len, MYSQL_CASES.values())),
              'all_passed': all(record['exit_code'] == 0 for record in records), 'results': records}
    (args.logdir / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['all_passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
