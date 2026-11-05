# StackChan MCP server

Drives a StackChan over the LAN control API as tools instead of shell commands.

Wraps the REST surface in
[`firmware/main/hal/hal_control_server.cpp`](../firmware/main/hal/hal_control_server.cpp).

## Tools

| Tool | What it does |
|---|---|
| `stackchan_status` | battery, charging, free heap/PSRAM, head angles, loaded face |
| `stackchan_set_head` | `yaw` -128..128, `pitch` 0..90, `speed` 100..1000 |
| `stackchan_set_led` | `r`/`g`/`b`, each 0..168 |
| `stackchan_set_emotion` | any of the 21 names, or a custom one from `face.yaml` |
| `stackchan_say` | text into the speech bubble (display only, not spoken) |

## Setup

Registered project-scoped in [`../.mcp.json`](../.mcp.json). Claude Code asks
once before trusting a project server; approve it and the tools appear.

Requires `mcp` and `httpx` in the Python that runs it:

```bash
pip install mcp httpx
```

## Configuration

| Variable | Default | Notes |
|---|---|---|
| `STACKCHAN_URL` | `http://192.168.4.86:8080` | the device's address |
| `STACKCHAN_TOKEN` | *(empty)* | sent as `X-StackChan-Token` |
| `STACKCHAN_TIMEOUT` | `6` | seconds per request |

**The device's IP can change.** It is a DHCP lease, so a router reboot may move
it. Check the boot log (`[HAL-Control] got ip ...`) or your DHCP table, then
update `STACKCHAN_URL`. A static reservation avoids the problem.

## Security

With `CONFIG_STACKCHAN_CONTROL_TOKEN` blank on the device — the default —
**anything on the LAN can drive the robot.** No authentication at all.

To lock it down, set the token in `firmware/sdkconfig.defaults.local`
(git-ignored), reflash, then set the same value as `STACKCHAN_TOKEN` here.

## Failure modes

Every tool returns `{"ok": false, "error": "..."}` rather than raising, because
a robot on a desk gets unplugged, loses WiFi and reboots. Expect:

- `cannot reach the StackChan at ...` — powered off, or wrong address
- `timed out after 6s` — on the network but not answering; often mid-boot,
  since the control server only starts once an IP is assigned
- `rejected: bad or missing token` — device has a token, this side does not
