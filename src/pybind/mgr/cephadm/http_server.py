import threading
from typing import TYPE_CHECKING, Tuple
from cherrypy import _cptree
from cherrypy.process.servers import ServerAdapter
from cherrypy_mgr import CherryPyMgr

from cephadm.agent import AgentEndpoint
from cephadm.services.service_discovery import ServiceDiscovery
from mgr_util import test_port_allocation, PortAlreadyInUse
from orchestrator import OrchestratorError

if TYPE_CHECKING:
    from cephadm.module import CephadmOrchestrator


class CephadmHttpServer(threading.Thread):
    def __init__(self, mgr: "CephadmOrchestrator") -> None:
        self.mgr = mgr
        self.agent = AgentEndpoint(mgr)
        self.service_discovery = ServiceDiscovery(mgr)
        self.cherrypy_shutdown_event = threading.Event()
        self.cherrypy_restart_event = threading.Event()
        self._service_discovery_port = self.mgr.service_discovery_port
        security_enabled, _, _ = self.mgr._get_security_config()
        self.security_enabled = security_enabled
        self.agent_adapter = None
        self.server_adapter = None
        super().__init__(target=self.run)

    def config_update(self) -> None:
        self.service_discovery_port = self.mgr.service_discovery_port
        security_enabled, _, _ = self.mgr._get_security_config()
        if self.security_enabled != security_enabled:
            self.security_enabled = security_enabled
            self.restart()

    @property
    def service_discovery_port(self) -> int:
        return self._service_discovery_port

    @service_discovery_port.setter
    def service_discovery_port(self, value: int) -> None:
        if self._service_discovery_port == value:
            return

        try:
            test_port_allocation(self.mgr.get_mgr_ip(), value)
        except PortAlreadyInUse:
            raise OrchestratorError(f'Service discovery port {value} is already in use. Listening on old port {self._service_discovery_port}.')
        except Exception as e:
            raise OrchestratorError(f'Cannot check service discovery port ip:{self.mgr.get_mgr_ip()} port:{value} error:{e}')

        self.mgr.log.info(f'Changing service discovery port from {self._service_discovery_port} to {value}...')
        self._service_discovery_port = value
        self.restart()

    def restart(self) -> None:
        self.cherrypy_restart_event.set()

    def run(self) -> None:
        def start_servers() -> Tuple[ServerAdapter, ServerAdapter]:
            # start service discovery server
            sd_port = self._service_discovery_port
            sd_ip = self.mgr.get_mgr_ip()
            sd_config, sd_ssl_info = self.service_discovery.configure(
                sd_port,
                sd_ip,
                self.security_enabled
            )
            self.mgr.log.info(f'Starting service discovery server on {sd_ip}:{sd_port}...')

            sd_tree = _cptree.Tree()
            sd_tree.mount(self.service_discovery, "/sd", config=sd_config)
            sd_adapter, _ = CherryPyMgr.mount(
                sd_tree,
                'service-discovery',
                (sd_ip, int(sd_port)),
                ssl_info=sd_ssl_info
            )

            # start agent server
            agent_config, agent_ssl_info, agent_mounts, bind_addr = self.agent.configure()
            self.mgr.log.info(f'Starting agent server on {bind_addr[0]}:{bind_addr[1]}...')

            agent_tree = _cptree.Tree()
            agent_tree.mount(self.agent, "/", config=agent_config)

            for app, path, conf in agent_mounts:
                agent_tree.mount(app, path, config=conf)

            agent_adapter, _ = CherryPyMgr.mount(
                agent_tree,
                'cephadm-agent',
                bind_addr,
                ssl_info=agent_ssl_info
            )
            return sd_adapter, agent_adapter

        try:
            self.server_adapter, self.agent_adapter = start_servers()
            self.mgr.log.info('Cherrypy server started successfully.')
        except Exception as e:
            self.mgr.log.error(f'Failed to start cherrypy server: {e}')
            for adapter in [self.server_adapter, self.agent_adapter]:
                if adapter:
                    adapter.stop()
            return

        while not self.cherrypy_shutdown_event.is_set():
            if self.cherrypy_restart_event.wait(timeout=0.5):
                self.cherrypy_restart_event.clear()
                self.mgr.log.debug('Restarting cherrypy server...')
                for adapter in [self.server_adapter, self.agent_adapter]:
                    if adapter:
                        adapter.stop()
                        adapter.unsubscribe()
                try:
                    self.server_adapter, self.agent_adapter = start_servers()
                    self.mgr.log.debug('Cherrypy server restarted successfully.')
                except Exception as e:
                    self.mgr.log.error(f'Failed to restart cherrypy server: {e}')
                    continue

        for adapter in [self.server_adapter, self.agent_adapter]:
            if adapter:
                adapter.stop()
                adapter.unsubscribe()

    def shutdown(self) -> None:
        self.mgr.log.debug('Stopping cherrypy engine...')
        self.cherrypy_shutdown_event.set()
