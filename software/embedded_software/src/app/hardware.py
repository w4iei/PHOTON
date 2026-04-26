"""
Simple shared pin registry so multiple scripts can reuse digital IO safely.

CircuitPython raises ``ValueError: Pin in use`` if you instantiate
``DigitalInOut`` twice for the same pin.  We avoid this by storing the handle
the first time it is claimed and returning the same object on subsequent calls.
"""

from digitalio import DigitalInOut, Direction, Pull

_REGISTRY = {}


def claim_output(pin, *, value=False):
    """
    Get a ``DigitalInOut`` configured as an output for ``pin``.

    If the pin was previously claimed, the same handle is returned instead of
    creating a new object.  The direction is forced to OUTPUT each time so that
    re-use from other scripts still works.
    """

    dio = _REGISTRY.get(pin)
    if dio is None:
        dio = DigitalInOut(pin)
        _REGISTRY[pin] = dio
    dio.direction = Direction.OUTPUT
    dio.value = bool(value)
    return dio


def claim_input(pin, *, pull=None):
    """
    Get a ``DigitalInOut`` configured as an input for ``pin``.

    ``pull`` may be ``Pull.UP`` or ``Pull.DOWN``.
    """

    dio = _REGISTRY.get(pin)
    if dio is None:
        dio = DigitalInOut(pin)
        _REGISTRY[pin] = dio
    dio.direction = Direction.INPUT
    if pull is not None:
        dio.pull = pull
    return dio


def release(pin):
    """
    Deinit and forget a previously claimed pin.

    Most scripts never call this because they hold hardware for the lifetime of
    the program, but it is handy for REPL experiments.
    """

    dio = _REGISTRY.pop(pin, None)
    if dio is not None:
        try:
            dio.deinit()
        except AttributeError:
            pass
