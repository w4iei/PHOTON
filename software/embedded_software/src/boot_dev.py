import storage
import usb_cdc

# Enable REPL over USB serial
usb_cdc.enable(console=True, data=False)

try:
    from app import nvm_flags

    if nvm_flags.is_usb_drive_disabled():
        storage.disable_usb_drive()
        print("boot_dev.py: MSC disabled (NVM flag).")
    else:
        print("boot_dev.py: Development mode — MSC and REPL enabled.")
except Exception as exc:
    print("boot_dev.py: NVM flag check failed:", exc)
    print("boot_dev.py: Development mode — MSC and REPL enabled.")
