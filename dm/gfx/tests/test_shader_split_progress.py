"""shader_split.py's progress lines: the jobs are counted as they finish, in any order, while the results keep the job order; the loose files do not wait for the archive ranges;
and a large loose file that no reader takes gets a red line before it is scanned whole."""
import concurrent.futures as cf
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import shader_split as split


class PooledProgressTests(unittest.TestCase):
    def test_a_slow_first_job_does_not_hold_the_count(self):
        release = threading.Event()
        def scan(job):   # job 0 is a large archive range: it finishes only after the others
            if job == 0 and not release.wait(10): raise TimeoutError('progress did not report the other 19 jobs')
            return job
        class Output(io.StringIO):
            def write(self, text):
                n = super().write(text)
                if text == 'progress: 19/20': release.set()
                return n
        lines = Output()
        with mock.patch.object(split, 'scan_file', scan), cf.ThreadPoolExecutor(4) as ex, contextlib.redirect_stdout(lines):
            try: results = split.pooled(ex, list(range(20)), 20)
            finally: release.set()   # unblock the worker before the executor joins it if the scan fails
        self.assertEqual(results, list(range(20)))   # the job order, as pool.map gave it
        counts = [l for l in lines.getvalue().split('\n') if l.startswith('progress: ')]
        self.assertIn('progress: 19/20', counts)
        self.assertEqual(counts[-1], 'progress: 20/20')

    def test_batches_and_an_offset_count_every_job_once(self):
        lines = io.StringIO()
        with mock.patch.object(split, 'scan_file', lambda job: job * 2), cf.ThreadPoolExecutor(3) as ex, contextlib.redirect_stdout(lines):
            results = split.pooled(ex, list(range(37)), 40, start=3, chunk=16)
        self.assertEqual(results, [j * 2 for j in range(37)])
        self.assertEqual(lines.getvalue().split('\n')[-2], 'progress: 40/40')   # the 3 jobs before, then all 37 of these

    def test_loose_files_are_counted_while_an_archive_range_runs(self):   # they used to start only once every archive range had finished
        release = threading.Event()
        def scan(job):   # jobs 0 and 1 are archive ranges, 0 a long one; 2.. are loose files
            if job == 0 and not release.wait(10): raise TimeoutError('the loose files waited for the archive range')
            return job
        class Output(io.StringIO):
            def write(self, text):
                n = super().write(text)
                if text == 'progress: 33/34': release.set()
                return n
        lines = Output()
        with mock.patch.object(split, 'scan_file', scan), cf.ThreadPoolExecutor(2) as ex, contextlib.redirect_stdout(lines):
            try: results = split.pooled(ex, list(range(34)), 34, chunk=16, single=2)
            finally: release.set()
        self.assertEqual(results, list(range(34)))
        self.assertIn('progress: 33/34', lines.getvalue().split('\n'))   # every job but the long range: 2 archive ranges and 32 loose files in one count
        self.assertEqual(lines.getvalue().split('\n')[-2], 'progress: 34/34')


class WholeScanTests(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup); self.dir = Path(td.name)

    def scan(self, name, data):
        p = self.dir / name; p.write_bytes(data); out = io.StringIO()
        with mock.patch.object(split, 'WHOLE_RED', 1000), contextlib.redirect_stdout(out): split.scan_file((str(p), name))
        return out.getvalue()

    def test_a_large_file_no_reader_takes_is_named_before_its_scan(self):   # Star Fox's 14.5 GB data.zip, before zip_jobs knew its VZIP header, held one worker for minutes with no hint
        out = self.scan('data.bin', b'VZIP' + bytes(2000))
        self.assertEqual(out.count('[RED]'), 1)
        self.assertRegex(out, r'^\[RED\] /data\.bin: 0\.00 GB scanned whole, by one worker: no archive reader takes it \(head 565a495000000000\)\n$')
        self.assertEqual(self.scan('small.bin', bytes(999)), '')

    def test_a_file_a_reader_takes_is_not_named(self):   # a Next Level Games .data is read through its .dict index, not scanned whole
        (self.dir / 'a.dict').write_bytes(b'')
        self.assertEqual(self.scan('a.data', bytes(2000)), '')


if __name__ == '__main__':
    unittest.main()
