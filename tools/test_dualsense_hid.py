"""Wired DualSense USB HID and WASAPI Audio Haptics verification tool.

Tests opening the wired Sony DualSense (VID 0x054C, PID 0x0CE6 / 0x0DF2) via
Win32 SetupAPI + HID + CreateFileW with shared read/write access, reading live
64-byte USB input reports, sending 48-byte USB output reports (lightbar, rumble,
adaptive triggers), and cleanly resetting controller state in a finally block.
"""

import ctypes
import ctypes.wintypes as wt
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
setupapi = ctypes.WinDLL("setupapi", use_last_error=True)
hid = ctypes.WinDLL("hid", use_last_error=True)

DIGCF_PRESENT = 0x00000002
DIGCF_DEVICEINTERFACE = 0x00000010
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
FILE_FLAG_OVERLAPPED = 0x40000000
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
ERROR_IO_PENDING = 997
WAIT_OBJECT_0 = 0x00000000

SONY_VID = 0x054C
DUALSENSE_PIDS = {0x0CE6: "DualSense", 0x0DF2: "DualSense Edge"}


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wt.DWORD),
        ("Data2", wt.WORD),
        ("Data3", wt.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", wt.DWORD),
        ("InterfaceClassGuid", GUID),
        ("Flags", wt.DWORD),
        ("Reserved", ctypes.c_size_t),
    ]


class HIDD_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("Size", wt.ULONG),
        ("VendorID", wt.USHORT),
        ("ProductID", wt.USHORT),
        ("VersionNumber", wt.USHORT),
    ]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [
        ("Usage", wt.USHORT),
        ("UsagePage", wt.USHORT),
        ("InputReportByteLength", wt.USHORT),
        ("OutputReportByteLength", wt.USHORT),
        ("FeatureReportByteLength", wt.USHORT),
        ("Reserved", wt.USHORT * 17),
        ("NumberLinkCollectionNodes", wt.USHORT),
        ("NumberInputButtonCaps", wt.USHORT),
        ("NumberInputValueCaps", wt.USHORT),
        ("NumberInputDataIndices", wt.USHORT),
        ("NumberOutputButtonCaps", wt.USHORT),
        ("NumberOutputValueCaps", wt.USHORT),
        ("NumberOutputDataIndices", wt.USHORT),
        ("NumberFeatureButtonCaps", wt.USHORT),
        ("NumberFeatureValueCaps", wt.USHORT),
        ("NumberFeatureDataIndices", wt.USHORT),
    ]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal", ctypes.c_size_t),
        ("InternalHigh", ctypes.c_size_t),
        ("Offset", wt.DWORD),
        ("OffsetHigh", wt.DWORD),
        ("hEvent", wt.HANDLE),
    ]


# Function prototypes (critical for 64-bit handles)
hid.HidD_GetHidGuid.argtypes = [ctypes.POINTER(GUID)]
hid.HidD_GetHidGuid.restype = None

hid.HidD_GetAttributes.argtypes = [wt.HANDLE, ctypes.POINTER(HIDD_ATTRIBUTES)]
hid.HidD_GetAttributes.restype = wt.BOOLEAN

hid.HidD_GetPreparsedData.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p)]
hid.HidD_GetPreparsedData.restype = wt.BOOLEAN

hid.HidD_FreePreparsedData.argtypes = [ctypes.c_void_p]
hid.HidD_FreePreparsedData.restype = wt.BOOLEAN

hid.HidP_GetCaps.argtypes = [ctypes.c_void_p, ctypes.POINTER(HIDP_CAPS)]
hid.HidP_GetCaps.restype = wt.LONG

setupapi.SetupDiGetClassDevsW.argtypes = [
    ctypes.POINTER(GUID), wt.LPCWSTR, wt.HWND, wt.DWORD
]
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p

setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID),
    wt.DWORD, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA)
]
setupapi.SetupDiEnumDeviceInterfaces.restype = wt.BOOL

setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    ctypes.c_void_p, ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
    ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.c_void_p
]
setupapi.SetupDiGetDeviceInterfaceDetailW.restype = wt.BOOL

setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
setupapi.SetupDiDestroyDeviceInfoList.restype = wt.BOOL

kernel32.CreateFileW.argtypes = [
    wt.LPCWSTR, wt.DWORD, wt.DWORD, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.HANDLE
]
kernel32.CreateFileW.restype = ctypes.c_void_p

kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CloseHandle.restype = wt.BOOL

kernel32.ReadFile.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)
]
kernel32.ReadFile.restype = wt.BOOL

kernel32.WriteFile.argtypes = [
    wt.HANDLE, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD), ctypes.POINTER(OVERLAPPED)
]
kernel32.WriteFile.restype = wt.BOOL

kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
kernel32.CreateEventW.restype = wt.HANDLE

kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.WaitForSingleObject.restype = wt.DWORD

kernel32.GetOverlappedResult.argtypes = [
    wt.HANDLE, ctypes.POINTER(OVERLAPPED), ctypes.POINTER(wt.DWORD), wt.BOOL
]
kernel32.GetOverlappedResult.restype = wt.BOOL

kernel32.CancelIo.argtypes = [wt.HANDLE]
kernel32.CancelIo.restype = wt.BOOL


def enumerate_dualsense_hid_paths():
    hid_guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(hid_guid))

    dev_info = setupapi.SetupDiGetClassDevsW(
        ctypes.byref(hid_guid), None, None, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    )
    if not dev_info or dev_info == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())

    results = []
    try:
        idx = 0
        iface_data = SP_DEVICE_INTERFACE_DATA()
        iface_data.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
        while setupapi.SetupDiEnumDeviceInterfaces(
            dev_info, None, ctypes.byref(hid_guid), idx, ctypes.byref(iface_data)
        ):
            idx += 1
            req_size = wt.DWORD(0)
            setupapi.SetupDiGetDeviceInterfaceDetailW(
                dev_info, ctypes.byref(iface_data), None, 0, ctypes.byref(req_size), None
            )
            if req_size.value == 0:
                continue

            buf = ctypes.create_string_buffer(req_size.value)
            # On 64-bit Windows, SP_DEVICE_INTERFACE_DETAIL_DATA_W.cbSize is 8
            cb_size = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = cb_size

            if not setupapi.SetupDiGetDeviceInterfaceDetailW(
                dev_info, ctypes.byref(iface_data), buf, req_size.value, None, None
            ):
                continue

            path_ptr = ctypes.c_void_p(ctypes.addressof(buf) + 4)
            device_path = ctypes.wstring_at(path_ptr)
            if "vid_054c" not in device_path.lower():
                continue

            handle = kernel32.CreateFileW(
                device_path,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                None,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                None,
            )
            if not handle or handle == INVALID_HANDLE_VALUE:
                err = ctypes.get_last_error()
                results.append({"path": device_path, "open_error": err})
                continue

            try:
                attrs = HIDD_ATTRIBUTES()
                attrs.Size = ctypes.sizeof(HIDD_ATTRIBUTES)
                if not hid.HidD_GetAttributes(handle, ctypes.byref(attrs)):
                    continue
                if attrs.VendorID != SONY_VID or attrs.ProductID not in DUALSENSE_PIDS:
                    continue

                caps = HIDP_CAPS()
                ppd = ctypes.c_void_p()
                if hid.HidD_GetPreparsedData(handle, ctypes.byref(ppd)):
                    try:
                        hid.HidP_GetCaps(ppd, ctypes.byref(caps))
                    finally:
                        hid.HidD_FreePreparsedData(ppd)

                results.append({
                    "path": device_path,
                    "name": DUALSENSE_PIDS[attrs.ProductID],
                    "vid": attrs.VendorID,
                    "pid": attrs.ProductID,
                    "version": attrs.VersionNumber,
                    "usage_page": caps.UsagePage,
                    "usage": caps.Usage,
                    "in_len": caps.InputReportByteLength,
                    "out_len": caps.OutputReportByteLength,
                    "feat_len": caps.FeatureReportByteLength,
                })
            finally:
                kernel32.CloseHandle(handle)
    finally:
        setupapi.SetupDiDestroyDeviceInfoList(dev_info)
    return results


