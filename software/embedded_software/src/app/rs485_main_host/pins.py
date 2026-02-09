"""Pin definitions for the RS-485 main host."""

try:
    import microcontroller
except Exception:  # pragma: no cover - host envs don't have CircuitPython modules
    microcontroller = None  # type: ignore

if microcontroller is None:
    UART_TX_PIN = None
    UART_RX_PIN = None
    RS485_DE_PIN = None
    RS485_TERM_PIN = None

    TP_INT_PIN = None
    TP_RST_PIN = None
    LCD_CS_PIN = None
    LCD_RST_PIN = None
    LCD_SCK_PIN = None
    LCD_MOSI_PIN = None
    LCD_MISO_PIN = None
    LCD_DC_PIN = None
    LCD_BL_PIN = None
    TOUCH_I2C_SDA = None
    TOUCH_I2C_SCL = None
else:
    UART_TX_PIN = microcontroller.pin.GPIO4
    UART_RX_PIN = microcontroller.pin.GPIO5
    RS485_DE_PIN = microcontroller.pin.GPIO1
    RS485_TERM_PIN = microcontroller.pin.GPIO18

    # LCD + touch pins (main host only).
    TP_INT_PIN = microcontroller.pin.GPIO2
    TP_RST_PIN = microcontroller.pin.GPIO3
    LCD_CS_PIN = microcontroller.pin.GPIO8
    LCD_RST_PIN = microcontroller.pin.GPIO9
    LCD_SCK_PIN = microcontroller.pin.GPIO10
    LCD_MOSI_PIN = microcontroller.pin.GPIO11
    LCD_MISO_PIN = microcontroller.pin.GPIO12
    LCD_DC_PIN = microcontroller.pin.GPIO14
    LCD_BL_PIN = microcontroller.pin.GPIO15
    TOUCH_I2C_SDA = microcontroller.pin.GPIO6
    TOUCH_I2C_SCL = microcontroller.pin.GPIO7
