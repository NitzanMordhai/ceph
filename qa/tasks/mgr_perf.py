"""
Sample ceph-mgr cost while the tasks nested under this one run.

Writes a sample stream and a summary to the job archive, and optionally
fails the job when a metric is worse than a threshold, so a long run can be
used to catch mgr regressions.
"""

import errno
import json
import logging
import os
import statistics
import threading
import time

from teuthology import misc
from teuthology.exceptions import CommandFailedError
from teuthology.task import Task

log = logging.getLogger(__name__)

# /proc/<pid>/stat: utime and stime, 0-based, in the fields after comm
PROC_UTIME, PROC_STIME = 11, 12


class MgrPerf(Task):
    """
    Measure ceph-mgr cost under whatever workload runs inside this task.

    Most of this comes from the mgr's own perf counters, which already
    carry per-module CPU and memory: ThreadMonitor refreshes them every
    mgr_module_monitor_interval seconds (5 by default). The whole perf dump
    is stored, so throttles, finishers and the objecter are available at
    analysis time without having to decide up front what matters.

    Every `interval` seconds it records, for the active mgr:
      - the full `perf dump`: per-module cpu_usage, serve_cpu_usage,
        mem_rss_current, notify_avg_usec, cmd_avg_usec, the api cache
        hit/miss counts, and the GIL counters if this build has them
      - `dump_mempools`, for where mgr memory actually sits
      - the prometheus module's per-step collect durations, if enabled
      - optionally the mgr's CPU from /proc on its own host. The per-module
        counters are integer percents, so anything under 1% reads as 0, and
        there is no process-total CPU counter at all. That is the only
        reason this task touches /proc; set proc_stats: false to skip it.

    Output lands in the archive under mgr_perf/:
      samples.json   every sample, for plotting
      summary.json   medians, per-cycle averages, slowest operations
      summary.txt    the same thing, readable

    Config:
      interval        seconds between samples (default 30)
      warmup          seconds to sample but discard, letting the workload
                      settle (default 120)
      prometheus      scrape the prometheus module (default true)
      mempool         collect dump_mempools (default true)
      proc_stats      read mgr CPU from /proc (default true)
      thresholds      fail the job if a summary value exceeds one of these.
                      Keys: collect_total_ms, mgr_rss_mb, mgr_cpu_cores,
                      any collect step name suffixed with _ms.
      dry_run         log threshold breaches instead of failing

    Example, measuring while rados bench runs:

        tasks:
        - install:
        - ceph:
        - mgr_perf:
            interval: 30
            warmup: 120
            thresholds:
              collect_total_ms: 2000
              mgr_rss_mb: 4096
        - radosbench:
            clients: [client.0]
            time: 3600

    Nothing is asserted unless `thresholds` is given, so by default this
    only collects data.
    """

    def setup(self):
        super(MgrPerf, self).setup()
        overrides = self.ctx.config.get('overrides', {})
        misc.deep_merge(self.config, overrides.get('mgr_perf', {}))

        self.interval = int(self.config.get('interval', 30))
        self.warmup = int(self.config.get('warmup', 120))
        self.want_prometheus = bool(self.config.get('prometheus', True))
        self.want_mempool = bool(self.config.get('mempool', True))
        self.want_proc = bool(self.config.get('proc_stats', True))
        self.thresholds = self.config.get('thresholds', {}) or {}
        self.dry_run = bool(self.config.get('dry_run', False))

        first_mon = misc.get_first_mon(self.ctx, None)
        (self.admin_remote,) = self.ctx.cluster.only(first_mon).remotes.keys()

        self.samples = []
        self._stop = threading.Event()
        self._thread = None
        self._prom_url = None
        self._failures = []

    # ---------------------------------------------------------------- helpers

    def _ceph(self, *args, **kwargs):
        return self.admin_remote.sh(['sudo', 'ceph'] + list(args), **kwargs)

    def _ceph_json(self, *args):
        return json.loads(self._ceph(*args, '--format=json'))

    def _active_mgr(self):
        return self._ceph_json('mgr', 'dump').get('active_name') or None

    def _mgr_remote(self, mgr_id):
        try:
            return self.ctx.daemons.get_daemon('mgr', mgr_id).remote
        except Exception:
            return None

    def _tell(self, mgr_id, *args):
        try:
            return json.loads(self._ceph('tell', 'mgr.%s' % mgr_id, *args))
        except (CommandFailedError, ValueError) as e:
            log.warning('mgr_perf: tell mgr.%s %s failed: %s',
                        mgr_id, ' '.join(args), e)
            return None

    def _enable_prometheus(self):
        """Turn the module on if it isn't already, and find its URL."""
        try:
            enabled = self._ceph_json('mgr', 'module', 'ls').get(
                'enabled_modules', [])
            if 'prometheus' not in enabled:
                log.info('mgr_perf: enabling the prometheus module')
                self._ceph('mgr', 'module', 'enable', 'prometheus')
            # the module needs a moment to bind and serve a first scrape
            for _ in range(60):
                url = self._ceph_json('mgr', 'services').get('prometheus')
                if url:
                    return url.rstrip('/') + '/metrics'
                time.sleep(5)
            log.warning('mgr_perf: prometheus never published a service URL')
        except (CommandFailedError, ValueError) as e:
            log.warning('mgr_perf: could not set up prometheus: %s', e)
        return None

    def _scrape_prometheus(self, remote):
        """Cumulative per-step collect seconds, keyed by method name."""
        if not (self._prom_url and remote):
            return None
        try:
            body = remote.sh(['curl', '-s', '--max-time', '60', self._prom_url])
        except CommandFailedError as e:
            log.warning('mgr_perf: scrape failed: %s', e)
            return None
        out = {}
        for line in body.splitlines():
            if not line.startswith('ceph_prometheus_collect_duration_seconds'):
                continue
            try:
                head, value = line.rsplit(' ', 1)
                kind = 'sum' if '_sum{' in head else (
                    'count' if '_count{' in head else None)
                if kind is None:
                    continue
                method = head.split('method="', 1)[1].split('"', 1)[0]
                out.setdefault(method, {})[kind] = float(value)
            except (IndexError, ValueError):
                continue
        return out or None

    def _proc_cpu_seconds(self, remote, mgr_id):
        """Total mgr CPU seconds. No perf counter exposes this."""
        if remote is None:
            return None
        try:
            pids = remote.sh(['pgrep', '-f',
                              'ceph-mgr.*-i[ =]*%s' % mgr_id]).split()
            if not pids:
                return None
            stat = remote.sh(['cat', '/proc/%s/stat' % pids[0]])
            fields = stat[stat.rindex(')') + 2:].split()
            ticks = int(fields[PROC_UTIME]) + int(fields[PROC_STIME])
            hz = int(remote.sh(['getconf', 'CLK_TCK']).strip() or 100)
            return ticks / hz
        except (CommandFailedError, ValueError, IndexError) as e:
            log.warning('mgr_perf: could not read mgr /proc: %s', e)
            return None

    # ------------------------------------------------------------- the sampler

    def _effective_config(self, mgr_id):
        """
        Ask the daemon, not the config store.

        'ceph config get' returns what the mon has, which is the default
        for anything set only in ceph.conf -- and startup options such as
        mgr_subinterpreter_modules are set exactly that way. Recording the
        daemon's own value is what makes a comparison between two jobs
        self-documenting.
        """
        out = {}
        for key in ('mgr_subinterpreter_modules', 'mgr_module_path',
                    'mgr_tick_period', 'mgr_module_monitor_interval',
                    'mgr_map_cache_enabled'):
            value = self._tell(mgr_id, 'config', 'get', key)
            if isinstance(value, dict):
                out.update(value)
        return out

    def _sample_once(self, started_at):
        mgr_id = self._active_mgr()
        if not mgr_id:
            return None
        sample = {
            't': round(time.time() - started_at, 1),
            'mgr': mgr_id,
            'perf': self._tell(mgr_id, 'perf', 'dump') or {},
        }
        if self.want_mempool:
            sample['mempool'] = self._tell(mgr_id, 'dump_mempools')
        remote = self._mgr_remote(mgr_id)
        if self.want_prometheus:
            sample['collect'] = self._scrape_prometheus(remote)
        if self.want_proc:
            sample['cpu_seconds'] = self._proc_cpu_seconds(remote, mgr_id)
        return sample

    def _run(self):
        started_at = time.time()
        while not self._stop.is_set():
            try:
                sample = self._sample_once(started_at)
                if sample:
                    sample['warmup'] = sample['t'] < self.warmup
                    self.samples.append(sample)
            except Exception as e:
                # never let sampling kill the job
                log.warning('mgr_perf: sample failed: %s', e)
            self._stop.wait(self.interval)

    # ------------------------------------------------------- summary helpers

    @staticmethod
    def _avg_pairs(section):
        """{name: avg_usec} for every {sum, avgcount} counter in a section."""
        out = {}
        for name, value in (section or {}).items():
            if isinstance(value, dict) and 'avgcount' in value:
                count = value.get('avgcount', 0)
                if count:
                    out[name] = value.get('sum', 0) / count
        return out

    @staticmethod
    def _to_ms(name, value, sum_is_float):
        """
        Convert an average counter to milliseconds.

        perf_counters.cc dumps a TIME_AVG sum as seconds with nanosecond
        decimals, so it parses as a float; a U64_AVG sum is a plain integer
        in whatever unit the caller passed. That makes the JSON type the
        reliable discriminator, with the name only needed for integers.
        """
        if sum_is_float:
            return value * 1000.0
        lowered = name.lower()
        if lowered.endswith(('_usec', '_us')):
            return value / 1000.0
        if lowered.endswith(('_nsec', '_ns')):
            return value / 1000000.0
        if lowered.endswith(('_msec', '_ms')):
            return value
        return None  # unknown unit, do not guess

    def _slowest_operations(self, first, last, top=15):
        """
        Average latency of every {sum, avgcount} counter in the whole dump,
        differenced across the window so it reflects this run only. Catches
        throttle waits, finisher queues and objecter latency without having
        to name them in advance.
        """
        rows = []
        for section, end in (last.get('perf') or {}).items():
            start = (first.get('perf') or {}).get(section, {})
            for name, value in (end or {}).items():
                if not (isinstance(value, dict) and 'avgcount' in value):
                    continue
                prev = start.get(name) if isinstance(start, dict) else None
                prev = prev if isinstance(prev, dict) else {}
                dc = value.get('avgcount', 0) - prev.get('avgcount', 0)
                ds = value.get('sum', 0) - prev.get('sum', 0)
                if dc <= 0:
                    continue
                ms = self._to_ms(name, ds / dc,
                                 isinstance(value.get('sum'), float))
                if ms is not None:
                    rows.append(['%s.%s' % (section, name), round(ms, 4), dc])
        rows.sort(key=lambda r: -r[1])
        return rows[:top]

    def _module_stats(self, useful):
        """Per-module CPU and latency, from the ThreadMonitor counters."""
        modules = {}
        for sample in useful:
            for section, body in (sample.get('perf') or {}).items():
                if not section.startswith('mgr_module_'):
                    continue
                name = section[len('mgr_module_'):]
                entry = modules.setdefault(name, {})
                for gauge in ('cpu_usage', 'serve_cpu_usage'):
                    if gauge in (body or {}):
                        entry.setdefault(gauge, []).append(body[gauge])
                for key, avg in self._avg_pairs(body).items():
                    entry.setdefault(key, []).append(avg)
        out = {}
        for name, series in modules.items():
            row = {}
            for key, values in series.items():
                if values:
                    row[key] = round(statistics.median(values), 3)
            # only keep modules that did something
            if any(row.get(k) for k in row):
                out[name] = row
        return out

    @staticmethod
    def _collect_per_interval(useful):
        """
        Collect cost per cycle between each pair of consecutive samples.

        The summary differences the ends of the window, which is the right
        average but says nothing about what happened in between. A cluster
        still peering, or a one-off stall, shows up here as a spike and
        would otherwise be quietly folded into the average.
        """
        series = []
        prev = None
        for sample in useful:
            now = sample.get('collect')
            if now and prev:
                total, cycles = 0.0, 0
                for method, end in now.items():
                    start = prev.get(method, {})
                    dc = end.get('count', 0) - start.get('count', 0)
                    ds = end.get('sum', 0) - start.get('sum', 0)
                    if dc > 0:
                        total += ds / dc * 1000.0
                        cycles = max(cycles, dc)
                if cycles:
                    series.append({'t': sample['t'], 'ms': round(total, 3),
                                   'cycles': cycles})
            if now:
                prev = now
        return series

    @staticmethod
    def _process_rss_mb(useful):
        """
        ThreadMonitor writes the whole process RSS into every module's
        mem_rss_current, so any module's value is the mgr's RSS.
        """
        values = []
        for sample in useful:
            for section, body in (sample.get('perf') or {}).items():
                if section.startswith('mgr_module_') and (body or {}).get(
                        'mem_rss_current'):
                    values.append(body['mem_rss_current'] / (1024.0 * 1024.0))
                    break
        return values

    # ------------------------------------------------------------------ report

    def _summarise(self):
        useful = [s for s in self.samples if not s['warmup']]
        if len(useful) < 2:
            return {'error': 'not enough samples after warmup',
                    'samples': len(self.samples)}

        first, last = useful[0], useful[-1]
        span = last['t'] - first['t']
        same_mgr = last['mgr'] == first['mgr']
        summary = {
            'samples': len(useful),
            'discarded_as_warmup': len(self.samples) - len(useful),
            'window_seconds': span,
            'interval': self.interval,
            'mgr_failovers': len({s['mgr'] for s in useful}) - 1,
            'effective_config': self._effective_config(last['mgr']),
        }

        if self.want_proc and span > 0 and same_mgr \
                and first.get('cpu_seconds') is not None \
                and last.get('cpu_seconds') is not None:
            summary['mgr_cpu_cores'] = (
                last['cpu_seconds'] - first['cpu_seconds']) / span

        rss = self._process_rss_mb(useful)
        if rss:
            summary['mgr_rss_mb'] = statistics.median(rss)
            summary['mgr_rss_mb_max'] = max(rss)
            summary['mgr_rss_mb_growth'] = rss[-1] - rss[0]

        modules = self._module_stats(useful)
        if modules:
            summary['modules'] = modules
            summary['modules_by_cpu'] = sorted(
                ((n, m.get('cpu_usage', 0)) for n, m in modules.items()),
                key=lambda kv: -kv[1])[:10]

        # prometheus collect durations are cumulative counters: difference
        # the window ends and divide by the cycles in between, so the figure
        # is per cycle and does not drift with run length
        steps = {}
        if first.get('collect') and last.get('collect'):
            for method, end in last['collect'].items():
                start = first['collect'].get(method, {})
                dc = end.get('count', 0) - start.get('count', 0)
                ds = end.get('sum', 0) - start.get('sum', 0)
                if dc > 0:
                    steps[method] = ds / dc * 1000.0
        if steps:
            summary['collect_steps_ms'] = steps
            summary['collect_total_ms'] = sum(steps.values())
            summary['collect_slowest'] = sorted(
                steps.items(), key=lambda kv: -kv[1])[:10]

        per_interval = self._collect_per_interval(useful)
        if per_interval:
            values = [p['ms'] for p in per_interval]
            median = statistics.median(values)
            summary['collect_per_interval_ms'] = per_interval
            summary['collect_stability'] = {
                'median_ms': round(median, 3),
                'min_ms': round(min(values), 3),
                'max_ms': round(max(values), 3),
                'max_over_median': round(max(values) / median, 2) if median
                else None,
            }
            # a settled cluster stays close to its own median; a big spike
            # means something else was going on and the window average is
            # not describing steady state
            if median and max(values) / median > 3.0:
                summary['collect_stability']['warning'] = (
                    'the window was not steady, so the averages above mix in '
                    'whatever caused the spike')

        mgr_section = (last.get('perf') or {}).get('mgr', {})
        first_mgr = (first.get('perf') or {}).get('mgr', {})
        hits = mgr_section.get('cache_hit')
        misses = mgr_section.get('cache_miss')
        if hits is not None and misses is not None:
            d_hit = hits - first_mgr.get('cache_hit', 0)
            d_miss = misses - first_mgr.get('cache_miss', 0)
            total = d_hit + d_miss
            summary['api_cache'] = {
                'hit': d_hit, 'miss': d_miss,
                'hit_ratio': round(d_hit / total, 3) if total else None}
        # present only when the mgr was built with the GIL counters
        gil = {k: v for k, v in mgr_section.items() if k.startswith('gil_')}
        summary['gil_counters'] = gil or 'absent in this build'

        summary['slowest_operations_ms'] = self._slowest_operations(first, last)

        if last.get('mempool'):
            pools = (last['mempool'].get('mempool', {}) or {}).get('by_pool', {})
            top = sorted(((n, p.get('bytes', 0)) for n, p in pools.items()),
                         key=lambda kv: -kv[1])[:10]
            summary['mempool_top_bytes'] = top
        return summary

    def _check_thresholds(self, summary):
        for key, limit in self.thresholds.items():
            if key in summary:
                value = summary[key]
            elif key.endswith('_ms') and key[:-3] in summary.get(
                    'collect_steps_ms', {}):
                value = summary['collect_steps_ms'][key[:-3]]
            else:
                log.warning('mgr_perf: no value for threshold %s', key)
                continue
            if value > limit:
                self._failures.append(
                    '%s = %.2f, above the limit of %.2f' % (key, value, limit))

    def _write(self, summary):
        if self.ctx.archive is None:
            log.info('mgr_perf: no archive, summary follows\n%s',
                     json.dumps(summary, indent=2))
            return
        outdir = os.path.join(self.ctx.archive, 'mgr_perf')
        try:
            os.makedirs(outdir)
        except OSError as e:
            if e.errno != errno.EEXIST:
                raise
        with open(os.path.join(outdir, 'samples.json'), 'w') as f:
            json.dump(self.samples, f, indent=2)
        with open(os.path.join(outdir, 'summary.json'), 'w') as f:
            json.dump(summary, f, indent=2)

        lines = ['ceph-mgr cost over %s samples, %.0fs window'
                 % (summary.get('samples', 0),
                    summary.get('window_seconds', 0))]
        for key, value in sorted(summary.get('effective_config', {}).items()):
            lines.append('  %-22s %r' % (key, value))
        for key in ('mgr_cpu_cores', 'mgr_rss_mb', 'mgr_rss_mb_max',
                    'mgr_rss_mb_growth', 'collect_total_ms', 'mgr_failovers'):
            if key in summary:
                lines.append('  %-22s %s' % (key, round(summary[key], 3)))
        if summary.get('api_cache'):
            lines.append('  %-22s %s' % ('api_cache', summary['api_cache']))
        if summary.get('collect_stability'):
            lines.append('  %-22s %s' % ('collect_stability',
                                         summary['collect_stability']))
        lines.append('  %-22s %s' % ('gil_counters', summary.get('gil_counters')))
        if summary.get('modules_by_cpu'):
            lines.append('  modules by cpu percent:')
            for name, pct in summary['modules_by_cpu']:
                lines.append('    %-28s %s' % (name, pct))
        for method, ms in summary.get('collect_slowest', []):
            lines.append('  collect %-28s %8.2f ms' % (method, ms))
        if summary.get('slowest_operations_ms'):
            lines.append('  slowest counters this window (ms avg):')
            for name, ms, count in summary['slowest_operations_ms']:
                lines.append('    %-44s %9.3f  n=%s' % (name, ms, count))
        if summary.get('mempool_top_bytes'):
            lines.append('  mempool top bytes:')
            for name, nbytes in summary['mempool_top_bytes']:
                lines.append('    %-28s %s' % (name, nbytes))
        text = '\n'.join(lines)
        with open(os.path.join(outdir, 'summary.txt'), 'w') as f:
            f.write(text + '\n')
        log.info('mgr_perf:\n%s', text)

    # --------------------------------------------------------------- lifecycle

    def begin(self):
        super(MgrPerf, self).begin()
        if self.want_prometheus:
            self._prom_url = self._enable_prometheus()
            log.info('mgr_perf: prometheus url %s', self._prom_url)
        log.info('mgr_perf: sampling every %ss, discarding the first %ss',
                 self.interval, self.warmup)
        self._thread = threading.Thread(target=self._run)
        self._thread.daemon = True
        self._thread.start()

    def end(self):
        super(MgrPerf, self).end()
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=self.interval + 120)
        summary = self._summarise()
        if self.thresholds:
            self._check_thresholds(summary)
        summary['threshold_failures'] = self._failures
        self._write(summary)
        if self._failures:
            message = 'mgr_perf thresholds exceeded: ' + '; '.join(self._failures)
            if self.dry_run:
                log.warning('mgr_perf: %s (dry_run)', message)
            else:
                raise RuntimeError(message)


task = MgrPerf