def overlapped_read(handle, size, timeout_ms=2000):
    buf = (ctypes.c_ubyte * size)()
    ov = OVERLAPPED()
    ov.hEvent = kernel32.CreateEventW(None, True, False, None)
    if not ov.hEvent:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        read_bytes = wt.DWORD(0)
        ok = kernel32.ReadFile(handle, buf, size, ctypes.byref(read_bytes), ctypes.byref(ov))
        if not ok:
            err = ctypes.get_last_error()
            if err != ERROR_IO_PENDING:
                raise ctypes.WinError(err)
            wait = kernel32.WaitForSingleObject(ov.hEvent, timeout_ms)
            if wait != WAIT_OBJECT_0:
                kernel32.CancelIo(handle)
                raise TimeoutError(f"ReadFile timed out after {timeout_ms} ms (wait={wait})")
            if not kernel32.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(read_bytes), False):
                raise ctypes.WinError(ctypes.get_last_error())
        return bytes(buf[: read_bytes.value])
    finally:
        kernel32.CloseHandle(ov.hEvent)


def overlapped_write(handle, data, timeout_ms=2000):
    buf = (ctypes.c_ubyte * len(data))(*data)
    ov = OVERLAPPED()
    ov.hEvent = kernel32.CreateEventW(None, True, False, None)
    if not ov.hEvent:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        written = wt.DWORD(0)
        ok = kernel32.WriteFile(handle, buf, len(data), ctypes.byref(written), ctypes.byref(ov))
        if not ok:
            err = ctypes.get_last_error()
            if err != ERROR_IO_PENDING:
                raise ctypes.WinError(err)
            wait = kernel32.WaitForSingleObject(ov.hEvent, timeout_ms)
            if wait != WAIT_OBJECT_0:
                kernel32.CancelIo(handle)
                raise TimeoutError(f"WriteFile timed out after {timeout_ms} ms (wait={wait})")
            if not kernel32.GetOverlappedResult(handle, ctypes.byref(ov), ctypes.byref(written), False):
                raise ctypes.WinError(ctypes.get_last_error())
        return written.value
    finally:
        kernel32.CloseHandle(ov.hEvent)


def build_usb_output_report(
    rumble_left=0,
    rumble_right=0,
    r2_mode=0x05,
    r2_params=None,
    l2_mode=0x05,
    l2_params=None,
    rgb=(0, 0, 0),
    player_leds=0x00,
    enable_lightbar=True,
    enable_player_leds=True,
):
    buf = bytearray(48)
    buf[0] = 0x02  # USB Output Report ID
    buf[1] = 0x0F  # valid_flag0: compat rumble (0x03) + R2 trigger (0x04) + L2 trigger (0x08)
    valid1 = 0x00
    if enable_lightbar:
        valid1 |= 0x04
    if enable_player_leds:
        valid1 |= 0x10
    buf[2] = valid1
    buf[3] = max(0, min(255, rumble_right))
    buf[4] = max(0, min(255, rumble_left))

    # Right Trigger (R2) starts at offset 11 (mode) + 10 param bytes
    buf[11] = r2_mode & 0xFF
    if r2_params:
        for i, val in enumerate(r2_params[:10]):
            buf[12 + i] = val & 0xFF

    # Left Trigger (L2) starts at offset 22 (mode) + 10 param bytes
    buf[22] = l2_mode & 0xFF
    if l2_params:
        for i, val in enumerate(l2_params[:10]):
            buf[23 + i] = val & 0xFF

    buf[42] = 0x02  # lightbar setup (LED_CONTROL)
    buf[43] = 0x00  # pulse options
    buf[44] = player_leds & 0x1F
    buf[45] = max(0, min(255, rgb[0]))
    buf[46] = max(0, min(255, rgb[1]))
    buf[47] = max(0, min(255, rgb[2]))
    return bytes(buf)


