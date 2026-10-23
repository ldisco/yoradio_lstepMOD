# Автозамена встроенного 5x7 шрифта GFX (кириллица / иконки yoRadio).
# Источник: src/fonts/glcdfont.c
# Куда:
#   - Arduino_GFX → .../GFX Library for Arduino/src/font/glcdfont.h
#   - Adafruit_GFX → .../Adafruit GFX Library/glcdfont.c
# Копирует только если содержимое отличается (иначе каждый билд трогал бы timestamp → лишний rebuild).
#
# Подключение: extra_scripts = post:tools/patch_glcdfont.py в platformio.ini
# (post: — после установки lib_deps, до компиляции).

Import("env")  # pylint: disable=undefined-variable

from pathlib import Path

PROJECT_DIR = Path(env["PROJECT_DIR"])  # noqa: F821
SRC = PROJECT_DIR / "src" / "fonts" / "glcdfont.c"
LIBDEPS = Path(env["PROJECT_LIBDEPS_DIR"]) / env["PIOENV"]  # noqa: F821

# (путь назначения, подпись в логе)
TARGETS = (
    (LIBDEPS / "GFX Library for Arduino" / "src" / "font" / "glcdfont.h", "Arduino_GFX"),
    (LIBDEPS / "Adafruit GFX Library" / "glcdfont.c", "Adafruit_GFX"),
)


def _same_bytes(a: Path, b: Path) -> bool:
    try:
        return a.read_bytes() == b.read_bytes()
    except OSError:
        return False


def patch_glcdfont():
    if not SRC.is_file():
        print(f"[patch_glcdfont] НЕТ источника: {SRC}")
        return

    src_data = SRC.read_bytes()
    any_missing = False

    for dst, label in TARGETS:
        if not dst.parent.is_dir():
            print(f"[patch_glcdfont] {label}: lib ещё нет ({dst.parent.name}) — пропуск")
            any_missing = True
            continue
        if dst.is_file() and _same_bytes(SRC, dst):
            print(f"[patch_glcdfont] {label}: уже наш шрифт, ок")
            continue
        dst.write_bytes(src_data)
        print(f"[patch_glcdfont] {label}: заменён → {dst}")

    if any_missing:
        print(
            "[patch_glcdfont] Часть библиотек ещё не скачана. "
            "После первого pio run (когда появится .pio/libdeps) пересоберите ещё раз."
        )


patch_glcdfont()
