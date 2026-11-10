#!/usr/bin/env python3
"""Launch BrawlRecompiled with Brawl Online matchmaking from a TOML configuration."""

import argparse
import json
import os
import signal
import shutil
import secrets
import subprocess
import sys
import time
from pathlib import Path
from types import SimpleNamespace


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", type=Path)
    parser.add_argument("--account", type=Path, help="Active launcher's Online/user.json")
    parser.add_argument("--output", type=Path, help="New session output directory")
    parser.add_argument("--plugin", type=Path, help="Brawl Online PPOnline.rel to install on recomp's SD")
    parser.add_argument("--handoff-dir", type=Path, help="The launcher's netplay User/Online folder")
    args = parser.parse_args()
    os.environ["BRAWL_ONLINE_CONFIG"] = str(args.config.resolve())
    if args.account:
        os.environ["BRAWL_ONLINE_ACCOUNT"] = str(args.account.resolve())
    if args.output:
        os.environ["BRAWL_ONLINE_OUTPUT"] = str(args.output.resolve())
    from online_config import ACCOUNT_FILE, ALLOWED_MODES, BRAWL_ONLINE_ROOT, CONFIG, MMCLIENT, path
    from online_loop import OnlineLoop
    from online_handoff import LaunchHandoff
    from online_menu import MenuBridge, ppom
    from online_protocol import Memory

    binary = path("client")
    for required in (
        binary,
        MMCLIENT,
        ACCOUNT_FILE,
        BRAWL_ONLINE_ROOT / "tools/gamecode/ppom.py",
    ):
        if not required.is_file():
            raise FileNotFoundError(required)
    with ACCOUNT_FILE.open() as stream:
        account = json.load(stream)
    for key in ("uid", "playKey", "displayName", "connectCode"):
        if not isinstance(account.get(key), str) or not account[key]:
            raise ValueError(f"Account file requires {key}")
    user = SimpleNamespace(
        display_name=account["displayName"], connect_code=account["connectCode"]
    )
    if args.plugin:
        plugin = args.plugin.resolve()
        if not plugin.is_file():
            raise FileNotFoundError(plugin)
        if "sd_root" in CONFIG["paths"]:
            sd_root = path("sd_root")
        elif (binary.parent / "portable.txt").is_file():
            sd_root = binary.parent / "UserData" / "SD"
        else:
            raise ValueError("Set paths.sd_root for a non-portable recomp installation")
        if not (sd_root / "Project+" / "pf").is_dir():
            raise FileNotFoundError(f"Project+ files are missing from {sd_root}")
        target = sd_root / "Project+" / "pf" / "plugins" / "PPOnline.rel"
        os.umask(0o077)
        target.parent.mkdir(parents=True, exist_ok=True)
        data = plugin.read_bytes()
        if not target.is_file() or target.read_bytes() != data:
            temporary = target.with_name(f".PPOnline-{os.getpid()}.tmp")
            try:
                temporary.write_bytes(data)
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
    output = path("output")
    os.umask(0o077)
    output.mkdir(parents=True, exist_ok=False)
    ipc = output / "control.port"
    token = secrets.token_hex(32)
    port = CONFIG["network"]["port"]
    if not isinstance(port, int) or not 1024 <= port <= 65533:
        raise ValueError("network.port must be between 1024 and 65533")
    env = dict(os.environ)
    for key in list(env):
        if key.startswith(
            ("BRAWL_ONLINE_", "BRAWL_ROLLBACK_", "BRAWL_REPLAY_")
        ) or key in (
            "BRAWL_CROSSPLAY_SEED",
            "BRAWL_PROTOCOL_SEED",
            "NSMBW_INPUT_SCRIPT",
        ):
            del env[key]
    env.update(
        BRAWL_ONLINE_IPC=str(ipc),
        BRAWL_ONLINE_IPC_TRANSPORT="tcp",
        BRAWL_ONLINE_IPC_TOKEN=token,
        BRAWL_ONLINE_LOOP="1",
        BRAWL_ROLLBACK_PORT=str(port + 2),
        BRAWL_ROLLBACK_PEER_PORT=str(port + 1),
        BRAWL_ROLLBACK_SLOT="1",
        BRAWL_ROLLBACK_START="119",
        BRAWL_ROLLBACK_TRACE=str(output / "trace"),
        NSMBW_DETERMINISTIC="1",
    )
    bridge = memory = handoff = None
    # The desktop launcher sends SIGTERM on quit. Run the cleanup below so the
    # game process does not survive its companion.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    with (output / "runtime.log").open("w") as log:
        process = subprocess.Popen(
            [str(binary)],
            cwd=binary.parent,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            deadline = time.monotonic() + 120
            while not ipc.exists():
                if process.poll() is not None:
                    raise RuntimeError(
                        f"Client exited with {process.returncode}; see runtime.log"
                    )
                if time.monotonic() > deadline:
                    raise TimeoutError("Client did not open its control socket")
                time.sleep(0.05)
            ipc.chmod(0o600)
            memory = Memory(ipc, token=token)
            print(
                f"Online as {user.display_name} ({user.connect_code}). "
                + ("Select Unranked or Direct in the game."
                   if 1 in ALLOWED_MODES else "Select Direct in the game."),
                flush=True,
            )
            while process.poll() is None:
                if bridge is None:
                    if memory.status()[1] in ("muMenuMain", "scSelctCharacter"):
                        block = ppom.find_block(memory)
                        if block.version < 10:
                            raise RuntimeError(
                                "Direct stage selection requires PPOnline v10 or newer"
                            )
                        bridge = OnlineLoop(
                            memory,
                            block,
                            user,
                            CONFIG["network"]["server"],
                            output,
                            port=port,
                        )
                        if args.handoff_dir:
                            handoff = LaunchHandoff(memory, block, args.handoff_dir)
                else:
                    bridge.tick()
                    if handoff:
                        handoff.tick(bridge)
                time.sleep(0.005)
            if process.returncode:
                raise RuntimeError(
                    f"Client exited with {process.returncode}; see runtime.log"
                )
        finally:
            if process.poll() is None:
                try:
                    if bridge:
                        bridge.close()
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            if bridge and process.poll() is not None:
                if bridge.wire:
                    bridge.wire.close()
                    bridge.wire = None
                MenuBridge.cancel(bridge)
            if memory:
                memory.close()
            if handoff:
                handoff.close()
            ipc.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
