#!/usr/bin/env python3
# dmsh_build.py's per-shader work in forked processes instead of threads. Most of it is Python on text: the MSL passes, the reflection, the sources of the side libraries. Threads take turns
# at one interpreter, so a build whose libraries come from the Metal cache ran on one core, whatever J says, and only the Metal compilers ever ran beside it.
# A worker is a fork, so it starts with everything the build holds; what a task changes stays in that worker, so a task gives back its result and each counter it added to (run's counters).
# Forked at a moment with no other thread, and before the pool starts its own. A worker leaves through os._exit: the parent's atexit handlers do not run there, so what a worker makes for itself
# and cleans up at exit stays behind: dmsh_pch's headers are made before the fork.
# Not part of the chain version (dmsh_chain.py), like dmsh_mtlcache.py and dmsh_pch.py: it decides where the work runs, not what it makes.
import concurrent.futures as cf, multiprocessing, sys

_job = None   # (task, items, counters), read by the forked workers


def _start():   # a worker's counters start from nothing: what the parent had counted stays the parent's
    for c in _job[2]: c.clear()


def _one(i):
    task, items, counters = _job
    result = task(items[i])
    added = [dict(c) for c in counters]
    for c in counters: c.clear()
    return result, added


def run(task, items, jobs, counters=()):
    """[task(x) for x in items], in that order, in at most jobs processes, taking the items in order. counters: the collections.Counter objects a task adds to, which end with the workers' counts
    added. An exception in a task is raised here, and a worker that dies raises concurrent.futures.process.BrokenProcessPool"""
    global _job
    items = list(items)
    if not items: return []
    _job = task, items, counters
    sys.stdout.flush(); sys.stderr.flush()   # a worker inherits what is buffered, and would write it again
    results = []
    try:
        with cf.ProcessPoolExecutor(max(1, min(jobs, len(items))), mp_context=multiprocessing.get_context('fork'), initializer=_start) as ex:
            for result, added in ex.map(_one, range(len(items))):
                results.append(result)
                for c, n in zip(counters, added): c.update(n)
    finally: _job = None
    return results
