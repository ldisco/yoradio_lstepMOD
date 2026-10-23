# Автоподмена prebuilt lwIP / esp_netif для тяжёлого FLAC (TCP window).
# Источник в репозитории (без внешних путей):
#   tools/audio_fix_libs/esp32s3/libesp_netif.a + liblwip.a  → S3
#   tools/audio_fix_libs/esp32/libesp_netif.a + liblwip.a    → classic ESP32 (Wrover)
# Куда (пакет PlatformIO после скачивания platform):
#   .../packages/framework-arduinoespressif32-libs/<mcu>/lib/
# Копирует только если содержимое отличается (иначе каждый билд трогал бы timestamp).
# Чип берём из env (BOARD_MCU / PIOENV), чтобы S3-сборка не трогала classic и наоборот.
#
# Подключение: post:tools/patch_audio_fix_libs.py в platformio.ini
# (post: — после установки пакетов/lib_deps, до линковки).

Import("env")  # pylint: disable=undefined-variable

from pathlib import Path

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
PACKAGES = Path(env["PROJECT_PACKAGES_DIR"])  # noqa: F821

FILES = (
    "libesp_netif.a",
    "liblwip.a",
)


def _same_bytes(a, b):
    try:
        return a.read_bytes() == b.read_bytes()
    except OSError:
        return False


def _resolve_mcu():
    """Вернуть 'esp32s3' | 'esp32' для папки framework-libs, иначе None (пропуск)."""
    mcu = (env.get("BOARD_MCU") or "").strip().lower()  # noqa: F821
    if mcu in ("esp32s3", "esp32"):
        return mcu
    # Запас: имя env (yoradio-esp32-wrover / yoradio-esp32s3 / jc3248…)
    pioenv = (env.get("PIOENV") or "").lower()  # noqa: F821
    if "wrover" in pioenv or pioenv.endswith("-esp32") or "esp32-wrover" in pioenv:
        return "esp32"
    if "esp32s3" in pioenv or "jc2432" in pioenv or "jc3248" in pioenv or "4848" in pioenv:
        return "esp32s3"
    if pioenv.startswith("yoradio-esp32") and "s3" not in pioenv:
        return "esp32"
    return None


def patch_audio_fix_libs():
    mcu = _resolve_mcu()
    if not mcu:
        print(
            f"[patch_audio_fix_libs] неизвестный MCU/env "
            f"(BOARD_MCU={env.get('BOARD_MCU')!r}, PIOENV={env.get('PIOENV')!r}) — пропуск"  # noqa: F821
        )
        return

    src_dir = PROJECT_DIR / "tools" / "audio_fix_libs" / mcu
    dst_dir = PACKAGES / "framework-arduinoespressif32-libs" / mcu / "lib"

    if not src_dir.is_dir():
        print(f"[patch_audio_fix_libs] НЕТ папки источника: {src_dir}")
        return

    if not dst_dir.is_dir():
        print(
            f"[patch_audio_fix_libs] framework-libs ещё нет ({dst_dir}) — пропуск. "
            "После первого скачивания platform пересоберите ещё раз."
        )
        return

    # Стрелку Unicode не печатаем: на Windows cp1251 click падает UnicodeEncodeError.
    print(f"[patch_audio_fix_libs] MCU={mcu}  src={src_dir.name}/ -> {dst_dir}")
    for name in FILES:
        src = src_dir / name
        dst = dst_dir / name
        if not src.is_file():
            print(f"[patch_audio_fix_libs] НЕТ источника: {src}")
            continue
        if dst.is_file() and _same_bytes(src, dst):
            print(f"[patch_audio_fix_libs] {name}: уже наш, ок")
            continue
        dst.write_bytes(src.read_bytes())
        print(f"[patch_audio_fix_libs] {name}: zamenen -> {dst}")


patch_audio_fix_libs()
