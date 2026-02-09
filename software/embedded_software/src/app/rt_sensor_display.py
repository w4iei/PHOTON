"""Render a live oscilloscope-style trace of one sensor on the ST7789 LCD."""

import time
import board
import busio
import digitalio
import displayio
import terminalio
import analogio
import microcontroller
from fourwire import FourWire
from adafruit_st7789 import ST7789
from adafruit_display_text import label

# Reference: https://docs.circuitpython.org/projects/st7789/en/latest/examples.html#id1


def main():
    # --- Configuration ---
    # NOTE: board.A0 is the standard alias for GPIO26/ADC0.
    SENSOR_ADC_PIN = board.A0
    SENSOR_ENABLE_PIN = microcontroller.pin.GPIO27
    GREEN_LED_PIN = microcontroller.pin.GPIO28
    RED_LED_PIN = microcontroller.pin.GPIO29
    SAMPLE_INTERVAL_S = 1/300  # 1/n Hz



    TP_INT_PIN = board.GP1
    # Reset GPIOs (may be shared between LCD + touch; override individually if needed)
    LCD_RESET_PIN = board.GP20
    TOUCH_RESET_PIN = board.GP2
    I2C_SDA = board.GP6
    I2C_SCL = board.GP7
    tft_cs = board.GP9
    LCD_SCK = board.GP10
    LCD_MOSI = board.GP11
    LCD_MISO = board.GP12
    tft_dc = board.GP14
    LCD_BACKLIGHT = board.GP15
    BACKLIGHT_ACTIVE_HIGH = True  # flip to False if hardware drives the LED on low
    RESET_ASSERT_LEVEL = False  # active-low reset line
    RESET_ASSERT_TIME_S = 0.015  # >=10 µs; keep generous for safety
    RESET_RELEASE_DELAY_S = 0.15  # ST7789T3 datasheet recommends >=120 ms
    LCD_SPI_BAUD = 24_000_000

    displayio.release_displays()
    spi = busio.SPI(clock=LCD_SCK, MOSI=LCD_MOSI, MISO=LCD_MISO)
    while not spi.try_lock():
        pass
    spi.configure(baudrate=24000000) # Configure SPI for 24MHz
    spi.unlock()

    display_bus = FourWire(spi, command=tft_dc, chip_select=tft_cs, reset=board.GP20)

    display = ST7789(display_bus, width=320, height=240, rotation=90)

    # Make the display context
    splash = displayio.Group()
    display.root_group = splash

    color_bitmap = displayio.Bitmap(320, 240, 1)
    color_palette = displayio.Palette(1)
    color_palette[0] = 0x00FF00  # Bright Green

    bg_sprite = displayio.TileGrid(color_bitmap, pixel_shader=color_palette, x=0, y=0)
    splash.append(bg_sprite)

    # Draw a smaller inner rectangle
    inner_bitmap = displayio.Bitmap(280, 200, 1)
    inner_palette = displayio.Palette(2)
    inner_palette[0] = 0x000000  # Black
    inner_palette[1] = 0xFFFF00  # Yellow trace
    inner_sprite = displayio.TileGrid(inner_bitmap, pixel_shader=inner_palette, x=20, y=20)
    splash.append(inner_sprite)

    # Labels on the outer frame (green background)
    inner_bottom_y = 20 + inner_bitmap.height
    label_color = 0x000000  # black on green

    top_max_label = label.Label(terminalio.FONT, text="MAX: --", color=label_color, x=5, y=12)
    splash.append(top_max_label)

    bottom_min_label = label.Label(
        terminalio.FONT, text="min: --", color=label_color, x=5, y=229
    )
    splash.append(bottom_min_label)

    # bottom_max_label = label.Label(
    #     terminalio.FONT, text="max: --", color=label_color, x=120, y=inner_bottom_y
    # )
    # splash.append(bottom_max_label)

    # time_left_label = label.Label(terminalio.FONT, text="0", color=label_color, x=5, y=239)
    # splash.append(time_left_label)

    # time_right_label = label.Label(
    #     terminalio.FONT, text=str(inner_bitmap.width), color=label_color, x=290, y=239
    # )
    # splash.append(time_right_label)

    # Backlight on
    
    backlight = digitalio.DigitalInOut(LCD_BACKLIGHT)
    backlight.switch_to_output(value=True)

    # Sensor enable + ADC
    sensor_enable = digitalio.DigitalInOut(SENSOR_ENABLE_PIN)
    sensor_enable.direction = digitalio.Direction.OUTPUT
    sensor_enable.value = True

    adc = analogio.AnalogIn(SENSOR_ADC_PIN)

    inner_width = inner_bitmap.width
    inner_height = inner_bitmap.height
    current_column = 0
    min_seen = None
    max_seen = None
    sample_buffer = [None] * inner_width  # last value drawn in each column
    column_rows = [None] * inner_width  # last row lit in each column

    def value_to_row(value, min_value, max_value):
        span = max_value - min_value
        if span <= 0:
            return inner_height // 2
        normalized = (value - min_value) / span
        normalized = min(1.0, max(0.0, normalized))
        return inner_height - 1 - int(normalized * (inner_height - 1))

    def redraw_all():
        for x, sample in enumerate(sample_buffer):
            if sample is None:
                continue
            new_row = value_to_row(sample, min_seen, max_seen)
            previous_row = column_rows[x]
            if previous_row is not None and previous_row != new_row:
                inner_bitmap[x, previous_row] = 0
            inner_bitmap[x, new_row] = 1
            column_rows[x] = new_row
        update_value_labels()

    def update_value_labels():
        min_text = f"min: {min_seen}" if min_seen is not None else "min: --"
        max_text = f"max: {max_seen}" if max_seen is not None else "max: --"
        top_max_label.text = f"MAX: {max_seen}" if max_seen is not None else "MAX: --"
        bottom_min_label.text = min_text
        # bottom_max_label.text = max_text

    while True:
        adc_value = adc.value

        rescale_needed = False
        if min_seen is None:
            min_seen = max_seen = adc_value
            rescale_needed = True
        else:
            if adc_value < min_seen:
                min_seen = adc_value
                rescale_needed = True
            if adc_value > max_seen:
                max_seen = adc_value
                rescale_needed = True

        # Clear previous pixel in this column
        previous_row = column_rows[current_column]
        if previous_row is not None:
            inner_bitmap[current_column, previous_row] = 0

        # Draw the new pixel
        row = value_to_row(adc_value, min_seen, max_seen)
        inner_bitmap[current_column, row] = 1
        sample_buffer[current_column] = adc_value
        column_rows[current_column] = row

        current_column = (current_column + 1) % inner_width

        if rescale_needed:
            redraw_all()
        else:
            update_value_labels()

        time.sleep(SAMPLE_INTERVAL_S)


if __name__ == '__main__':
    main()
