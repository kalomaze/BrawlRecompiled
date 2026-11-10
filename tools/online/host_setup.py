"""Host-side setup fields matching Dolphin's GameplaySession protocol."""


def read_sync(m):
    stage = m.u32(0x80B8A428)
    positions = [m.u32(stage + 0x1B4 + 4 * i) for i in range(4)]
    if sorted(positions) != list(range(4)):
        raise ValueError("Invalid stage spawn order")
    return dict(
        game_frame=m.read(0x901812A0, 24).hex(),
        app=m.u32(m.u32(0x8059FFAC) + 0x100),
        rng=[m.u32(a) for a in (0x805A00BC, 0x805A0424, 0x8059FF58)],
        serial=m.u32(0x8059C668),
        start_pos=positions,
    )


def read_tasks(m):
    scheduler = m.u32(0x805A0068)
    lists = []
    for base, next_offset, count in ((0x14, 8, 17), (0x58, 16, 40), (0xF8, 24, 14)):
        for i in range(count):
            names, seen = [], set()
            task = m.u32(scheduler + base + 4 * i)
            while task:
                if task in seen or len(seen) >= 4096:
                    raise ValueError("Invalid task chain")
                seen.add(task)
                names.append(m.read(m.u32(task), 48).split(b"\0")[0].decode())
                task = m.u32(task + next_offset)
            lists.append(names)
    return lists


def direct_setup(game, locks, alternate_stages):
    """Both ready; first available stage pick in port order, as Dolphin's host."""
    if any(not lock.get("ready") or lock.get("game") != game for lock in locks):
        return None
    picks = [lock for lock in locks if 0 <= lock.get("stage", 65535) < 65534]
    if not picks:
        return None
    players = []
    for lock in locks:
        if not 0 <= lock["kind"] < 0x3E or len(bytes.fromhex(lock["pv"])) != 60:
            raise ValueError("Invalid player lock-in")
        players.append([lock["kind"], lock["costume"], lock["pv"]])
    pick = picks[0]
    return dict(
        game=game,
        stage=pick["stage"],
        asl=pick.get("asl", 0) if alternate_stages else 0,
        players=players,
    )
