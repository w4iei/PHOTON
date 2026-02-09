import board
import busio
import digitalio
import displayio
import terminalio
from fourwire import FourWire
from adafruit_st7789 import ST7789
from adafruit_display_text import label

# Reference: https://docs.circuitpython.org/projects/st7789/en/latest/examples.html#id1


def main():
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
    inner_palette = displayio.Palette(1)
    inner_palette[0] = 0xAA0088  # Purple
    inner_sprite = displayio.TileGrid(inner_bitmap, pixel_shader=inner_palette, x=20, y=20)
    splash.append(inner_sprite)

    # Draw a label
    text_group = displayio.Group(scale=3, x=57, y=120)
    text = "Hello World!"
    text_area = label.Label(terminalio.FONT, text=text, color=0xFFFF00)
    text_group.append(text_area)  # Subgroup for text scaling
    splash.append(text_group)

    # Backlight on
    
    backlight = digitalio.DigitalInOut(LCD_BACKLIGHT)
    backlight.switch_to_output(value=True)

    while True:
        pass


if __name__ == '__main__':
    main()
