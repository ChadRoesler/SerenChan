#!/usr/bin/env python3
"""MCP server for a StackChan running the LAN control API.

Wraps the REST surface in firmware/main/hal/hal_control_server.cpp so the
robot can be driven as tools rather than shell commands.

Config (stdio server):

    "stackchan": {
      "type": "stdio",
      "command": "python",
      "args": ["D:\\\\serenDaemon\\\\StackChan\\\\mcp\\\\stackchan_mcp.py"],
      "env": {"STACKCHAN_URL": "http://192.168.0.201:8080"}
    }

STACKCHAN_TOKEN is sent as X-StackChan-Token when set, matching
CONFIG_STACKCHAN_CONTROL_TOKEN on the device.
"""

import os
import sys

import httpx
from mcp.server.fastmcp import FastMCP

BASE = os.environ.get("STACKCHAN_URL", "http://192.168.0.201:8080").rstrip("/")
TOKEN = os.environ.get("STACKCHAN_TOKEN", "")
TIMEOUT = float(os.environ.get("STACKCHAN_TIMEOUT", "6"))

mcp = FastMCP("stackchan")


def _headers():
    return {"X-StackChan-Token": TOKEN} if TOKEN else {}


def _call(method, path, payload=None):
    """One place for every request, so failures read the same everywhere.

    The robot is a device on a LAN: it gets unplugged, loses WiFi, reboots
    into the bootloader. A dead socket is an ordinary outcome, not a crash.
    """
    url = BASE + path
    try:
        with httpx.Client(timeout=TIMEOUT) as client:
            if method == "GET":
                r = client.get(url, headers=_headers())
            else:
                r = client.post(url, json=payload or {}, headers=_headers())
    except httpx.ConnectError:
        return {"ok": False,
                "error": "cannot reach the StackChan at %s -- is it powered and on the network?" % BASE}
    except httpx.TimeoutException:
        return {"ok": False, "error": "timed out after %gs talking to %s" % (TIMEOUT, BASE)}
    except Exception as exc:  # noqa: BLE001 - surface anything unexpected as data
        return {"ok": False, "error": "%s: %s" % (type(exc).__name__, exc)}

    if r.status_code == 401:
        return {"ok": False, "error": "rejected: bad or missing token (set STACKCHAN_TOKEN)"}
    if r.status_code >= 400:
        return {"ok": False, "error": "HTTP %d: %s" % (r.status_code, r.text[:200])}

    try:
        return r.json()
    except Exception:
        return {"ok": False, "error": "non-JSON reply: %s" % r.text[:200]}


@mcp.tool()
def stackchan_status() -> dict:
    """Read the robot's current state.

    Returns firmware version, battery level and charging flag, free internal
    heap and PSRAM, current head yaw/pitch in degrees, and the name of the
    loaded face ("default" means no custom face was found).
    """
    return _call("GET", "/api/status")


@mcp.tool()
def stackchan_set_head(yaw: int = -9999, pitch: int = -9999, speed: int = 150) -> dict:
    """Point the robot's head.

    yaw: -128..128, negative is the robot's left. pitch: 0..90, 90 is up.
    Omit either to leave that axis alone. speed: 100..1000, 150 reads natural.

    Stay within +/-45 yaw for normal interaction; larger angles look like the
    robot is deliberately staring away.
    """
    return _call("POST", "/api/head", {"yaw": yaw, "pitch": pitch, "speed": speed})


@mcp.tool()
def stackchan_set_led(r: int = 0, g: int = 0, b: int = 0) -> dict:
    """Set both onboard LED strips. Each channel 0..168 (the safe range).

    This is the robot's own internal lighting, not room lights.
    """
    return _call("POST", "/api/led", {"r": r, "g": g, "b": b})


@mcp.tool()
def stackchan_set_emotion(name: str) -> dict:
    """Set the facial expression.

    Known to the stock face: neutral, happy, laughing, funny, sad, angry,
    crying, loving, embarrassed, surprised, shocked, thinking, winking, cool,
    relaxed, delicious, kissy, confident, sleepy, silly, confused.

    A custom face can define its own names in face.yaml. Anything unrecognised
    falls back to neutral rather than failing.

    Note: "sleepy" also parks the head and stops idle movement.
    """
    return _call("POST", "/api/emotion", {"name": name})


@mcp.tool()
def stackchan_say(text: str) -> dict:
    """Put text in the robot's speech bubble.

    This is display only -- it does not speak aloud. Audio belongs to the
    assistant backend, not this API.
    """
    return _call("POST", "/api/say", {"text": text})


if __name__ == "__main__":
    print("stackchan mcp -> %s%s" % (BASE, " (token set)" if TOKEN else " (no token)"),
          file=sys.stderr)
    mcp.run()
