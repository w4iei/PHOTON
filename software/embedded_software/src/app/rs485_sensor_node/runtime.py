"""CircuitPython runtime helpers."""

try:
    import board
    import busio
    import photon_rs485
    import storage
    import microcontroller
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    board = None  # type: ignore
    busio = None  # type: ignore
    photon_rs485 = None  # type: ignore
    storage = None  # type: ignore
    microcontroller = None  # type: ignore

try:
    import supervisor
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    supervisor = None  # type: ignore


def reset_board() -> bool:
    if microcontroller is not None:
        try:
            microcontroller.reset()
            return True
        except Exception:
            pass
    if supervisor is not None:
        try:
            supervisor.reload()
            return True
        except Exception:
            pass
    return False
