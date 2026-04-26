"""SPI-based sensor scanning helper backed by photon_sensorscan.

Requires the special CircuitPython build for the PHOTON Sensor node with
the photon_sensorscanner/photon_sensorscan module.
"""

from __future__ import annotations

from array import array

from app.utils import sleep_us

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


def _normalize_int_list(values, default_values, required_len: int, min_v: int, max_v: int):
    src = default_values if values is None else values
    out = []
    for idx in range(required_len):
        raw = src[idx] if idx < len(src) else default_values[idx % len(default_values)]
        try:
            value = int(raw)
        except Exception:
            value = default_values[idx % len(default_values)]
        if value < min_v:
            value = min_v
        if value > max_v:
            value = max_v
        out.append(value)
    return out


# Build a safe per-bank SPI bus map of length bank_count, with defaults and clamped indices.
def _normalize_bank_map(bank_map, bank_count: int, spi_count: int):
    default_map = [0 if idx < 4 else 1 for idx in range(bank_count)]
    if spi_count <= 1:
        return [0] * bank_count
    if bank_map is None:
        return default_map
    out = []
    for idx in range(bank_count):
        value = bank_map[idx] if idx < len(bank_map) else default_map[idx]
        try:
            bus_idx = int(value)
        except Exception:
            bus_idx = default_map[idx]
        if bus_idx < 0:
            bus_idx = 0
        if bus_idx >= spi_count:
            bus_idx = spi_count - 1
        out.append(bus_idx)
    return out


