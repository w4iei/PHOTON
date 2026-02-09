# Production: obey NVM flags; keep console enabled for logs/support.
import storage
import usb_cdc

usb_cdc.enable(console=True, data=False)  # set to False to hard-disable REPL

try:
    from app.helpers import nvm_flags

    if nvm_flags.is_usb_drive_disabled():
        storage.disable_usb_drive()
        print("boot_prod.py: MSC disabled (NVM flag).")
    else:
        print("boot_prod.py: MSC enabled (NVM flag).")
except Exception as exc:
    storage.disable_usb_drive()
    print("boot_prod.py: NVM flag check failed:", exc)
    print("boot_prod.py: MSC disabled (fallback).")
