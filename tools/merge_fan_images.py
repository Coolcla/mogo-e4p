#!/usr/bin/env python3
"""Merge and audit the two Fan firmware profiles into 0x0-start images."""

from __future__ import annotations

import hashlib
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo


ROOT = Path(__file__).resolve().parents[1]
BUILDS = ROOT / "build"
OUTPUTS = ROOT / "outputs"
ORIGINAL = BUILDS / "fan-inputs" / "mogo-e4p.bin"
REFERENCE_FULL = BUILDS / "fan-inputs" / "Fan-usb-full.bin"
APP_OFFSET = 0x10000
APP_CAPACITY = 5 * 1024 * 1024
SEGMENTS = (
    (0x2000, "bootloader/bootloader.bin"),
    (0x8000, "partition_table/partition-table.bin"),
    (0xD000, "ota_data_initial.bin"),
    (APP_OFFSET, "Fan.bin"),
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_segments(profile: str) -> list[tuple[int, bytes, str]]:
    base = BUILDS / profile
    return [(offset, (base / name).read_bytes(), name) for offset, name in SEGMENTS]


def partition_layout(data: bytes) -> list[tuple[int, int, int, int, str, int]]:
    """Return logical partition records, ignoring IDF table padding/checksum bytes."""
    rows = []
    for pos in range(0, len(data) - 31, 32):
        row = data[pos:pos + 32]
        magic = int.from_bytes(row[:2], "little")
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            raise ValueError(f"invalid partition-table record at 0x{pos:x}")
        label = row[12:28].split(b"\0", 1)[0].decode("ascii", errors="strict")
        rows.append((row[2], row[3], int.from_bytes(row[4:8], "little"),
                     int.from_bytes(row[8:12], "little"), label,
                     int.from_bytes(row[28:32], "little")))
    if not rows:
        raise ValueError("partition table is empty")
    return rows


def partition_at_0x8000(image: bytes) -> bytes:
    if len(image) < 0x9000:
        raise ValueError("reference image is too small to contain a partition table")
    return image[0x8000:0x9000]


def merge(profile: str) -> tuple[bytes, int, str]:
    segments = load_segments(profile)
    if len(segments[0][1]) > segments[1][0] - segments[0][0]:
        raise ValueError(f"{profile}: bootloader overlaps the partition table")
    if len(segments[-1][1]) > APP_CAPACITY:
        raise ValueError(f"{profile}: app exceeds the 5 MiB OTA partition")
    image_end = max(offset + len(data) for offset, data, _ in segments)
    image = bytearray(b"\xff" * image_end)
    for offset, data, _ in segments:
        image[offset : offset + len(data)] = data
    merged = bytes(image)

    # Verify each flash segment and every gap before the app.
    cursor = 0
    for offset, data, name in segments:
        assert merged[offset : offset + len(data)] == data, name
        assert merged[cursor:offset] == b"\xff" * (offset - cursor), f"gap before {name}"
        cursor = offset + len(data)
    return merged, len(segments[-1][1]), sha256(merged)


def main() -> None:
    OUTPUTS.mkdir(exist_ok=True)
    original = ORIGINAL.read_bytes()
    new_partition = (BUILDS / "fan-dfs" / SEGMENTS[1][1]).read_bytes()
    old_layout = partition_layout(partition_at_0x8000(original))
    reference_full = REFERENCE_FULL.read_bytes()
    reference_layout = partition_layout(partition_at_0x8000(reference_full))
    new_layout = partition_layout(new_partition)
    assert old_layout == new_layout, "logical partition layout differs from the Fan.zip base"
    assert reference_layout == new_layout, "logical partition layout differs from Fan-usb-full.bin"
    assert new_layout == [
        (1, 0, 0xD000, 0x2000, "otadata", 0),
        (1, 1, 0xF000, 0x1000, "phy_init", 0),
        (0, 0x10, 0x10000, APP_CAPACITY, "ota_0", 0),
        (0, 0x11, 0x510000, APP_CAPACITY, "ota_1", 0),
        (1, 0x81, 0xA10000, 0x5E8000, "storage", 0),
        (1, 2, 0xFF8000, 0x8000, "nvs", 0),
    ], "partition layout is not the expected S31 dual-OTA layout"
    assert new_partition == (BUILDS / "fan-no-dfs" / SEGMENTS[1][1]).read_bytes(), (
        "partition tables differ between profiles"
    )
    assert (BUILDS / "fan-dfs" / SEGMENTS[2][1]).read_bytes() == (
        BUILDS / "fan-no-dfs" / SEGMENTS[2][1]
    ).read_bytes(), "OTA initialization data differ between profiles"

    results: dict[str, tuple[bytes, int, str]] = {}
    for profile, filename in (("fan-dfs", "fan.bin"), ("fan-no-dfs", "fan-no-dfs.bin")):
        merged, app_size, digest = merge(profile)
        (OUTPUTS / filename).write_bytes(merged)
        results[filename] = (merged, app_size, digest)

    (OUTPUTS / "sha256.txt").write_text(
        "".join(f"{digest}  {filename}\n" for filename, (_, _, digest) in results.items()),
        encoding="ascii",
    )

    input_hashes = {}
    for line in (BUILDS / "fan-inputs" / "input-sha256.txt").read_text(encoding="ascii").splitlines():
        digest, filename = line.split(maxsplit=1)
        input_hashes[filename] = digest

    updated_fan = (ROOT / "assets" / "image" / "fan.png").read_bytes()
    assert updated_fan[:8] == b"\x89PNG\r\n\x1a\n", "updated fan.png is not PNG"
    updated_fan_size = (
        int.from_bytes(updated_fan[16:20], "big"),
        int.from_bytes(updated_fan[20:24], "big"),
    )

    dfs_app = results["fan.bin"][1]
    fixed_app = results["fan-no-dfs.bin"][1]
    dfs_bootloader = (BUILDS / "fan-dfs" / SEGMENTS[0][1]).read_bytes()
    fixed_bootloader = (BUILDS / "fan-no-dfs" / SEGMENTS[0][1]).read_bytes()
    font_subset = (ROOT / "main" / "assets" / "builtin.ttf").stat().st_size
    font_resource = (BUILDS / "fan-dfs" / "builtin_images" / "builtin.ttf.z").stat().st_size
    tzdata = BUILDS / "fan-inputs" / "tzdata-py" / "tzdata" / "zoneinfo"
    with (tzdata / "America" / "Vancouver").open("rb") as zone_file:
        vancouver = ZoneInfo.from_file(zone_file, key="America/Vancouver")
    build_date = datetime.now(vancouver).strftime("%Y-%m-%d %H:%M:%S")
    lines = [
        "Fan firmware build record",
        f"Build date: {build_date} America/Vancouver",
        "Source: chopper1026/mogo-e4p",
        "Target: esp32s31 --preview",
        r"ESP-IDF: v6.1.1 (C:\esp\v6.1.1\esp-idf)",
        r"Compiler: riscv32-esp-elf GCC 15.2.0 (crosstool-NG esp-15.2.0_20251204; C:\Espressif)",
        "App version: 1.1.8",
        "Flash mode/frequency: DIO / 80 MHz / 16 MiB declaration",
        "",
        "Input assets",
        f"mogo-e4p.bin: {len(original):,} bytes, SHA256 {input_hashes['mogo-e4p.bin']}",
        f"Fan-usb-full.bin reference: {len(reference_full):,} bytes, SHA256 {sha256(reference_full)}",
        f"startup.png: 684 x 1216, SHA256 {input_hashes['startup.png']}",
        f"NotoSansCJK.ttf: 36,143,328 bytes, SHA256 {input_hashes['NotoSansCJK.ttf']}",
        f"Fan.zip fan.png reference: 271 x 182, SHA256 {input_hashes['fan.png']}",
        f"Updated fan.png: {updated_fan_size[0]} x {updated_fan_size[1]}, {len(updated_fan):,} bytes, SHA256 {sha256(updated_fan)}",
        "",
        "Common image layout",
        f"bootloader at 0x2000: each profile uses its own build ({len(dfs_bootloader):,} bytes); DFS SHA256 {sha256(dfs_bootloader)}; no-DFS SHA256 {sha256(fixed_bootloader)}",
        "partition table: 0x8000 (logical records match Fan.zip and Fan-usb-full.bin)",
        "OTA initialization data: 0xd000",
        "application: 0x10000",
        "OTA slots: 0x10000 and 0x510000, each 5 MiB",
        f"App partition: {APP_CAPACITY // (1024 * 1024)} MiB",
        f"DFS app: 0x{dfs_app:x} bytes, 0x{APP_CAPACITY - dfs_app:x} bytes free",
        f"Fixed-frequency app: 0x{fixed_app:x} bytes, 0x{APP_CAPACITY - fixed_app:x} bytes free",
        f"Built-in TTF subset: {font_subset:,} bytes at wght=500; compressed resource {font_resource:,} bytes",
        "",
        "Outputs (both are 0x0-start merged images, not full-flash dumps)",
        f"fan.bin: {len(results['fan.bin'][0]):,} bytes, SHA256 {results['fan.bin'][2]}",
        "  DFS enabled (CONFIG_PM_ENABLE=y, CONFIG_PM_DFS_INIT_AUTO=y), CPU target 320 MHz; tickless idle disabled.",
        f"fan-no-dfs.bin: {len(results['fan-no-dfs.bin'][0]):,} bytes, SHA256 {results['fan-no-dfs.bin'][2]}",
        "  DFS disabled (CONFIG_PM_ENABLE=n), fixed CPU 320 MHz; tickless idle disabled.",
        "",
        "Keyboard: English/Zhuyin modes show one number row; numeric mode uses its own digit row without the duplicate top row or retype key. Keys use bold wght=500; password inputs have show/hide controls; KOReader and WebDAV fields use Wi-Fi-style immediate input display; the space and 中/En keys have shared, non-overlapping draw, hit and dirty-refresh bounds.",
        "WebDAV: custom HTTPS server URL, username and password are persisted in NVS; first use opens setup and later use connects directly. Supports browsing, upload, download to /sdcard/WebDAV, remote rename (MOVE) and delete. The book import page can browse WebDAV and import/open EPUB, TXT and MOBI in the configured books directory; remote books and folders support management actions.",
        "Credentials and settings backup: v50 saves Wi-Fi profiles, WebDAV server/user/password, Readest access/refresh tokens and account ID, KOSync server/user/password hash/device ID, and the complete reading direction. Restoring writes credentials back to the service NVS namespaces for automatic loading; v48/v49 backups remain readable. The backup file is not encrypted.",
        "Profile: editable personal name (default 凡繁煩) is shown by the avatar; the status-bar signature remains empty.",
        "USB MSC: host descriptors identify Fan E4P SD / Fan TF Card. SD probe busy results remain retryable; startup reports missing card, unmounted volume and timeout separately. A connected-but-unconfigured USB cable is not distinguishable from a missing data cable on the device, so the page advises checking the cable and host port.",
        "USB stability: MSC uses mogo-e4p's original independent SDMMC host, clock and pin setup, with the board's 4 KiB internal DMA relay applied to protect unsafe buffers; the high-speed TinyUSB MSC task uses an 8 KiB stack for nested SDMMC callbacks.",
        "Wi-Fi book transfer: TF card uploads are limited to 50 MiB per book by both firmware and the upload page; built-in flash keeps its 1 MiB per-file limit, and available space is checked independently.",
        "Reading sync: Readest Cloud device-code OAuth and KOReader KOSync are configured independently; closing/leaving a book queues an upload, and opening a book queues two-way comparison when a provider is configured. KOSync document IDs use KOReader-compatible partial MD5 samples at offsets 0, 1024, 4096, ...; Readest's existing book identity is unchanged.",
        "  Sync work acquires the configured network automatically and releases its lease on completion; calibrated system time is required. A six-entry in-memory queue keeps recent book positions while another request is active; connection failures retry after 30 seconds. KOSync/Readest receive a chapter-level DocFragment XPointer plus percentage; exact intra-chapter cross-engine position is approximate.",
        "  Cloud protocol exchange and physical-device sync behavior have not been exercised without an E4P runtime/account session.",
        "WebDAV: listing metadata staging is stored in PSRAM; the former 24-item local array was about 9.4 KiB and exceeded the 8 KiB WebDAV worker stack.",
        "Network lifecycle: one-shot time, weather/location, WebDAV, reading sync and CloudZao jobs acquire the saved route and release their network lease on success, failure or cancellation. A network-only Wi-Fi connection started by a job is stopped when its last lease is returned; the long-running Wi-Fi book-transfer session remains active until the user ends it.",
        "Simplified-to-traditional book conversion is paused and omitted from the firmware build; source files are retained for future resumption.",
        "Acceptance: both firmware profiles built; UI font coverage, image generation and transfer webpage interaction were checked. WebDAV crash remediation is build-verified but still needs physical-device confirmation.",
        "USB MSC DMA-buffer and task-stack hardening is build-verified; USB enumeration and host file access still need physical-device confirmation.",
        "  Lower Mainland weather scenarios and all Open-Meteo WMO condition translations are covered in host source; weather raster and vertical glyph host C tests could not run because no native C compiler is installed.",
        "  Native keyboard rendering and complete offline EPUB/TXT sample-book tests remain pending because no native C compiler or E4P runtime is installed.",
        "The E4P was not flashed. Physical-device validation remains pending.",
        "",
    ]
    (OUTPUTS / "build-info.txt").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print("\n".join(f"{name}: {len(data)} bytes SHA256 {digest}" for name, (data, _, digest) in results.items()))


if __name__ == "__main__":
    main()
