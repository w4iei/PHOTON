"""Display and touch helpers for the RS-485 main host."""

from __future__ import annotations

try:
    import busio
    import digitalio
    import displayio
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    busio = None  # type: ignore
    digitalio = None  # type: ignore
    displayio = None  # type: ignore

from .constants import (
    BAR_BG,
    BAR_COLOR,
    BAR_PAD_Y,
    DISPLAY_HEIGHT,
    DISPLAY_ROTATION,
    DISPLAY_UPDATE_S,
    DISPLAY_VALUE_MAX,
    DISPLAY_WIDTH,
    GRID_COLS,
    GRID_ROWS,
    LABEL_PAD_X,
    LABEL_PAD_Y,
    LABEL_SCALE,
    LCD_SPI_BAUD,
    MAX_SENSORS,
    SENSOR_NODE_DEVICE_IDS,
    TOTAL_SENSORS,
)
from .pins import (
    LCD_BL_PIN,
    LCD_CS_PIN,
    LCD_DC_PIN,
    LCD_MISO_PIN,
    LCD_MOSI_PIN,
    LCD_RST_PIN,
    LCD_SCK_PIN,
    TOUCH_I2C_SCL,
    TOUCH_I2C_SDA,
    TP_INT_PIN,
    TP_RST_PIN,
)


def try_init_touch():
    try:
        from app import touchscreen_test as touch

        touch_dev = touch.Touch_CST328(
            i2c_sda=TOUCH_I2C_SDA,
            i2c_scl=TOUCH_I2C_SCL,
            irq_pin=TP_INT_PIN,
            rst_pin=TP_RST_PIN,
        )
        print("Touch init ok.")
        return touch_dev
    except Exception as exc:
        print(f"Touch init failed (optional): {exc}")
        return None


def try_init_display():
    try:
        from adafruit_display_text import label
        from adafruit_st7789 import ST7789
        from fourwire import FourWire
        import terminalio

        displayio.release_displays()
        spi = busio.SPI(clock=LCD_SCK_PIN, MOSI=LCD_MOSI_PIN, MISO=LCD_MISO_PIN)
        while not spi.try_lock():
            pass
        spi.configure(baudrate=LCD_SPI_BAUD)
        spi.unlock()
        display_bus = FourWire(spi, command=LCD_DC_PIN, chip_select=LCD_CS_PIN, reset=LCD_RST_PIN)
        display = ST7789(
            display_bus,
            width=DISPLAY_WIDTH,
            height=DISPLAY_HEIGHT,
            rotation=DISPLAY_ROTATION,
        )

        backlight = digitalio.DigitalInOut(LCD_BL_PIN)
        backlight.switch_to_output(value=True)

        cell_w = DISPLAY_WIDTH // GRID_COLS
        cell_h = DISPLAY_HEIGHT // GRID_ROWS
        label_h = 8 * LABEL_SCALE
        bar_w = max(cell_w // 3, 4)
        bar_h = max(cell_h - label_h - (BAR_PAD_Y * 2), 2)

        root = displayio.Group()
        display.root_group = root

        bg_bitmap = displayio.Bitmap(DISPLAY_WIDTH, DISPLAY_HEIGHT, 1)
        bg_palette = displayio.Palette(1)
        bg_palette[0] = BAR_BG
        root.append(displayio.TileGrid(bg_bitmap, pixel_shader=bg_palette))

        bar_palette = displayio.Palette(2)
        bar_palette[0] = BAR_BG
        bar_palette[1] = BAR_COLOR

        bar_bitmaps = []
        for idx in range(TOTAL_SENSORS):
            col = idx % GRID_COLS
            row = idx // GRID_COLS
            label_x = (col * cell_w) + LABEL_PAD_X
            bar_x = (col * cell_w) + (cell_w - bar_w) // 2
            bar_y = (row * cell_h) + BAR_PAD_Y
            label_y = bar_y + bar_h + LABEL_PAD_Y

            board_slot = idx // MAX_SENSORS
            board_id = SENSOR_NODE_DEVICE_IDS[board_slot] if board_slot < len(SENSOR_NODE_DEVICE_IDS) else 0
            sensor_id = idx % 32
            text = "%d-%02d" % (board_id, sensor_id)
            text_area = label.Label(
                terminalio.FONT,
                text=text,
                color=0xFFFFFF,
                x=label_x,
                y=label_y + (label_h // 2),
                scale=LABEL_SCALE,
            )
            root.append(text_area)

            bitmap = displayio.Bitmap(bar_w, bar_h, 2)
            tile = displayio.TileGrid(bitmap, pixel_shader=bar_palette, x=bar_x, y=bar_y)
            root.append(tile)
            bar_bitmaps.append(bitmap)

        return display, bar_bitmaps, bar_w, bar_h
    except Exception as exc:
        print(f"Display init failed (optional): {exc}")
        return None, None, 0, 0


def disable_display():
    try:
        displayio.release_displays()
    except Exception:
        pass
    try:
        backlight = digitalio.DigitalInOut(LCD_BL_PIN)
        backlight.switch_to_output(value=False)
    except Exception:
        pass


def update_bars(bitmaps, values, bar_w: int, bar_h: int) -> None:
    if not bitmaps:
        return
    for idx, bitmap in enumerate(bitmaps):
        value = values[idx] if idx < len(values) else 0
        level = int((value / DISPLAY_VALUE_MAX) * bar_h)
        if level < 0:
            level = 0
        if level > bar_h:
            level = bar_h
        bitmap.fill(0)
        if level == 0:
            continue
        start_y = bar_h - level
        for y in range(start_y, bar_h):
            for x in range(bar_w):
                bitmap[x, y] = 1
