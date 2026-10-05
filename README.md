# iw-test-agent – DUT agent for AP/STA (WDS) + 802.11s mesh tests

The agent runs on each OpenWrt DUT. It connects to the
[iw-test-controller](https://github.com/gvvsnrnaveen/iw-test-controller) running on a laptop.
The controller plans and runs the tests and writes the results. The agent receives its commands
and applies them to the DUT through `uci`, `wifi`, `iw` and `ping`. When the run ends, the agent
restores the DUT's original configuration.

```
 Laptop                                      DUT 1 / DUT 2 (this repo)
 ┌──────────────────────────┐   TCP 5555     ┌──────────────────────────────┐
 │ iw-test-controller       │◄───────────────│ iw-test-agent          │
 │  plans and runs tests,   │  JSON lines    │  uci / wifi / iw / ping      │
 │  writes the CSV          │  over ethernet │  backup + restore of config  │
 └──────────────────────────┘                └──────────────────────────────┘
```

The agent connects **to the controller** and retries every 5 s until it gets through, so the
controller and the agents can start in any order. Controller setup, test selection, the test
matrix and the CSV output are documented in the
[controller repository](https://github.com/gvvsnrnaveen/iw-test-controller).

## Layout

| Path | What |
|---|---|
| `dut_agent/iw-test-agent.c` | DUT agent (single C file, libc only) |
| `dut_agent/openwrt/` | OpenWrt SDK package with a procd service so agents auto-connect at boot |
| `dut_agent/deploy_agent.sh` | scp the binary and start it over ssh |

## Requirements

* OpenWrt ≥ 21.02 (uses `config device` bridge syntax), `iw`, `iwinfo`, busybox `ping`.
* For mesh with `sae`, install `wpad-mesh-*`. Open mesh works with stock `wpad`.
* An ethernet management link to the controller. The agent disables the DUT's wifi-ifaces
  while tests run.

## Build

```sh
# native (for simulation on the laptop)
make -C dut_agent

# cross compile with an OpenWrt / QSDK toolchain
export STAGING_DIR=<sdk>/staging_dir
make -C dut_agent CC=<sdk>/staging_dir/toolchain-*/bin/aarch64-openwrt-linux-musl-gcc

# or build an .ipk (installs /usr/sbin/iw-test-agent + /etc/init.d/iw-test-agent)
cp -rL dut_agent/openwrt <sdk>/package/iw-test-agent-agent
make -C <sdk> package/iw-test-agent-agent/compile V=s
```

## Start the agent

In the examples, `192.168.1.100` is the controller and `192.168.1.1` / `192.168.1.2` are the DUTs.

Option A, ad hoc:
```sh
AGENT_BIN=path/to/cross/iw-test-agent dut_agent/deploy_agent.sh 192.168.1.1 192.168.1.100 dut1
AGENT_BIN=path/to/cross/iw-test-agent dut_agent/deploy_agent.sh 192.168.1.2 192.168.1.100 dut2
```

Option B, persistent (the .ipk is installed):
```sh
uci set iw-test-agent.main.server=192.168.1.100
uci set iw-test-agent.main.name=dut1
uci set iw-test-agent.main.enabled=1
uci commit iw-test-agent && /etc/init.d/iw-test-agent enable && /etc/init.d/iw-test-agent start
```

### Options

```
iw-test-agent -s <controller-ip> [options]
  -s, --server HOST        controller (laptop) address (required)
  -p, --port PORT          controller port (default 5555)
  -n, --name NAME          DUT name reported to controller (default hostname)
  -t, --token TOKEN        shared token, must match controller --token
  -r, --retry SEC          reconnect interval (default 5)
  -m, --radio-map MAP      phy->uci map, e.g. phy0=radio0,phy1=radio1
  -N, --narrowband-cmd CMD hook run before 'wifi up' for 5/10 MHz tests (%p %r %b %c)
  -l, --log FILE           log to FILE instead of stderr
  -1, --once               exit after the first session ends
  -R, --no-restore         do not restore original config on disconnect
  -S, --sim                simulation mode (no wireless changes)
  -F, --sim-fail PCT       simulated failure rate per operation in % (with -S)
  -v, --verbose            log every executed command
```

## What the agent does to the DUT

The controller decides which tests run and in what order. For each test, the agent:

1. **Prepares** (once per session): backs up `/etc/config/{wireless,network,firewall}` to
   `/tmp/iw-test-agent_backup`. Creates bridge `br-fpt` with `<subnet>.1` (DUT A) or
   `<subnet>.2` (DUT B), plus an ACCEPT firewall zone. Disables the existing wifi-ifaces.
2. **Applies** the test: enables only the radio under test. Sets `channel` and `htmode`;
   HT5/HT10 also set `hwmode=11g`. Creates `wireless.fpt_iface` (ifname `fpt0`, `wds=1` for AP/STA,
   `mesh_id` for mesh), then runs `wifi up`.
3. **Reports the link state**: whether the AP came up and the STA shows `Connected to` (AP/STA),
   or whether a peer is in `mesh plink ESTAB` (mesh).
4. **Reports** the channel and width from `iw dev fpt0 info`, plus signal and bitrate.
5. **Pings** the other DUT when the controller asks.
6. **Tears down** the test interface.

At the end of the session, when the controller disconnects, or when the agent receives SIGTERM,
the agent restores the original config (unless `-R`). If it finds a stale backup at start-up, it
restores that too.

## Notes and limitations

* **HT5 / HT10**: these are configured as `htmode=HT5`/`HT10` with `hwmode=11g`.
  Upstream mac80211 and drivers do not implement 5/10 MHz for AP/STA/mesh. Unless your
  firmware/driver build supports it, these tests will FAIL with
  `width 20 MHz, expected 5`, which is a real finding, not a tool error. If your vendor tree has
  its own knob, pass it via `-N/--narrowband-cmd`.
* SSIDs, keys and country codes may contain only `A-Z a-z 0-9 . _ - + @ : = ,`, because they are
  passed to `uci` through the shell.
* Set a real country on the controller. In the world regdomain most 5 GHz channels are *no-IR*,
  and those channels are skipped.
* Simulation: `iw-test-agent -s 127.0.0.1 -n dut1 -S` (and `-n dut2`) lets you try the full
  controller flow on the laptop without hardware.
* Run only one agent per DUT. A second agent with the same name replaces the first session, and
  the controller logs a warning.
