"""Package existing, validated firmware for a manual GitHub Release. No network calls."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
FLASH_FILES = {
    "0x0": "bootloader/bootloader.bin",
    "0x8000": "partition_table/partition-table.bin",
    "0xd000": "ota_data_initial.bin",
    "0x20000": "xiaozhi.bin",
}
UPDATE_FILES = ("firmware.dfu", "radio.bin", "resources.ths", "splash.bin",
                "update.fuf", "updater.bin")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2) + "\n").encode("utf-8")


def safe_file(base, name):
    path = PurePosixPath(name)
    if (path.is_absolute() or not path.parts or ".." in path.parts
            or "\\" in name or ":" in name):
        raise ValueError("Nonportable or escaping manifest path")
    resolved = (base / path).resolve()
    if not resolved.is_relative_to(base.resolve()) or not resolved.is_file():
        raise ValueError("Manifest file missing or outside release directory")
    return resolved


def esp_payload(base):
    manifest = json.loads((base / "flasher_args.json").read_text(encoding="utf-8"))
    for name in manifest["flash_files"].values():
        safe_file(base, name)
    if manifest["flash_files"] != FLASH_FILES:
        raise ValueError("Expected DeepFlipper S3 N16R8 flash layout")
    settings = {"flash_mode": "dio", "flash_size": "16MB", "flash_freq": "80m"}
    if (manifest["flash_settings"] != settings
            or manifest["extra_esptool_args"]["chip"] != "esp32s3"):
        raise ValueError("Expected ESP32-S3 DIO/80m/16MB")
    merged = safe_file(base, "merged-binary.bin").read_bytes()
    if not merged or merged[0] != 0xE9 or len(merged) > 16 * 1024 * 1024:
        raise ValueError("Invalid merged image")
    # Never distribute a device NVS readback in the factory image.
    if merged[0x9000:0xd000] != b"\xff" * 0x4000:
        raise ValueError("Factory image contains nonblank NVS")
    payload = {"merged-binary.bin": merged}
    for offset, name in FLASH_FILES.items():
        data = safe_file(base, name).read_bytes()
        start = int(offset, 16)
        if not data or merged[start:start + len(data)] != data:
            raise ValueError("Merged image differs from flash component: " + name)
        payload[name] = data
    # Recreate public flashing metadata, rather than publishing arbitrary fields.
    public = {
        "write_flash_args": ["--flash-mode", "dio", "--flash-size", "16MB",
                             "--flash-freq", "80m"],
        "flash_settings": settings, "flash_files": FLASH_FILES,
        "extra_esptool_args": {"chip": "esp32s3", "before": "default-reset",
                               "after": "hard-reset", "stub": True},
    }
    payload["flasher_args.json"] = json_bytes(public)
    payload["烧录说明.md"] = FLASH_NOTES.encode("utf-8")
    return payload


FLASH_NOTES = """# ESP32-S3 N16R8 烧录

仅适用项目 deepflipper-s3 参考接线、16MB Flash／8MB Octal PSRAM。
解压 ZIP 后执行；无需 ESP-IDF，仅需 Python + esptool 或独立下载工具。
先断开 Flipper 供电，避免 USB 与 GPIO 5V 直接并联。替换实际 COM 口。

```powershell
python -m pip install esptool
# 首次安装／不同分区：重置 Wi-Fi、网页登录密码和设备身份
python -m esptool --chip esp32s3 --port COM42 --baud 460800 write-flash 0x0 merged-binary.bin
```

自动进入下载模式失败时：按住 BOOT、点按 RESET，再松开 BOOT。

仅当分区相同且仍使用初始 ota_0 槽时，可保留 NVS 升级应用：

```powershell
python -m esptool --chip esp32s3 --port COM42 --baud 460800 write-flash 0x20000 xiaozhi.bin
```