class Scanner:
    """Wrapper around the photon_sensorscan C backend."""

    def __init__(
        self,
        spi_buses,
        bank_cs_pins,
        *,
        settle_us: int,
        samples_per_channel: int = 1,
        sensors_per_bank: int,
        total_sensors: int,
        bank_spi_bus=None,
        sensor_adc_channels=None,
        sensor_enable_gpio_bits=None,
        spi_baudrate: int = 4_000_000,
        spi_mode: int = 0,
        osr_mode: int = 0,
    ) -> None:
        self.total_sensors = int(total_sensors)
        self.sensors_per_bank = int(sensors_per_bank)
        self._settle_us = int(settle_us)
        self._samples_per_channel = max(1, int(samples_per_channel))
        self._spi_baudrate = max(100_000, int(spi_baudrate))
        self._spi_mode = int(spi_mode)
        if self._spi_mode < 0 or self._spi_mode > 3:
            raise ValueError("spi_mode must be 0, 1, 2, or 3")
        self._osr_mode = max(0, min(7, int(osr_mode)))
        self._spi_polarity = 1 if self._spi_mode >= 2 else 0
        self._spi_phase = 1 if (self._spi_mode % 2) else 0

        if self.total_sensors < 1:
            raise ValueError("total_sensors must be >= 1")
        if self.sensors_per_bank < 1:
            raise ValueError("sensors_per_bank must be >= 1")

        self._spi_specs = []
        for bus in spi_buses:
            if isinstance(bus, (tuple, list)) and len(bus) == 3:
                self._spi_specs.append(("pins", tuple(bus)))
            else:
                self._spi_specs.append(("obj", bus))
        if len(self._spi_specs) == 0:
            raise ValueError("at least one SPI bus is required")

        self._bank_count = len(bank_cs_pins)
        if self._bank_count < 1:
            raise ValueError("at least one bank CS pin is required")
        self._bank_cs_raw_pins = list(bank_cs_pins)

        self._bank_spi_bus = _normalize_bank_map(bank_spi_bus, self._bank_count, len(self._spi_specs))
        self._sensor_adc_channels = _normalize_int_list(
            sensor_adc_channels,
            [7, 5, 3, 1],
            self.sensors_per_bank,
            0,
            7,
        )
        self._sensor_enable_gpio_bits = _normalize_int_list(
            sensor_enable_gpio_bits,
            [6, 4, 2, 0],
            self.sensors_per_bank,
            -1,
            7,
        )
        self._emitter_masks = [0 if bit < 0 else (1 << bit) for bit in self._sensor_enable_gpio_bits]
        self._emitter_all_mask = 0
        for mask in self._emitter_masks:
            self._emitter_all_mask |= int(mask)

        self._update_id = 0
        self._use_c = False
        self._c_refresh_sensor = None
        self._c_refresh_bank = None
        self._c_refresh_all = None
        self._c_refresh_status = None
        self._c_brownout = None
        self._c_crcerr_fuse = None
        self._c_reset_device = None
        self._c_module = None
        self._startup_status_probe_pending = False
        self._startup_status_probe_bank = 0

        # C driver owns this buffer layout (bank-major indexing).
        self._readings_buffer = array("H", [0] * (self._bank_count * self.sensors_per_bank))

        try:
            self._c_module = __import__("photon_sensorscan")
        except ImportError as exc:
            msg = (
                "Missing photon_sensorscan C driver. "
                "Install CircuitPython using the special UF2 for Photon sensor boards."
            )
            print(f"[ERR] {msg}")
            raise RuntimeError(msg) from exc
        try:
            self._init_c_backend()
        except Exception as exc:
            self._use_c = False
            print(f"[ERR] photon_sensorscan init failed: {exc.__class__.__name__}: {exc}")
            raise
        self._use_c = True

    @property
    def update_id(self) -> int:
        return int(self._update_id)

    @property
    def use_c(self) -> bool:
        return self._use_c

    @property
    def bank_count(self) -> int:
        return int(self._bank_count)

    @property
    def readings_buffer(self):
        return self._readings_buffer

    def debug_config(self) -> dict:
        return {
            "bank_count": int(self._bank_count),
            "sensors_per_bank": int(self.sensors_per_bank),
            "total_sensors": int(self.total_sensors),
            "bank_spi_bus": tuple(int(v) for v in self._bank_spi_bus),
            "sensor_adc_channels": tuple(int(v) for v in self._sensor_adc_channels),
            "sensor_enable_gpio_bits": tuple(int(v) for v in self._sensor_enable_gpio_bits),
            "bank_cs_pins": tuple(str(pin) for pin in self._bank_cs_raw_pins),
        }

    def c_debug_state(self):
        if self._c_module is None:
            return None
        debug_state_fn = getattr(self._c_module, "debug_state", None)
        if not callable(debug_state_fn):
            return None
        try:
            return debug_state_fn()
        except Exception:
            return None

    def deinit(self) -> None:
        deinit_fn = getattr(self._c_module, "deinit", None)
        if callable(deinit_fn):
            deinit_fn()

    def reinit_c_backend(self) -> bool:
        if self._c_module is None:
            return False
        self._use_c = False
        self._clear_c_bindings()
        deinit_fn = getattr(self._c_module, "deinit", None)
        if callable(deinit_fn):
            try:
                deinit_fn()
            except Exception as exc:
                print(f"[WARN] photon_sensorscan deinit during reinit failed: {exc}")
        sleep_us(1000)
        try:
            self._init_c_backend()
        except Exception as exc:
            self._use_c = False
            print(f"[WARN] photon_sensorscan reinit failed: {exc.__class__.__name__}: {exc}")
            return False
        self._use_c = True
        return True

    def scan_all_sensors(self, *, on_scan=None, handle_frames=None) -> None:
        # Prefer explicit used-bank refresh to avoid stale values on a partial final bank
        # when total_sensors is not an exact multiple of sensors_per_bank.
        used_bank_count = (self.total_sensors + self.sensors_per_bank - 1) // self.sensors_per_bank
        if callable(self._c_refresh_bank):
            for bank in range(used_bank_count):
                self._c_refresh_bank(bank)
        else:
            self._c_refresh_all()
        if on_scan is not None:
            on_scan(self._readings_buffer, self.total_sensors)
        if handle_frames is not None:
            handle_frames()
        self._update_id = (self._update_id + 1) & 0xFFFFFFFF
        self._maybe_log_startup_system_status_after_first_scan()

    def scan_sensor_subset(self, buffer, indices, *, on_sample=None, handle_frames=None) -> None:
        for out_idx, sensor_idx in enumerate(indices):
            value = self.read_sensor(int(sensor_idx))
            _buffer_set(buffer, out_idx, value)
            if on_sample is not None:
                on_sample(int(sensor_idx), value)
            if handle_frames is not None:
                handle_frames()
        self._update_id = (self._update_id + 1) & 0xFFFFFFFF
        self._maybe_log_startup_system_status_after_first_scan()

    def read_channel(self, bank: int, channel: int) -> int:
        bank = int(bank)
        channel = int(channel)
        if bank < 0 or bank >= self._bank_count:
            raise ValueError("bank out of range")
        if channel < 0 or channel >= self.sensors_per_bank:
            raise ValueError("channel out of range")
        idx = (bank * self.sensors_per_bank) + channel
        if idx >= self.total_sensors:
            raise ValueError("sensor out of range")
        self._c_refresh_sensor(bank, channel)
        value = int(self._readings_buffer[idx])
        return int(value)

    def read_sensor(self, sensor_idx: int) -> int:
        sensor_idx = int(sensor_idx)
        if sensor_idx < 0 or sensor_idx >= self.total_sensors:
            raise ValueError("sensor_idx out of range")
        bank = sensor_idx // self.sensors_per_bank
        channel = sensor_idx % self.sensors_per_bank
        return self.read_channel(bank, channel)

    def read_system_status(self, bank: int) -> int:
        bank = int(bank)
        status = 0
        if bool(self._c_brownout(bank)):
            status |= 0x01
        if bool(self._c_crcerr_fuse(bank)):
            status |= 0x04
        return int(status)

    def reset_system_status(self, bank: int) -> None:
        bank = int(bank)
        if bank < 0 or bank >= self._bank_count:
            raise ValueError("bank out of range")
        if not callable(self._c_reset_device):
            raise RuntimeError("C backend does not expose reset_device")
        self._c_reset_device(bank)

    def arm_startup_system_status_probe(self, bank: int = 0) -> None:
        bank = int(bank)
        if bank < 0 or bank >= self._bank_count:
            raise ValueError("bank out of range")
        self.reset_system_status(bank)
        self._startup_status_probe_pending = True
        self._startup_status_probe_bank = bank
        print(f"[CHK] SYSTEM_STATUS reset requested on bank{bank}.")

    def _init_c_backend(self) -> None:
        spi0 = self._spi_specs[0]
        spi1 = self._spi_specs[1]
        if spi0[0] != "pins" or spi1[0] != "pins":
            raise RuntimeError("C scanner requires SPI pin tuples")
        if self._c_module is None:
            raise RuntimeError("photon_sensorscan module not loaded")
        # Always clear any prior C-side retained pin state across soft reloads.
        deinit_fn = getattr(self._c_module, "deinit", None)
        if callable(deinit_fn):
            try:
                deinit_fn()
            except Exception:
                pass

        # New C init API requires explicit bank count and a per-bank SPI bus map.
        bank_count = int(self._bank_count)
        bank_spi_bus = tuple(
            0 if int(bus_idx) <= 0 else 1 for bus_idx in self._bank_spi_bus[:bank_count]
        )
        if len(bank_spi_bus) != bank_count:
            raise RuntimeError("bank_spi_bus length must match bank_count")

        self._c_module.init(
            spi0=tuple(spi0[1]),
            spi1=tuple(spi1[1]),
            bank_count=bank_count,
            cs=tuple(self._bank_cs_raw_pins),
            bank_spi_bus=bank_spi_bus,
            adc_channels=tuple(self._sensor_adc_channels),
            emitter_bits=tuple(self._sensor_enable_gpio_bits),
            settle_us=self._settle_us,
            baudrate=self._spi_baudrate,
            polarity=self._spi_polarity,
            phase=self._spi_phase,
            readings_buffer=self._readings_buffer,
            osr_mode=self._osr_mode,
        )
        self._c_refresh_sensor = self._c_module.refresh_sensor
        self._c_refresh_bank = self._c_module.refresh_bank
        self._c_refresh_all = self._c_module.refresh_all
        self._c_refresh_status = getattr(self._c_module, "refresh_status", None)
        self._c_brownout = getattr(self._c_module, "brownout", None)
        self._c_crcerr_fuse = getattr(self._c_module, "crcerr_fuse", None)
        self._c_reset_device = getattr(self._c_module, "reset_device", None)
        self._validate_sensor_scan_init()

    def _clear_c_bindings(self) -> None:
        self._c_refresh_sensor = None
        self._c_refresh_bank = None
        self._c_refresh_all = None
        self._c_refresh_status = None
        self._c_brownout = None
        self._c_crcerr_fuse = None
        self._c_reset_device = None

    def _validate_sensor_scan_init(self) -> None:
        if not callable(self._c_refresh_bank):
            return
        if self._emitter_all_mask == 0:
            return

        startup_min_valid = 1
        startup_passes = 4
        startup_retry_us = 1000
        startup_max = 0
        for _ in range(startup_passes):
            for bank in range(self._bank_count):
                base = bank * self.sensors_per_bank
                if base >= self.total_sensors:
                    break
                self._c_refresh_bank(bank)
                count = min(self.sensors_per_bank, self.total_sensors - base)
                for slot in range(count):
                    sample = int(self._readings_buffer[base + slot])
                    if sample > startup_max:
                        startup_max = sample
            if startup_max >= startup_min_valid:
                return
            sleep_us(startup_retry_us)
        print(f"[WARN] Sensor scan init low startup values (max={startup_max}); continuing.")

    def _format_system_status(self, status: int) -> str:
        return (
            f"BOR={status & 0x01} "
            f"CRCERR_FUSE={(status >> 2) & 0x01} "
            f"OSR_DONE={(status >> 3) & 0x01} "
            f"SEQ_STATUS={(status >> 6) & 0x01} "
            f"RSVD7={(status >> 7) & 0x01}"
        )

    def _maybe_log_startup_system_status_after_first_scan(self) -> None:
        if not self._startup_status_probe_pending:
            return
        bank = self._startup_status_probe_bank
        self._startup_status_probe_pending = False
        if callable(self._c_refresh_status):
            try:
                self._c_refresh_status()
            except Exception as exc:
                print(
                    "[WARN] SYSTEM_STATUS refresh after first scan failed "
                    f"on bank{bank}: {exc}"
                )
        try:
            status = int(self.read_system_status(bank)) & 0xFF
        except Exception as exc:
            print(f"[WARN] SYSTEM_STATUS read after first scan failed on bank{bank}: {exc}")
            return
        print(
            f"[CHK] SYSTEM_STATUS after first scan bank{bank}: 0x{status:02X} "
            f"({self._format_system_status(status)})"
        )
