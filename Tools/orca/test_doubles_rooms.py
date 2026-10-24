#!/usr/bin/env python3
# Copyright 2026 YouGame
# SPDX-License-Identifier: GPL-2.0-or-later
"""Runner regressions using isolated fake Orca children; no disc or external service."""
import importlib.util
import io
import os
import socket
from pathlib import Path
import threading
import time
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("doubles_rooms", Path(__file__).with_name("doubles-rooms.py"))
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class CoSeatFixtureTest(unittest.TestCase):
    def test_friend_from_queue_uses_its_destination_room_assignment(self):
        text = ("Orca room: in room qhome1 on seat 1, hosting\n"
                "Orca room: in room doubles444 on seat 2, joining\n"
                "Drop-in: plugged into port 2 at frame 1964\n")
        self.assertEqual(fixture.room_assignment(text, "doubles444", True)[1], "2")
        self.assertEqual(fixture.room_assignment(text, "doubles444", False)[1], "1")
        self.assertIsNone(fixture.room_assignment(text, "another444", True))

    def test_fixture_page_adds_only_after_its_own_welcome_and_only_once(self):
        page = fixture.FixturePage("http://127.0.0.1:1", 1, True)
        welcome = {"id": 1, "type": "mpRoom", "me": "Doubles1",
                   "lobby": {"maxLocalPlayers": 2},
                   "participants": [{"connectionId": "Doubles1", "localIndex": 0}]}
        messages = page.receive("yg " + fixture.json.dumps(welcome))
        self.assertEqual(len(messages), 2)
        self.assertEqual(fixture.json.loads(messages[1][3:]),
                         {"push": {"slug": "doubles-fixture", "cmd": "local-add"}})
        self.assertEqual(len(page.receive("yg " + fixture.json.dumps(welcome))), 1)
        solo = fixture.FixturePage("http://127.0.0.1:1", 1, False)
        self.assertEqual(len(solo.receive("yg " + fixture.json.dumps(welcome))), 1)
        pending = fixture.FixturePage("http://127.0.0.1:1", 1, True)
        welcome["participants"] = []
        self.assertEqual(len(pending.receive("yg " + fixture.json.dumps(welcome))), 1)

    def test_ticket_reservations_include_existing_local_seats(self):
        server = fixture.Server(("127.0.0.1", 0))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            rooms = server.rooms
            rooms.admit("co", {"p": "Doubles0"})
            rooms.local_command("co", "Doubles0", {"t": "local-add"})
            site = f"http://127.0.0.1:{server.server_port}"
            fixture.post_fixture(site, "/api/multiplayer/ticket", {
                "dev": "orca-doubles-test", "room": "co", "player_id": "Doubles1"})
            self.assertEqual(rooms.members[("co", "Doubles1")], 2)
            rooms.local_command("co", "Doubles1", {"t": "local-add"})
            with self.assertRaises(fixture.urllib.error.HTTPError) as refused:
                fixture.post_fixture(site, "/api/multiplayer/ticket", {
                    "dev": "orca-doubles-test", "room": "co", "player_id": "Doubles2"})
            self.assertEqual(refused.exception.code, 409)
        finally:
            server.shutdown()
            server.server_close()

    def test_relative_files_remain_valid_after_the_child_changes_directory(self):
        with tempfile.TemporaryDirectory() as work:
            disc = Path(work) / "fixture-disc"
            disc.write_bytes(b"fixture")
            self.assertEqual(fixture.file_argument(os.path.relpath(disc)), str(disc.resolve()))
        self.assertEqual(fixture.file_argument("fake-orca"), "fake-orca")

    def test_only_local_pairs_agree_the_co_seat_capability(self):
        self.assertEqual(fixture.role_caps(0, {0: 2}, None), "caps join leave stats coseats\n")
        self.assertEqual(fixture.role_caps(1, {0: 2}, None), "caps join leave stats\n")
        self.assertEqual(fixture.role_caps(0, {}, "ranked"), "caps join leave stats host results locks\n")

    def test_emulator_working_directory_is_its_isolated_user(self):
        class Inspected(Exception):
            pass
        def spawn(command, **kwargs):
            kwargs["stderr"].close()
            self.assertEqual(str(kwargs["cwd"]), command[command.index("-u") + 1])
            raise Inspected()
        args = SimpleNamespace(site="http://127.0.0.1:1", room="test", hold_expiry=False,
            roles="0,1", input_role=[], blackout_role=[], events=None, defer_role=[], refused_role=[],
            transport="direct", probe=None, input=None, profile=None, pplus=None, mod_dir=None,
            binary="fake-orca", disc="unused", keep_logs=None)
        with patch.object(fixture.subprocess, "Popen", side_effect=spawn):
            with self.assertRaises(Inspected):
                fixture.run(args)

    def test_packet_activity_covers_both_local_ports(self):
        rooms = fixture.Rooms()
        rooms.record("co", "Doubles1", {"k": "p", "s": 2, "c": 1,
                     "p": "0001000000000000", "co": [[3, "0002000000000000"]]})
        self.assertEqual(rooms.diagnostics("co")["input_activity"], {"Doubles1": [2, 3]})

    def test_two_local_seats_keep_one_connection_and_lowest_free_ports(self):
        rooms = fixture.Rooms()
        self.assertIsNone(rooms.admit("co", {"p": "Doubles0"}))
        self.assertIsNone(rooms.local_command("co", "Doubles0", {"t": "local-add"}))
        self.assertIsNone(rooms.admit("co", {"p": "Doubles1"}))
        self.assertIsNone(rooms.local_command("co", "Doubles1", {"t": "local-add"}))
        participants = rooms.roster("co", "Doubles1")["participants"]
        self.assertEqual([(p["connectionId"], p.get("localIndex", 0), p["slot"])
                          for p in participants],
                         [("Doubles0", 0, 0), ("Doubles0", 1, 1),
                          ("Doubles1", 0, 2), ("Doubles1", 1, 3)])
        self.assertEqual(len(rooms.members), 2)
        self.assertEqual(rooms.admit("co", {"p": "Doubles2"}), ({}, 409))
        self.assertIsNone(rooms.local_command("co", "Doubles0", {
            "t": "local-remove", "id": "local:Doubles0:1"}))
        self.assertIsNone(rooms.admit("co", {"p": "Doubles2"}))
        self.assertEqual(rooms.members[("co", "Doubles2")], 1)

    def test_local_arguments_and_queue_refusal(self):
        self.assertEqual(fixture.local_counts(["server", "0=2", "1=2"]), {0: 2, 1: 2})
        for invalid in (["0=3"], ["4=2"], ["0=2", "0=2"]):
            with self.assertRaises(ValueError):
                fixture.local_counts(invalid)
        rooms = fixture.Rooms(queue_mode="casual")
        rooms.admit("co", {"p": "Doubles0"})
        self.assertEqual(rooms.local_command("co", "Doubles0", {"t": "local-add"}), "queue")
        self.assertEqual(len(rooms.roster("co", "Doubles0")["participants"]), 1)


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
