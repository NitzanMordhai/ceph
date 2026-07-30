
import json
import time
import requests
import errno
import logging

from teuthology.exceptions import CommandFailedError

from .mgr_test_case import MgrTestCase


log = logging.getLogger(__name__)


class TestModuleSelftest(MgrTestCase):
    """
    That modules with a self-test command can be loaded and execute it
    without errors.

    This is not a substitute for really testing the modules, but it
    is quick and is designed to catch regressions that could occur
    if data structures change in a way that breaks how the modules
    touch them.
    """
    MGRS_REQUIRED = 1

    def setUp(self):
        super(TestModuleSelftest, self).setUp()
        self.setup_mgrs()

    def _selftest_plugin(self, module_name):
        self._load_module("selftest")
        self._load_module(module_name)

        # Execute the module's self_test() method
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "module", module_name)

    def _require_mgr_module(self, module_name):
        dump = json.loads(self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "dump", "--format=json-pretty"))
        mgr_map_dump = dump.get("mgrmap", dump)
        # getting the available modules and checking if the module is in the list
        # and if it can run, if not, skip the test
        for entry in mgr_map_dump["available_modules"]:
            if entry.get("name") == module_name:
                if not entry.get("can_run", True):
                    self.skipTest(entry.get("error_string") or
                                  "%s module cannot run" % module_name)
                return
        raise RuntimeError("module %r not found in mgr dump" % module_name)

    def test_prometheus(self):
        self._assign_ports("prometheus", "server_port", min_port=8100)
        self._selftest_plugin("prometheus")

    def test_influx(self):
        self._require_mgr_module("influx")
        self._selftest_plugin("influx")

    def test_diskprediction_local(self):
        self._load_module("selftest")
        python_version = self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "self-test", "python-version")
        if tuple(int(v) for v in python_version.split('.')) == (3, 8):
            # https://tracker.ceph.com/issues/45147
            self.skipTest(f'python {python_version} not compatible with '
                          'diskprediction_local')
        self._selftest_plugin("diskprediction_local")

    def test_telegraf(self):
        self._selftest_plugin("telegraf")

    def test_iostat(self):
        self._selftest_plugin("iostat")

    def test_devicehealth(self):
        self._selftest_plugin("devicehealth")

    def test_selftest_run(self):
        self._load_module("selftest")
        self.mgr_cluster.mon_manager.raw_cluster_cmd("mgr", "self-test", "run")

    def test_telemetry(self):
        self._selftest_plugin("telemetry")

    def test_crash(self):
        self._selftest_plugin("crash")

    def test_orchestrator(self):
        self._selftest_plugin("orchestrator")


    def test_selftest_config_update(self):
        """
        That configuration updates are seen by running mgr modules
        """
        self._load_module("selftest")

        def get_value():
            return self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "config", "get", "testkey").strip()

        self.assertEqual(get_value(), "None")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "config", "set", "mgr", "mgr/selftest/testkey", "foo")
        self.wait_until_equal(get_value, "foo", timeout=10)

        def get_localized_value():
            return self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "config", "get_localized", "testkey").strip()

        self.assertEqual(get_localized_value(), "foo")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "config", "set", "mgr", "mgr/selftest/{}/testkey".format(
                self.mgr_cluster.get_active_id()),
            "bar")
        self.wait_until_equal(get_localized_value, "bar", timeout=10)


    def test_selftest_command_spam(self):
        # Use the selftest module to stress the mgr daemon
        self._load_module("selftest")

        # Use the dashboard to test that the mgr is still able to do its job
        self._assign_ports("dashboard", "ssl_server_port")
        self._load_module("dashboard")
        self.mgr_cluster.mon_manager.raw_cluster_cmd("dashboard",
                                                     "create-self-signed-cert")

        original_active = self.mgr_cluster.get_active_id()
        original_standbys = self.mgr_cluster.get_standby_ids()

        self.mgr_cluster.mon_manager.raw_cluster_cmd("mgr", "self-test",
                                                     "background", "start",
                                                     "command_spam")

        dashboard_uri = self._get_uri("dashboard")

        delay = 10
        periods = 10
        for i in range(0, periods):
            t1 = time.time()
            # Check that an HTTP module remains responsive
            r = requests.get(dashboard_uri, verify=False)
            self.assertEqual(r.status_code, 200)

            # Check that a native non-module command remains responsive
            self.mgr_cluster.mon_manager.raw_cluster_cmd("osd", "df")

            time.sleep(delay - (time.time() - t1))

        self.mgr_cluster.mon_manager.raw_cluster_cmd("mgr", "self-test",
                                                     "background", "stop")

        # Check that all mgr daemons are still running
        self.assertEqual(original_active, self.mgr_cluster.get_active_id())
        self.assertEqual(original_standbys, self.mgr_cluster.get_standby_ids())

    def test_module_commands(self):
        """
        That module-handled commands have appropriate  behavior on
        disabled/failed/recently-enabled modules.
        """

        # Calling a command on a disabled module should return the proper
        # error code.
        self._load_module("selftest")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "module", "disable", "selftest")
        with self.assertRaises(CommandFailedError) as exc_raised:
            self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "run")

        self.assertEqual(exc_raised.exception.exitstatus, errno.EOPNOTSUPP)

        # Calling a command that really doesn't exist should give me EINVAL.
        with self.assertRaises(CommandFailedError) as exc_raised:
            self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "osd", "albatross")

        self.assertEqual(exc_raised.exception.exitstatus, errno.EINVAL)

        # Enabling a module and then immediately using ones of its commands
        # should work (#21683)
        self._load_module("selftest")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "self-test", "config", "get", "testkey")

        # Calling a command for a failed module should return the proper
        # error code.
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "self-test", "background", "start", "throw_exception")
        with self.assertRaises(CommandFailedError) as exc_raised:
            self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "run"
            )
        self.assertEqual(exc_raised.exception.exitstatus, errno.EIO)

        # A health alert should be raised for a module that has thrown
        # an exception from its serve() method
        self.wait_for_health(
            "Module 'selftest' has failed: Synthetic exception in serve",
            timeout=30)
        # prune the crash reports, so that the health report is back to
        # clean
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "crash", "prune", "0")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "module", "disable", "selftest")

        self.wait_for_health_clear(timeout=30)

    def test_module_remote(self):
        """
        Use the selftest module to exercise inter-module communication
        """
        self._require_mgr_module("influx")
        self._load_module("selftest")
        # The "self-test remote" operation just happens to call into
        # influx.
        self._load_module("influx")

        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "self-test", "remote")

    def test_selftest_cluster_log(self):
        """
        Use the selftest module to test the cluster/audit log interface.
        """
        priority_map = {
            "info": "INF",
            "security": "SEC",
            "warning": "WRN",
            "error": "ERR"
        }
        self._load_module("selftest")
        for priority in priority_map.keys():
            message = "foo bar {}".format(priority)
            log_message = "[{}] {}".format(priority_map[priority], message)
            # Check for cluster/audit logs:
            # 2018-09-24 09:37:10.977858 mgr.x [INF] foo bar info
            # 2018-09-24 09:37:10.977860 mgr.x [SEC] foo bar security
            # 2018-09-24 09:37:10.977863 mgr.x [WRN] foo bar warning
            # 2018-09-24 09:37:10.977866 mgr.x [ERR] foo bar error
            with self.assert_cluster_log(log_message):
                self.mgr_cluster.mon_manager.raw_cluster_cmd(
                    "mgr", "self-test", "cluster-log", "cluster",
                    priority, message)
            with self.assert_cluster_log(log_message, watch_channel="audit"):
                self.mgr_cluster.mon_manager.raw_cluster_cmd(
                    "mgr", "self-test", "cluster-log", "audit",
                    priority, message)

    def test_selftest_cluster_log_unknown_channel(self):
        """
        Use the selftest module to test the cluster/audit log interface.
        """
        with self.assertRaises(CommandFailedError) as exc_raised:
            self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "cluster-log", "xyz",
                "ERR", "The channel does not exist")
        self.assertEqual(exc_raised.exception.exitstatus, errno.EOPNOTSUPP)

    def test_serve_failure(self):
        """
        That an exception thrown from serve() marks the module failed,
        with a dedicated test instead of a buried assertion (tracker #78786).
        """
        self._load_module("selftest")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "self-test", "background", "start", "throw_exception")

        self.wait_for_health(
            "Module 'selftest' has failed: Synthetic exception in serve",
            timeout=30)

        self.mgr_cluster.mon_manager.raw_cluster_cmd("crash", "prune", "0")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "module", "disable", "selftest")
        self.wait_for_health_clear(timeout=30)

    def test_command_handler_failure(self):
        """
        That an exception thrown from a command handler marks the module
        failed (tracker #78786).
        """
        self._load_module("selftest")

        # The module is still healthy at this point, so this reaches
        # ActivePyModule::handle_command()'s exception path (-EINVAL),
        # not the DaemonServer.cc pre-check gate that rejects commands to
        # an *already*-failed module with -EIO.
        with self.assertRaises(CommandFailedError) as exc_raised:
            self.mgr_cluster.mon_manager.raw_cluster_cmd(
                "mgr", "self-test", "command", "throw")
        self.assertEqual(exc_raised.exception.exitstatus, errno.EINVAL)

        self.wait_for_health(
            "Module 'selftest' has failed: Synthetic exception in "
            "handle_command",
            timeout=30)

        # No crash dump is generated for this path (handle_command's catch
        # doesn't pass a module name to handle_pyerror), so no crash prune
        # is needed here.
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "module", "disable", "selftest")
        self.wait_for_health_clear(timeout=30)

    def test_notify_failure(self):
        """
        That an exception thrown from notify() marks the module failed
        (tracker #78786).
        """
        self._load_module("selftest")
        self.mgr_cluster.set_module_conf("selftest", "notify_throw", "true")

        # Trigger an osd map change so notify_all("osd_map", ...) fires.
        self.mgr_cluster.mon_manager.raw_cluster_cmd("osd", "set", "noout")
        self.mgr_cluster.mon_manager.raw_cluster_cmd("osd", "unset", "noout")

        self.wait_for_health(
            "Module 'selftest' has failed: Synthetic exception in notify",
            timeout=30)

        self.mgr_cluster.mon_manager.raw_cluster_cmd("crash", "prune", "0")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "module", "disable", "selftest")
        self.wait_for_health_clear(timeout=30)

    def test_config_notify_failure(self):
        """
        That an exception thrown from config_notify() marks the module
        failed (tracker #78786). Setting any module config value triggers
        config_notify(), so no separate trigger step is needed here.
        """
        self._load_module("selftest")
        self.mgr_cluster.set_module_conf(
            "selftest", "config_notify_throw", "true")

        self.wait_for_health(
            "Module 'selftest' has failed: Synthetic exception in "
            "config_notify",
            timeout=30)

        self.mgr_cluster.mon_manager.raw_cluster_cmd("crash", "prune", "0")
        self.mgr_cluster.mon_manager.raw_cluster_cmd(
            "mgr", "module", "disable", "selftest")
        self.wait_for_health_clear(timeout=30)


