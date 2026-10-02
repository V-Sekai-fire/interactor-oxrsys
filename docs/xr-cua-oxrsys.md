# xr-cua-oxrsys

`xr-cua-oxrsys` is the name a Claude Code (or other MCP) session uses to drive and
observe the OXRSys Simulator through [cua-driver](https://github.com/trycua/cua)
on macOS. The server runs on the same machine as the simulator; one agent can
then launch the simulator, connect it to a running OXRSys runtime, read the
decoded preview and send simulated keyboard, mouse and scroll input, all in
the background.

Flow 2 (headset client) adds a Linux cua-driver on the headset and a second
`xr-cua-oxrsys-headset` MCP, invoked over SSH.

## Prerequisites

- `cua-driver` installed on the Mac (`brew install trycua/cua/cua-driver`, or a
  release tarball). The simulator will drive without it; `xr-cua-oxrsys` is only the
  automation surface.
- Accessibility and Screen Recording granted to the CuaDriver app
  (`cua-driver permissions grant` opens the dialogs). The grants attribute to
  `com.trycua.driver`, not to the terminal that runs the agent.

## Register the MCP server

Add this to `~/.claude.json` under `mcpServers`, or run the installer below:

```json
"xr-cua-oxrsys": {
  "command": "cua-driver",
  "args": ["mcp"]
}
```

The repo ships a one-shot installer that writes the row, keeps the user's other
servers, and prints a diff:

```
scripts/xr-cua-oxrsys-register.sh
```

A session that was running when the row was added picks it up on `/mcp
reconnect xr-cua-oxrsys`.

## Drive the simulator from a session

Once the simulator is open and connected to a runtime, an agent can:

- `launch_app` the simulator (bundle id `com.trycua.oxrsys.simulator`,
  or `OXRSys Simulator.app` by path),
- `get_window_state` to read the preview as pixels and the control row as
  AX elements,
- `click`, `type_text`, `press_key` and `scroll` to send keyboard, mouse and
  scroll into the view the simulator captures (`ZQSD/WASD`, mouse look, scroll
  to walk).

All calls run in the background: no focus steal, no cursor move, and the
operator can keep typing in another window.

## Not covered here

- The headset-side `xr-cua-oxrsys-headset` for the Steam Frame or a Quest is on the
  Flow 2 branch and lives in `docs/platforms/`.
- Shipping a bundled cua-driver with the simulator is out of scope. Users
  install it once and reuse it across projects.
