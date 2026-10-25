#!/usr/bin/env python3
"""Installed Flowie::Flowie broker + Flowie::Client -> TurboDB QoS1 Outbox.
All paths/DBs/broker are test-only and loopback; no public MQTT listeners.
"""
import os
import pathlib
import subprocess
import sys
import tempfile
import time

def run(argv, env, want=0, seconds=15):
    action = subprocess.run(argv, env=env, capture_output=True, text=True,
                            timeout=seconds, check=False)
    if action.returncode != want:
        raise AssertionError(
            f"cmd={argv[0]} expected={want} got={action.returncode}\n"
            f"stdout={action.stdout[-2800:]}\nstderr={action.stderr[-2800:]}")
    return action

class Broker:
    def __init__(self, binary, env, folder):
        self.out = open(pathlib.Path(folder) / "broker.log", "w+t", encoding="utf-8")
        self.process = subprocess.Popen(
            [binary], stdin=subprocess.PIPE, stdout=self.out,
            stderr=subprocess.STDOUT, env=env)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("Flowie broker process quit before ready")
            self.out.seek(0)
            if "FLOWIE_BROKER_READY" in self.out.read():
                return
            time.sleep(0.1)
        raise RuntimeError("Flowie broker READY timeout")

    def events(self):
        self.out.flush()
        self.out.seek(0)
        return self.out.read().count("BROKER_EVENT_ACCEPTED")

    def wait_events(self, count):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if self.events() >= count: return
            time.sleep(0.05)
        raise AssertionError(f"Flowie broker saw {self.events()}, expected {count}")

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.communicate(input=b"\n", timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                raise RuntimeError("Flowie broker did not stop")
        self.out.flush()
        self.out.seek(0)
        text = self.out.read()
        self.out.close()
        if self.process.returncode != 0:
            raise RuntimeError("Flowie broker failure: " + text[-2800:])

def main() -> int:
    if len(sys.argv) != 6:
        raise SystemExit("usage: harness broker publisher seed audit sqlite-driver")
    broker_bin, publisher_bin, seed_bin, audit_bin, plugin = (
        str(pathlib.Path(x).resolve()) for x in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="mygames-flowie-outbox-") as root:
        db = str(pathlib.Path(root) / "events.sqlite")
        env = os.environ.copy()
        env["MYGAMES_MQTT_PORT"] = "18893"
        env.pop("MYGAMES_MQTT_USER", None)
        env.pop("MYGAMES_MQTT_PASSWORD", None)
        seed = lambda option: run([seed_bin,plugin,db,option],env)
        audit = lambda expected: run([audit_bin,plugin,db,str(expected)],env)
        publish = lambda option, e=env, want=0: run(
            [publisher_bin,plugin,db,option],e,want=want)
        seed("--seed")
        audit(1)
        broker = Broker(broker_bin, env, root)
        try:
            # Forced process death AFTER observed QoS1 PUBACK but BEFORE
            # committing the Outbox ACK. Broker accepted one event; DB still
            # contains it, so restart must publish the SAME event again.
            crash_env = env.copy()
            crash_env["MYGAMES_TEST_CRASH_AFTER_PUBACK"] = "1"
            publish("--once", e=crash_env, want=77)
            broker.wait_events(1)
            audit(1)
            publish("--once")
            broker.wait_events(2)
            audit(2)
            publish("--drain")
            broker.wait_events(3)
            audit(0)
            publish("--drain")
            assert broker.events() == 3, "empty outbox published extra MQTT packets"
        finally:
            broker.close()

        # Broker outage: no QoS1 settlement and no database ACK.
        seed("--append")
        audit(3)
        publish("--once", want=4)
        audit(3)

        # Restart a real Flowie broker and retry the pending event.
        broker2 = Broker(broker_bin, env, root)
        try:
            publish("--once")
            broker2.wait_events(1)
            audit(0)
        finally:
            broker2.close()

        print("mygames.flowie_qos1_outbox: PASS "
              "(PUBACK, at-least-once duplicate, broker outage, restart redelivery)",
              flush=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
