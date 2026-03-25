from __future__ import annotations

import socket
import threading
from typing import Optional


class Channel3Tunnel:
    def __init__(
        self,
        local_host: str = "127.0.0.1",
        local_port: int = 50052,
        host_cid: int = 2,
        host_port: int = 101,
    ) -> None:
        self._local_host = local_host
        self._local_port = local_port
        self._host_cid = host_cid
        self._host_port = host_port
        self._listen_sock: Optional[socket.socket] = None
        self._accept_thread: Optional[threading.Thread] = None
        self._conn_threads: list[threading.Thread] = []
        self._active_sockets: set[socket.socket] = set()
        self._lock = threading.Lock()
        self._running = False

    def start(self) -> None:
        if self._running:
            return

        listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listen_sock.bind((self._local_host, self._local_port))
        listen_sock.listen()

        self._listen_sock = listen_sock
        self._running = True
        self._accept_thread = threading.Thread(target=self._accept_loop, daemon=True)
        self._accept_thread.start()

    def stop(self) -> None:
        if not self._running and self._listen_sock is None:
            return

        self._running = False

        if self._listen_sock is not None:
            try:
                self._listen_sock.close()
            except OSError:
                pass
            self._listen_sock = None

        with self._lock:
            sockets = list(self._active_sockets)
        for sock in sockets:
            try:
                sock.close()
            except OSError:
                pass

        if self._accept_thread is not None:
            self._accept_thread.join(timeout=1.0)
            self._accept_thread = None

        for thread in self._conn_threads:
            thread.join(timeout=1.0)
        self._conn_threads.clear()

    def is_running(self) -> bool:
        return self._running

    def _accept_loop(self) -> None:
        assert self._listen_sock is not None

        while self._running:
            try:
                local_sock, _ = self._listen_sock.accept()
            except OSError:
                break

            thread = threading.Thread(
                target=self._bridge_connection,
                args=(local_sock,),
                daemon=True,
            )
            self._conn_threads.append(thread)
            thread.start()

    def _bridge_connection(self, local_sock: socket.socket) -> None:
        try:
            remote_sock = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
            remote_sock.connect((self._host_cid, self._host_port))
        except OSError:
            try:
                local_sock.close()
            except OSError:
                pass
            return

        self._register_socket(local_sock)
        self._register_socket(remote_sock)

        uplink = threading.Thread(
            target=self._relay,
            args=(local_sock, remote_sock),
            daemon=True,
        )
        downlink = threading.Thread(
            target=self._relay,
            args=(remote_sock, local_sock),
            daemon=True,
        )
        uplink.start()
        downlink.start()
        uplink.join()
        downlink.join()

        self._unregister_socket(local_sock)
        self._unregister_socket(remote_sock)
        for sock in (local_sock, remote_sock):
            try:
                sock.close()
            except OSError:
                pass

    def _relay(self, src: socket.socket, dst: socket.socket) -> None:
        try:
            while self._running:
                data = src.recv(8192)
                if not data:
                    break
                dst.sendall(data)
        except OSError:
            pass
        finally:
            try:
                dst.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    def _register_socket(self, sock: socket.socket) -> None:
        with self._lock:
            self._active_sockets.add(sock)

    def _unregister_socket(self, sock: socket.socket) -> None:
        with self._lock:
            self._active_sockets.discard(sock)
