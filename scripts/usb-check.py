"""Read X2600 USB descriptors and CPU information through the Cloner driver."""
from pathlib import Path

import usb.backend.libusb0
import usb.core
import usb.util

workspace = Path(__file__).resolve().parent.parent
driver_dll = (
    workspace
    / "tools/cloner-windows/cloner-2.5.54-windows_alpha"
    / "cloner-win32-driver/amd64/libusb0.dll"
)
backend = usb.backend.libusb0.get_backend(find_library=lambda name: str(driver_dll))
if backend is None:
    raise SystemExit(f"Cloner libusb0 backend unavailable: {driver_dll}")
device = usb.core.find(idVendor=0xA108, idProduct=0xEAEF, backend=backend)
if device is None:
    raise SystemExit("X2600 USB device A108:EAEF not found. Check USB Boot mode and the OTG data cable.")

# Standard GET_DESCRIPTOR and official VR_GET_CPU_INFO are control reads.
# No configuration change, firmware download, erase or Flash write is sent.
descriptor = bytes(device.ctrl_transfer(0x80, 0x06, 0x0100, 0, 18, timeout=2000))
cpu_info = bytes(device.ctrl_transfer(0xC0, 0x00, 0, 0, 5, timeout=2000))
print("USB device: A108:EAEF")
print("Device descriptor:", descriptor.hex())
print("CPU information:", cpu_info.decode("ascii", errors="replace"))
usb.util.dispose_resources(device)