class TestModuleSelftestStandby(MgrTestCase):
    """
    Module-failure scenarios that specifically need a standby mgr to
    exist: shutdown() failures are only reachable via
    StandbyPyModules::shutdown(), invoked during standby->active
    promotion -- ceph mgr module disable never reaches an active module's
    shutdown() (it always respawns the whole daemon instead). See
    tracker #78786.
    """
    MGRS_REQUIRED = 2

    def setUp(self):
        super(TestModuleSelftestStandby, self).setUp()
        self.setup_mgrs()

    def test_module_load_failure(self):
        """
        That a module that fails to import is reported as such, without
        disturbing the active mgr or any other module.
        """
        standby_id = self.mgr_cluster.get_standby_ids()[0]
        original_active = self.mgr_cluster.get_active_id()

        module_path = self.mgr_cluster.get_config(
            "mgr_module_path", service_type="mgr")
        remote = self.mgr_cluster.mgr_daemons[standby_id].remote
        broken_module_dir = "{0}/_qa_broken_module".format(module_path)

        def get_standby_entry():
            mgr_map = self.mgr_cluster.get_mgr_map()
            for entry in mgr_map["standbys"]:
                if entry["name"] == standby_id:
                    return entry
            return None

        try:
            remote.sudo_write_file(
                "{0}/module.py".format(broken_module_dir), "", mkdir=True)
            remote.sudo_write_file(
                "{0}/__init__.py".format(broken_module_dir),
                "raise ImportError('qa synthetic module load failure')\n")

            self.mgr_cluster.mgr_restart(standby_id)
            self.wait_until_true(
                lambda: get_standby_entry() is not None, timeout=60)

            entry = get_standby_entry()
            broken = None
            for m in entry["available_modules"]:
                if m["name"] == "_qa_broken_module":
                    broken = m
                    break
            self.assertIsNotNone(broken)
            self.assertFalse(broken["can_run"])
            self.assertIn(
                "qa synthetic module load failure", broken["error_string"])

            self.assertEqual(
                self.mgr_cluster.get_active_id(), original_active)
            self.wait_for_health_clear(timeout=30)
        finally:
            remote.run(args=["sudo", "rm", "-rf", broken_module_dir])
            self.mgr_cluster.mgr_restart(standby_id)
            self.wait_until_true(
                lambda: standby_id in self.mgr_cluster.get_standby_ids(),
                timeout=60)

    def test_standby_shutdown_throw_marks_failed(self):
        """
        That an exception thrown from a standby module's shutdown() marks
        it failed, visible once it's promoted to active.
        """
        self.mgr_cluster.set_module_conf(
            "selftest", "shutdown_throw", "true")

        original_active = self.mgr_cluster.get_active_id()
        original_standbys = self.mgr_cluster.get_standby_ids()

        self._load_module("selftest")
        self.wait_until_true(
            lambda: set(self.mgr_cluster.get_standby_ids())
            == set(original_standbys),
            timeout=30)

        self.mgr_cluster.mgr_fail(original_active)
        self.wait_until_true(
            lambda: self.mgr_cluster.get_active_id() in original_standbys,
            timeout=30)

        self.wait_for_health(
            "Module 'selftest' has failed: Synthetic exception in shutdown",
            timeout=30)

        self.mgr_cluster.mon_manager.raw_cluster_cmd("crash", "prune", "0")

    def test_standby_shutdown_hang_does_not_block_promotion(self):
        """
        That a standby module's shutdown() hanging forever does not block
        promotion to active forever -- the actual daemon-availability bug
        motivating tracker #78786.
        """
        self.config_set("mgr", "mgr_module_shutdown_timeout", 3)
        self.mgr_cluster.set_module_conf(
            "selftest", "standby_shutdown_hang", "true")

        original_active = self.mgr_cluster.get_active_id()
        original_standbys = self.mgr_cluster.get_standby_ids()

        self._load_module("selftest")
        self.wait_until_true(
            lambda: set(self.mgr_cluster.get_standby_ids())
            == set(original_standbys),
            timeout=30)

        self.mgr_cluster.mgr_fail(original_active)

        # Deliberately tight bound: well under the 30s *default*
        # mgr_module_shutdown_timeout, built from the 3s shrunk timeout
        # plus margin for normal promotion overhead. This only holds
        # because PyModuleRunner::shutdown() bounds the shutdown() call
        # *and* the subsequent thread.join() together.
        self.wait_until_true(
            lambda: self.mgr_cluster.get_active_id() in original_standbys,
            timeout=15)

        self.wait_for_health(
            "Module 'selftest' has failed", timeout=15)
