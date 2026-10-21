#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runner regressions using isolated fake Orca children; no disc or external service."""
import importlib.util
import io
import socket
from pathlib import Path
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("doubles_rooms", Path(__file__).with_name("doubles-rooms.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class RunnerTest(unittest.TestCase):
    def test_slow_second_ticket_cannot_swap_role_ports(self):
        registered = threading.Event()

        class StopAfterThirdSpawn(Exception):
            pass

        class Child:
            def __init__(self, registration):
                self.stdin = io.StringIO()
                self.stdout = iter(())
                self.registration = registration

            def poll(self):
                return None

            def wait(self, timeout=None):
                self.registration.join()
                return 0

        def spawn(command, **kwargs):
            role = int(Path(command[command.index("-u") + 1]).name.split("-")[1])
            if role == 2:
                kwargs["stderr"].close()  # This stub never creates the third child.
                self.assertTrue(registered.is_set(), "third role started before the second owned its seat")
                raise StopAfterThirdSpawn()
            log = kwargs["stderr"]

            def register():
                if role == 1:
                    time.sleep(0.2)  # Model a slow ticket response while the next role is ready.
                log.write(f"Orca room: in room test on seat {role + 1}, joining\n")
                log.flush()
                if role == 1:
                    registered.set()

            registration = threading.Thread(target=register)
            registration.start()
            return Child(registration)

        args = SimpleNamespace(site="http://127.0.0.1:1", room="test", hold_expiry=False,
                               roles="0,1,2", input_role=[], blackout_role=[], events=None,
                               defer_role=[], refused_role=[], transport="direct", probe=None,
                               input=None, profile=None, pplus=None, mod_dir=None, binary="fake-orca",
                               disc="unused", keep_logs=None)
        with patch.object(fixture.subprocess, "Popen", side_effect=spawn):
            with self.assertRaises(StopAfterThirdSpawn):
                fixture.run(args)


    def test_blackout_keeps_all_roles_on_the_same_test_compatibility(self):
        class StopAfterEnvironment(Exception):
            pass

        def spawn(command, **kwargs):
            # Even the non-faulted host must have a valid (inactive) blackout config:
            # TestOverridesActive contributes test=true to the compatibility key.
            kwargs["stderr"].close()  # This environment-only stub creates no child.
            self.assertEqual(kwargs["env"].get("ORCA_TEST_BLACKOUT"), "1000000-1000001")
            raise StopAfterEnvironment()

        args = SimpleNamespace(site="http://127.0.0.1:1", room="test", hold_expiry=False,
                               roles="0,1,2", input_role=[], blackout_role=["2=50-56"], events=None,
                               defer_role=[], refused_role=[], transport="direct", probe=None,
                               input=None, profile=None, pplus=None, mod_dir=None, binary="fake-orca",
                               disc="unused", keep_logs=None)
        with patch.object(fixture.subprocess, "Popen", side_effect=spawn):
            with self.assertRaises(StopAfterEnvironment):
                fixture.run(args)


    def test_legacy_pplus_boots_the_launcher_with_the_disc_and_no_local_input(self):
        class StopAfterCommand(Exception):
            pass

        def spawn(command, **kwargs):
            kwargs["stderr"].close()  # Inspect the command without starting an emulator.
            self.assertEqual(kwargs["env"].get("ORCA_PROFILE"), "PPLUS32")
            self.assertEqual(kwargs["env"].get("ORCA_NO_LOCAL_INPUT"), "1")
            self.assertEqual(command[command.index("-e") + 1], "clone/launcher.dol")
            self.assertIn("Dolphin.Core.DefaultISO=brawl.wbfs", command)
            raise StopAfterCommand()

        args = SimpleNamespace(site="http://127.0.0.1:1", room="test", hold_expiry=False,
                               roles="0,1,2", input_role=[], blackout_role=[], events=None,
                               defer_role=[], refused_role=[], transport="direct", probe=None,
                               input=None, profile=None, pplus="clone/launcher.dol", mod_dir=None,
                               binary="fake-orca", disc="brawl.wbfs", keep_logs=None)
        with patch.object(fixture.subprocess, "Popen", side_effect=spawn):
            with self.assertRaises(StopAfterCommand):
                fixture.run(args)


    def test_a_nonreading_peer_cannot_hold_the_room_lock(self):
        sender, reader = socket.socketpair()
        sender.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
        rooms = fixture.Rooms()
        released = threading.Event()

        def send():
            with rooms.lock:
                try:
                    rooms.send((sender, threading.Lock()), {"d": "x" * 65500})
                except OSError:
                    pass
                finally:
                    released.set()

        worker = threading.Thread(target=send, daemon=True)
        worker.start()
        try:
            self.assertTrue(released.wait(2.5), "a nonreading socket kept the whole room locked")
        finally:
            reader.close()
            worker.join(timeout=2)
            sender.close()


if __name__ == "__main__":
    unittest.main()
