#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

"""Run native modem checks under unsuppressed Valgrind Memcheck."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time

CORE_TESTS = {
    'test_config', 'test_main', 'test_runtime', 'test_log_buffer', 'test_log_target',
    'test_host', 'test_session', 'test_loop', 'test_library', 'test_media_workers',
    'test_sip_signaling',
    'test_native_core', 'test_native_core_debug', 'test_native_dte', 'test_terminal',
    'test_slmodem_adapter', 'test_slmodem_driver', 'test_slmodem_calls',
    'test_slmodem_terminal', 'test_audio_path', 'test_audio_scheduler', 'test_rtp',
    'test_rtp_endpoint', 'test_rtp_receiver', 'test_rtp_stream', 'test_rtp_runtime',
    'test_sip_media_bridge', 'test_sip_rtp',
    'test_recording_files', 'test_ds0_codec', 'test_ds0_probe', 'test_ds0_ppp',
    'test_ds0_datapump', 'test_ds0_sip', 'test_v42_xid', 'test_v42bis_codec',
    'test_lapm_v44_session', 'test_dc_remover', 'test_binary_equalizer',
    'test_v91_control', 'test_v92_cpd',
}
INTEGRATION_CASES = {
    'v21': 'forced V.21 raw ATO1 resume',
    'bell103': 'Bell 103 raw ATO1 resume',
    'v22': 'V.22bis LAPM retrain',
    'bell212': 'Bell 212A LAPM ATO1 resume',
    'v23': 'V.23 raw ATO1 resume',
    'v32': 'V.32bis 14400 LAPM retrain',
    'v34': 'V.34 21600 LAPM retrain',
    'v90': 'V.90 28000 downstream with digital caller',
    'v91': 'automatic V.91 default V44',
    'v92': 'V.92 52000/40000 PCM duplex LAPM rate exchange and retrain with digital caller',
}


def parse_report(path):
    text = path.read_text(errors='replace') if path.exists() else ''
    errors = re.findall(r'ERROR SUMMARY: ([\d,]+) errors', text)
    leaks = {}
    for kind in ['definitely lost', 'indirectly lost', 'possibly lost', 'still reachable']:
        match = re.search(re.escape(kind) + r': ([\d,]+) bytes in ([\d,]+) blocks', text)
        if match:
            leaks[kind] = {'bytes': int(match[1].replace(',', '')),
                           'blocks': int(match[2].replace(',', ''))}
    heap = re.search(r'in use at exit: ([\d,]+) bytes in ([\d,]+) blocks', text)
    return {'errors': sum(int(count.replace(',', '')) for count in errors) if errors else None,
            'heap_at_exit': {'bytes': int(heap[1].replace(',', '')),
                             'blocks': int(heap[2].replace(',', ''))} if heap else None,
            'leaks': leaks,
            'invalid_fd_warnings': text.count('Warning: invalid file descriptor')}


def run_case(repo, output, case, timeout):
    name, binary, arguments, variables = case
    command = ['valgrind', '--tool=memcheck', '--track-origins=yes',
               '--leak-check=full', '--show-leak-kinds=all',
               '--errors-for-leak-kinds=definite,indirect,possible',
               '--error-exitcode=99', '--num-callers=30', '--track-fds=yes',
               '--log-file=' + str(output / (name + '.vg')), str(binary), *arguments]
    digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    started = time.monotonic()
    with (output / (name + '.out')).open('w') as log:
        with subprocess.Popen(command, cwd=repo, env=dict(os.environ, **variables),
                              stdout=log, stderr=subprocess.STDOUT,
                              start_new_session=True) as process:
            try:
                status = process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                # Own the whole check, including any forked test processes.
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait()
                status = 124
    report = parse_report(output / (name + '.vg'))
    record = {'name': name, 'command': command, 'test_environment': variables,
              'binary_sha256': digest,
              'exit': status, 'seconds': round(time.monotonic() - started, 3), **report}
    record['passed'] = status == 0 and report['errors'] == 0 and not report['invalid_fd_warnings']
    (output / (name + '.json')).write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--suite', choices=['core', 'native', 'full'], default='core')
    parser.add_argument('--integration', action='store_true', help='add representative training/data/retrain calls over both codecs')
    parser.add_argument('--test', action='append', default=[], help='select a native test by basename (repeatable)')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--timeout', type=float, default=3600, help='seconds per check; full PCM shards can take hours')
    parser.add_argument('--output', type=Path, required=True, help='a new directory for reports')
    parser.add_argument('--skip-build', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1 or args.timeout <= 0 or not math.isfinite(args.timeout):
        parser.error('jobs and timeout must be positive')
    if shutil.which('valgrind') is None:
        parser.error('valgrind is not installed')
    repo = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    try:
        output.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f'cannot create report directory: {error}')
    if not args.skip_build:
        with (output / 'build.log').open('w') as log:
            for command in [['make', '-j2', 'all'], ['make', '-C', 'tests', '-j2', 'all']]:
                subprocess.run(command, cwd=repo, stdout=log, stderr=subprocess.STDOUT, check=True)
    # Ask the Makefile for registered tests, avoiding obsolete files in build/.
    command = ['make', '-s', '--no-print-directory', '-C', 'tests',
               '--eval=memcheck-list:;@printf "%s\\n" $(TEST_BINARIES)', 'memcheck-list']
    listing = subprocess.check_output(command, cwd=repo, text=True)
    binaries = {Path(line).name: repo / 'tests' / line for line in listing.splitlines() if line}
    unknown = set(args.test) - binaries.keys()
    if unknown:
        parser.error('unknown native tests: ' + ', '.join(sorted(unknown)))
    selected = set(args.test) if args.test else set(binaries)
    if not args.test and args.suite == 'core':
        selected = {name for name in selected if name in CORE_TESTS or name.startswith('test_v44_')}
    cases = []
    for name in sorted(selected):
        binary = binaries[name]
        if name == 'test_fsk_integration':
            if args.suite == 'full' or args.test:
                cases.append(('integration-core', binary, [], {'MODEM_INTEGRATION_SKIP_V34_MATRIX': '1', 'MODEM_INTEGRATION_FILTER': '', 'V34_MATRIX_FILTER': ''}))
                for rate in [2400, 2743, 2800, 3000, 3200, 3429]:
                    cases.append((f'integration-v34-{rate}', binary, [], {'MODEM_INTEGRATION_FILTER': '', 'V34_MATRIX_FILTER': f'at {rate} symbol/s'}))
        elif name == 'test_pcm_call':
            if args.suite == 'full' or args.test:
                cases.extend((f'pcm-call-{shard}', binary, ['--shard', str(shard)], {}) for shard in range(4))
        else:
            cases.append((name, binary, [], {}))
    if args.integration and args.suite != 'full':
        binary = binaries.get('test_fsk_integration')
        if binary is None:
            parser.error('integration binary is unavailable')
        cases.extend(('integration-' + name, binary, [], {'MODEM_INTEGRATION_FILTER': pattern, 'MODEM_INTEGRATION_SKIP_V34_MATRIX': '1', 'V34_MATRIX_FILTER': ''}) for name, pattern in INTEGRATION_CASES.items())
    if not cases:
        parser.error('no checks were selected')
    missing = sorted({str(case[1]) for case in cases if not case[1].is_file()})
    if missing:
        parser.error('test binaries are missing; rebuild first: ' + ', '.join(missing))
    skipped = sorted(CORE_TESTS - binaries.keys()) if args.suite == 'core' and not args.test else []
    for name in skipped:
        print('SKIP ' + name + ' (not available in this build)', flush=True)
    metadata = {'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True).strip(),
                'tracked_changes': subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=repo, text=True).splitlines(),
                'valgrind': subprocess.check_output(['valgrind', '--version'], text=True).strip(),
                'suite': args.suite, 'jobs': args.jobs, 'timeout': args.timeout,
                'skipped': skipped,
                'cases': [case[0] for case in cases]}
    (output / 'run.json').write_text(json.dumps(metadata, indent=2) + '\n')
    records = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(run_case, repo, output, case, args.timeout) for case in cases]
        for future in as_completed(futures):
            record = future.result()
            records.append(record)
            print(('PASS' if record['passed'] else 'FAIL') + ' ' + record['name'], flush=True)
    records.sort(key=lambda r: r['name'])
    (output / 'summary.json').write_text(json.dumps(records, indent=2) + '\n')
    failed = sum(not r['passed'] for r in records)
    print(f'{len(records)} checks; {failed} failed; reports: {output}')
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