def main():
    print("=== Enumerating Sony DualSense HID Interfaces ===")
    devices = enumerate_dualsense_hid_paths()
    if not devices:
        print("ERROR: No Sony HID device found.")
        return 1

    usb_gamepad = None
    for d in devices:
        if "open_error" in d:
            print(f"Skipped (open error {d['open_error']}): {d['path']}")
            continue
        print(
            f"Found {d['name']} VID=0x{d['vid']:04X} PID=0x{d['pid']:04X} "
            f"UsagePage=0x{d['usage_page']:02X} Usage=0x{d['usage']:02X} "
            f"In={d['in_len']} Out={d['out_len']} Feat={d['feat_len']}\n"
            f"  Path: {d['path']}"
        )
        if d["in_len"] == 64 and d["out_len"] == 48 and d["usage_page"] == 0x01:
            usb_gamepad = d

    if not usb_gamepad:
        print("ERROR: Wired USB DualSense (In=64, Out=48) not found. Is Bluetooth-only connected?")
        return 2

    handle = kernel32.CreateFileW(
        usb_gamepad["path"],
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        None,
    )
    if not handle or handle == INVALID_HANDLE_VALUE:
        err = ctypes.get_last_error()
        print(f"ERROR: Could not reopen DualSense handle (winerr={err})")
        return 3

    try:
        print("\n=== Reading USB Input Report (0x01) ===")
        in_report = overlapped_read(handle, usb_gamepad["in_len"], timeout_ms=2000)
        print(f"Read {len(in_report)} bytes, ReportID=0x{in_report[0]:02X}")
        if len(in_report) >= 12 and in_report[0] == 0x01:
            lx, ly, rx, ry, l2, r2 = in_report[1], in_report[2], in_report[3], in_report[4], in_report[5], in_report[6]
            btns = int.from_bytes(in_report[8:11], "little")
            print(f"  Sticks: LX={lx} LY={ly} RX={rx} RY={ry} | Triggers: L2={l2} R2={r2} | ButtonsMask=0x{btns:06X}")

        print("\n=== Testing USB Output Report (0x02): Lightbar + Gentle Rumble + Triggers ===")
        # Step A: Cyan lightbar + light rumble + R2/L2 continuous resistance
        test_pkt = build_usb_output_report(
            rumble_left=40,
            rumble_right=40,
            r2_mode=0x01,  # Continuous resistance
            r2_params=[0x20, 0x90],  # start zone, force
            l2_mode=0x02,  # Section/weapon resistance
            l2_params=[0x20, 0x60, 0xA0],
            rgb=(0, 180, 255),
            player_leds=0x04,
        )
        written = overlapped_write(handle, test_pkt)
        print(f"Wrote test Output Report: {written} bytes")
        time.sleep(0.25)

        # Step B: Gold lightbar, rumble off, keep trigger test briefly
        test_pkt2 = build_usb_output_report(
            rumble_left=0,
            rumble_right=0,
            r2_mode=0x01,
            r2_params=[0x20, 0x90],
            l2_mode=0x02,
            l2_params=[0x20, 0x60, 0xA0],
            rgb=(255, 170, 0),
            player_leds=0x0E,
        )
        written2 = overlapped_write(handle, test_pkt2)
        print(f"Wrote step 2 Output Report: {written2} bytes")
        time.sleep(0.25)
    finally:
        # CRITICAL: Always reset controller to neutral state
        try:
            reset_pkt = build_usb_output_report(
                rumble_left=0,
                rumble_right=0,
                r2_mode=0x05,  # Off
                l2_mode=0x05,  # Off
                rgb=(0, 0, 0),
                player_leds=0x00,
                enable_lightbar=True,
                enable_player_leds=True,
            )
            written_rst = overlapped_write(handle, reset_pkt)
            print(f"Reset Output Report sent cleanly ({written_rst} bytes).")
        except Exception as exc:
            print(f"WARNING: Failed to send reset packet: {exc}")
        kernel32.CloseHandle(handle)

    print("DualSense HID hardware test PASSED.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
