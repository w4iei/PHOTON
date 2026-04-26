"""CircuitPython runtime helpers."""

import board
import busio
import microcontroller
import photon_rs485
import storage
import supervisor


def reset_board() -> bool:
    try:
        microcontroller.reset()
        return True
    except Exception:
        pass
    try:
        supervisor.reload()
        return True
    except Exception:
        pass
    return False
