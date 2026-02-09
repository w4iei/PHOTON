"""Sensor scanning helper with optional C acceleration."""

from __future__ import annotations

try:
    import photon_sensorscan
except Exception:  # pragma: no cover - optional on host
    photon_sensorscan = None  # type: ignore

try:
    import analogio
except Exception:  # pragma: no cover - optional on host
    analogio = None  # type: ignore

from app.helpers import hardware
from app.helpers.utils import sleep_us


def _buffer_get(buffer, index: int) -> int:
    if hasattr(buffer, "itemsize") and getattr(buffer, "itemsize") == 1:
        offset = index * 2
        return int.from_bytes(buffer[offset : offset + 2], "little")
    if isinstance(buffer, bytearray):
        offset = index * 2
        return int.from_bytes(buffer[offset : offset + 2], "little")
    if hasattr(buffer, "typecode") and getattr(buffer, "typecode") == "H":
        return int(buffer[index])
    return int(buffer[index])


def _buffer_set(buffer, index: int, value: int) -> None:
    if hasattr(buffer, "itemsize") and getattr(buffer, "itemsize") == 1:
        offset = index * 2
        buffer[offset : offset + 2] = int(value).to_bytes(2, "little")
        return
    if isinstance(buffer, bytearray):
        offset = index * 2
        buffer[offset : offset + 2] = int(value).to_bytes(2, "little")
        return
    if hasattr(buffer, "typecode") and getattr(buffer, "typecode") == "H":
        buffer[index] = int(value)
        return
    buffer[index] = int(value)


class Scanner:
    """Wrapper around photon_sensorscan.Scanner with a Python fallback."""

    def __init__(
        self,
        enable_pins,
        sel0_pin,
        sel1_pin,
        adc_pin,
        *,
        settle_us: int,
        samples_per_channel: int = 1,
        sensors_per_bank: int,
        total_sensors: int,
        use_c: bool | None = None,
    ) -> None:
        self.total_sensors = int(total_sensors)
        self.sensors_per_bank = int(sensors_per_bank)
        self._settle_us = int(settle_us)
        self._samples_per_channel = max(1, int(samples_per_channel))
        self._update_id = 0
        self._scanner = None
        self._use_c = False
        self._enable_pins = None
        self._sel0 = None
        self._sel1 = None
        self._adc = None

        if use_c is not False and photon_sensorscan is not None:
            try:
                self._scanner = photon_sensorscan.Scanner(
                    enable_pins,
                    sel0_pin,
                    sel1_pin,
                    adc_pin,
                    settle_us=self._settle_us,
                    samples_per_channel=self._samples_per_channel,
                    sensors_per_bank=self.sensors_per_bank,
                    total_sensors=self.total_sensors,
                )
                self._use_c = True
                return
            except Exception:
                self._scanner = None

        if analogio is None:
            raise RuntimeError("analogio unavailable; cannot use Python scanner fallback.")

        self._enable_pins = [hardware.claim_output(pin, value=False) for pin in enable_pins]
        self._sel0 = hardware.claim_output(sel0_pin, value=False)
        self._sel1 = hardware.claim_output(sel1_pin, value=False)
        self._adc = analogio.AnalogIn(adc_pin)

    @property
    def update_id(self) -> int:
        if self._use_c:
            return int(self._scanner.update_id)
        return int(self._update_id)

    @property
    def use_c(self) -> bool:
        return self._use_c

    def deinit(self) -> None:
        if self._use_c:
            self._scanner.deinit()
            return
        if self._adc is not None:
            self._adc.deinit()

    def scan_into(self, buffer, *, on_sample=None, handle_frames=None) -> None:
        if self._use_c:
            self._scanner.scan_into(buffer)
            if on_sample is not None or handle_frames is not None:
                for sensor_idx in range(self.total_sensors):
                    value = _buffer_get(buffer, sensor_idx)
                    if on_sample is not None:
                        on_sample(sensor_idx, value)
                    if handle_frames is not None:
                        handle_frames()
            return

        total = self.total_sensors
        sensors_per_bank = self.sensors_per_bank
        bank_count = len(self._enable_pins)
        for bank_idx in range(bank_count):
            base = bank_idx * sensors_per_bank
            if base >= total:
                break
            self._select_bank(bank_idx)
            for channel in range(sensors_per_bank):
                sensor_idx = base + channel
                if sensor_idx >= total:
                    break
                self._select_channel(channel)
                sleep_us(self._settle_us)
                value = self._read_adc()
                _buffer_set(buffer, sensor_idx, value)
                if on_sample is not None:
                    on_sample(sensor_idx, value)
                if handle_frames is not None:
                    handle_frames()
            self._enable_pins[bank_idx].value = False
        self._update_id = (self._update_id + 1) & 0xFFFFFFFF

    def scan_indices_into(self, buffer, indices, *, on_sample=None, handle_frames=None) -> None:
        if self._use_c:
            self._scanner.scan_indices_into(buffer, indices)
            if on_sample is not None or handle_frames is not None:
                for out_idx, sensor_idx in enumerate(indices):
                    value = _buffer_get(buffer, out_idx)
                    if on_sample is not None:
                        on_sample(int(sensor_idx), value)
                    if handle_frames is not None:
                        handle_frames()
            return

        for out_idx, sensor_idx in enumerate(indices):
            value = self.read_sensor(int(sensor_idx))
            _buffer_set(buffer, out_idx, value)
            if on_sample is not None:
                on_sample(int(sensor_idx), value)
            if handle_frames is not None:
                handle_frames()
        self._update_id = (self._update_id + 1) & 0xFFFFFFFF

    def read_channel(self, bank: int, channel: int) -> int:
        if self._use_c:
            return int(self._scanner.read_channel(bank, channel))
        self._select_bank(bank)
        self._select_channel(channel)
        sleep_us(self._settle_us)
        value = self._read_adc()
        self._enable_pins[bank].value = False
        return int(value)

    def read_sensor(self, sensor_idx: int) -> int:
        bank = sensor_idx // self.sensors_per_bank
        channel = sensor_idx % self.sensors_per_bank
        return self.read_channel(bank, channel)

    def _select_bank(self, bank_idx: int) -> None:
        for idx, pin in enumerate(self._enable_pins):
            pin.value = idx == bank_idx

    def _select_channel(self, channel: int) -> None:
        self._sel0.value = bool(channel & 0x01)
        self._sel1.value = bool(channel & 0x02)

    def _read_adc(self) -> int:
        if self._samples_per_channel <= 1:
            return int(self._adc.value) >> 4
        acc = 0
        for _ in range(self._samples_per_channel):
            acc += int(self._adc.value)
        return (acc // self._samples_per_channel) >> 4
