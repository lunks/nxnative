"""dmsh_build.py's forked workers (dm/gfx/tools/dmsh_workers.py): results in item order from more than one process, counters added back, a task's exception raised in the parent, a dead worker
reported instead of hanging, and nothing a worker inherits written or cleaned up twice. No game files."""
import collections
import concurrent.futures.process
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_workers  # noqa: E402


class Workers(unittest.TestCase):
    def test_results_come_in_item_order_from_several_processes(self):
        def task(x):
            time.sleep(0.02 if x % 3 else 0.05)   # later items finish first
            return x * 2, os.getpid()
        out = dmsh_workers.run(task, range(24), 4)
        self.assertEqual([r[0] for r in out], [x * 2 for x in range(24)])
        pids = {r[1] for r in out}
        self.assertGreater(len(pids), 1); self.assertNotIn(os.getpid(), pids)

    def test_no_items_start_no_worker(self):
        self.assertEqual(dmsh_workers.run(self.fail, [], 4), [])

    def test_counters_are_added_back_without_counting_twice(self):
        a, b = collections.Counter(already=1), collections.Counter()
        def task(x):
            a['seen'] += 1; a[('tuple', 'key')] += x; b[x % 2] += 1
            return x
        dmsh_workers.run(task, range(10), 3, (a, b))
        self.assertEqual(a, {'already': 1, 'seen': 10, ('tuple', 'key'): 45})
        self.assertEqual(b, {0: 5, 1: 5})

    def test_a_task_changes_nothing_but_its_result_in_the_parent(self):
        state = {}
        def task(x): state[x] = True; return x
        dmsh_workers.run(task, range(4), 2)
        self.assertEqual(state, {})

    def test_an_exception_in_a_task_is_raised_here(self):
        def task(x):
            if x == 3: raise ValueError('shader 3')
            return x
        with self.assertRaisesRegex(ValueError, 'shader 3'): dmsh_workers.run(task, range(8), 2)

    def test_a_worker_that_dies_is_an_error_not_a_hang(self):
        with self.assertRaises(concurrent.futures.process.BrokenProcessPool): dmsh_workers.run(lambda x: os._exit(1), range(4), 2)

    def test_more_jobs_than_items(self):
        self.assertEqual(dmsh_workers.run(lambda x: x + 1, [1, 2], 15), [2, 3])

    def test_a_worker_writes_nothing_the_parent_buffered_and_runs_no_cleanup_of_the_parent(self):
        with tempfile.TemporaryDirectory() as d:
            marker = Path(d) / 'cleanups'
            script = Path(d) / 'build.py'
            script.write_text(textwrap.dedent(f'''
                import atexit, sys
                sys.path.insert(0, {str(REPO / 'dm/gfx/tools')!r})
                import dmsh_workers
                atexit.register(lambda: open({str(marker)!r}, 'a').write('x'))
                sys.stdout.write('buffered once\\n')   # not flushed: a pipe is block buffered
                def task(x): print('line', x, flush=True); return x
                dmsh_workers.run(task, range(6), 3)
            '''))
            r = subprocess.run([sys.executable, str(script)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.count('buffered once'), 1)
            self.assertEqual(sorted(ln for ln in r.stdout.splitlines() if ln.startswith('line')), [f'line {i}' for i in range(6)])
            self.assertEqual(marker.read_text(), 'x')


if __name__ == '__main__':
    unittest.main()
