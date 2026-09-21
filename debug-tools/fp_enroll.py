#!/usr/bin/env python3
"""Drive fprintd EnrollStart with a long D-Bus call timeout so the polkit
auth dialog has enough time to be answered, instead of the ~25s default
that fprintd-enroll(1) uses internally."""
import sys
import gi
gi.require_version('GLib', '2.0')
from gi.repository import Gio, GLib

USER = sys.argv[1] if len(sys.argv) > 1 else "dae51d"
FINGER = sys.argv[2] if len(sys.argv) > 2 else "right-index-finger"
DEVICE_PATH = "/net/reactivated/Fprint/Device/0"
BUS_NAME = "net.reactivated.Fprint"
LONG_TIMEOUT_MS = 120000  # 2 minutes for the polkit prompt

bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)

def call(method, args, timeout=-1):
    return bus.call_sync(
        BUS_NAME, DEVICE_PATH, "net.reactivated.Fprint.Device",
        method, args, None, Gio.DBusCallFlags.NONE, timeout, None)

result_state = {"done": False, "ok": False, "msg": ""}

def on_signal(connection, sender, path, iface, signal, params):
    if signal == "EnrollStatus":
        status, done = params.unpack()
        print(f"EnrollStatus: {status} done={done}", flush=True)
        if done:
            result_state["done"] = True
            result_state["ok"] = (status == "enroll-completed")
            result_state["msg"] = status
            loop.quit()

sub_id = bus.signal_subscribe(
    BUS_NAME, "net.reactivated.Fprint.Device", "EnrollStatus", DEVICE_PATH,
    None, Gio.DBusSignalFlags.NONE, on_signal)

print(f"Claiming device for {USER}...", flush=True)
call("Claim", GLib.Variant("(s)", (USER,)))
print("Claimed. Starting enroll (watch for the auth dialog now)...", flush=True)

try:
    call("EnrollStart", GLib.Variant("(s)", (FINGER,)), timeout=LONG_TIMEOUT_MS)
except GLib.Error as e:
    print(f"EnrollStart failed: {e}", flush=True)
    bus.signal_unsubscribe(sub_id)
    call("Release", GLib.Variant("()", ()))
    sys.exit(1)

print("EnrollStart accepted; waiting for tap events (touch the sensor)...", flush=True)
loop = GLib.MainLoop()
GLib.timeout_add_seconds(90, lambda: (loop.quit(), False)[1])  # overall safety cap
loop.run()

bus.signal_unsubscribe(sub_id)
try:
    call("EnrollStop", GLib.Variant("()", ()))
except GLib.Error as e:
    print(f"EnrollStop warning: {e}", flush=True)
try:
    call("Release", GLib.Variant("()", ()))
except GLib.Error as e:
    print(f"Release warning: {e}", flush=True)

if result_state["done"] and result_state["ok"]:
    print("SUCCESS: fingerprint enrolled")
    sys.exit(0)
else:
    print(f"FAILED: {result_state['msg'] or 'no completion signal received'}")
    sys.exit(1)