不要全片擦除。OTA 选槽改变或不确定时使用完整安装，然后重新配网、导出身份，
在服务端重新登记设备并导入新连接配置。完整镜像覆盖 NVS，不能用它保留旧配置。
完整镜像与分散镜像二选一，不要把所有 BIN 都写到 0x0。
分散镜像地址见 flasher_args.json；DIO／80MHz／16MB。
同时更新配套 Flipper TGZ。初始热点密码／设备网页登录密码见
Flipper Settings → AI Assistant → Setup 的 AP pass；没有统一默认密码。
"""


def check_flipper(base):
    archive = base / "flipper-z-f7-update-local.tgz"
    prefix = "f7-update-local/"
    with tarfile.open(archive, "r:gz") as tar:
        members = tar.getmembers()
        files = [m for m in members if m.isfile()]
        if (len(files) != len(UPDATE_FILES)
                or {m.name for m in files} != {prefix + n for n in UPDATE_FILES}
                or any(not (m.isfile() or (m.isdir() and m.name.rstrip("/")
                                           == "f7-update-local")) for m in members)):
            raise ValueError("Unexpected files in Flipper TGZ")
        for member in files:
            data = tar.extractfile(member).read()
            if data != (base / member.name).read_bytes():
                raise ValueError("Flipper TGZ is stale: " + member.name)
    return archive


def git_output(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def source_state(root, allow_dirty):
    dirty = bool(git_output(root, "status", "--porcelain", "--untracked-files=all").strip())
    if dirty and not allow_dirty:
        raise ValueError("Working tree is dirty; commit first, or use --allow-dirty for a local preview")
    paths = sorted(set(git_output(root, "ls-files", "-z", "--cached", "--others",
                                 "--exclude-standard").split(b"\0")) - {b""})
    digest = hashlib.sha256()
    count = 0
    for raw in paths:
        path = root / raw.decode("utf-8")
        if path.is_file():
            digest.update(raw + b"\0" + hashlib.sha256(path.read_bytes()).digest())
            count += 1
    return {
        "head": git_output(root, "rev-parse", "HEAD").decode().strip(),
        "dirty": dirty, "source_files": count,
        "source_snapshot_sha256": digest.hexdigest(),
        "provenance": "Packaging-time source snapshot, not a build-time attestation. Rebuild from the release commit before publishing.",
    }


def package(root, version, allow_dirty=False):
    if not re.fullmatch(r"v\d+\.\d+\.\d+(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?", version):
        raise ValueError("Use a version such as v0.1.0-preview.1")
    dest = root / "build/github-release" / version
    if dest.exists():
        raise ValueError("Output already exists; choose a new version instead of overwriting")
    state = source_state(root, allow_dirty)
    validation = json.loads((root / "build/validation.json").read_text(encoding="utf-8"))
    release = root / "build/release"
    esp = esp_payload(release / "esp32")
    flipper = check_flipper(release / "flipper")
    if (sha(esp["merged-binary.bin"]) != validation["esp32"]["merged_sha256"]
            or sha((release / "flipper/firmware.bin").read_bytes())
            != validation["flipper"]["sha256"]):
        raise ValueError("Firmware hashes differ from build/validation.json; run validation again")
    state.update({
        "version": version, "packaged_at_utc": datetime.now(timezone.utc).isoformat(),
        "targets": {"flipper": "f7", "esp32": "deepflipper-s3 N16R8; ESP-IDF 6.1"},
        "hardware_validation": "Not established by packaging; verify on physical devices",
        "validation_report_sha256": sha((root / "build/validation.json").read_bytes()),
        "payload_sha256": {"esp32/" + n: sha(d) for n, d in esp.items()},
    })
    # Stage atomically; no partially prepared release directory on validation failure.
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".package-", dir=dest.parent) as temp:
        stage = Path(temp) / version
        stage.mkdir()
        fz_name = f"DeepFlipper-{version}-flipper-f7.tgz"
        esp_name = f"DeepFlipper-{version}-esp32s3-n16r8.zip"
        shutil.copyfile(flipper, stage / fz_name)
        with zipfile.ZipFile(stage / esp_name, "w", zipfile.ZIP_DEFLATED) as archive:
            for name, data in sorted(esp.items()):
                archive.writestr(name, data)
        state["firmware_archives_sha256"] = {
            n: sha((stage / n).read_bytes()) for n in (fz_name, esp_name)}
        (stage / "SOURCE_STATE.json").write_bytes(json_bytes(state))
        preview = ("**本地预览：源码尚有未提交改动，附件不精确对应 HEAD 或 Tag。正式发布前请提交、重建、重新验证和打包。**\n\n"
                   if state["dirty"] else "发布前请确认固件由所选提交重新构建；打包时源码指纹不是构建时证明。\n\n")
        notes = f"""# DeepFlipper {version}

{preview}鲸鱼娘系统桌面 + ESP32-S3 语音与配置网页 + Python 模型管理后台。
提供硬件工具、对话上下文、红外学习／Sub-GHz RAW 采集和音频对比试听。
手动启动正常应用前回收 AI 后台栈和 UART RPC 资源，退出后恢复；用于缓解 NFC 等应用内存不足。
对话音频加入首包接收保护、启动缓冲和每轮诊断；服务端区分上下行采样率并提供同样本流式／缓冲对比。
流式音频使用 480ms 启动／断粮恢复缓冲，TTS 编码器每轮同步重置；尾部等 PCM／I2S 排空后再切省电。
完整能力和限制以对应版本 README 为准，不代表所有菜单均能由 AI 控制。

- Flipper F7：qFlipper → Install from file，选 `{fz_name}`；TGZ 包含 SD 资源与工具 FAP。
- ESP32-S3 N16R8：解压 `{esp_name}`，按中文说明烧录。完整镜像 0x0 会重置配置；
  仅升级应用 0x20000 限相同分区和初始 ota_0 槽，保留 NVS。
- 服务端：对应源码运行 Docker Compose，8000 语音／8001 本机管理；模型权重另行下载。
- 校验下载文件：对照 SHA256SUMS.txt。

构建报告中的固件哈希已核对；软件编译和模拟结果不等于实机验证。
新增硬件采集、续航和扬声器效果需实机确认，不保证特定卡片／无线目标兼容。
源码 HEAD：`{state['head']}`；dirty：`{str(state['dirty']).lower()}`。
本目录不含模型、SDK、ELF、设备 Token、Wi-Fi 密码或服务器私有配置。
"""
        (stage / "RELEASE_NOTES.md").write_text(notes, encoding="utf-8")
        sums = "".join(f"{sha(p.read_bytes())}  {p.name}\n" for p in sorted(stage.iterdir()))
        (stage / "SHA256SUMS.txt").write_text(sums, encoding="utf-8")
        stage.rename(dest)
    return dest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--allow-dirty", action="store_true", help="Local preview only")
    args = parser.parse_args()
    try:
        dest = package(ROOT, args.version, args.allow_dirty)
    except (ValueError, KeyError, OSError, tarfile.TarError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Release packaging failed: {error}\n")
    print(f"Prepared locally: {dest}\nNo tag, push or remote release was created.")


if __name__ == "__main__":
    main()
