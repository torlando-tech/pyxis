"""Regression checks for watchdog/reset detection in the hardware soak harness."""

import importlib.util
import queue
import sys
import threading
import types
from pathlib import Path
from unittest.mock import patch


REPO_ROOT = Path(__file__).resolve().parents[2]
HARNESS_PATH = REPO_ROOT / "tests/hardware/tdeck_harness.py"
SPEC = importlib.util.spec_from_file_location("tdeck_harness", HARNESS_PATH)
assert SPEC is not None
HARNESS = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
with patch.dict(sys.modules, {"serial": types.ModuleType("serial")}):
    SPEC.loader.exec_module(HARNESS)


def test_fault_classifier_detects_watchdog_panic_and_reboot_evidence():
    assert HARNESS.is_fault_line("Guru Meditation Error: Core 0 panic'ed")
    assert HARNESS.is_fault_line("Reset reason: TASK_WDT (6)")
    assert HARNESS.is_fault_line("rst:0xc (SW_CPU_RESET)")
    assert not HARNESS.is_fault_line("Task Watchdog: loopTask subscribed")
    assert not HARNESS.is_fault_line("normal message traffic")


def test_fault_queue_is_armed_atomically_after_intentional_boot():
    tdeck = HARNESS.TDeck.__new__(HARNESS.TDeck)
    tdeck._fault_q = queue.Queue()
    tdeck._fault_lock = threading.Lock()
    tdeck._fault_monitor_armed = threading.Event()

    # Evidence from the intentional harness reset is ignored while unarmed.
    tdeck._record_fault_if_armed("Reset reason: SOFTWARE")
    assert tdeck.drain_faults() == []

    # arm_fault_monitor holds the same lock used by the reader-side recorder, so
    # no reset line can land in the gap between clearing boot evidence and arming.
    tdeck.arm_fault_monitor()
    assert tdeck._fault_monitor_armed.is_set()
    tdeck._record_fault_if_armed("Reset reason: TASK_WDT")
    assert tdeck.drain_faults() == ["Reset reason: TASK_WDT"]
    assert tdeck.drain_faults() == []
