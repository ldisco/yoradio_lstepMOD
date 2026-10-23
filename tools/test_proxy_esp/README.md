# test_proxy_esp — тест VPS-прокси для yoRadio **lstepMOD**

Папка **`tools/test_proxy_esp`** в репозитории **[yoRadio lstepMOD](https://github.com/ldisco/yoradio_lstepMOD)** (форк YoRadio v0.9.720, **не** стоковый upstream): настройка nginx на своём VPS и проверка цепочки **ESP32 → ваш сервер :80 → Gemini/Groq** до прошивки основной **lstepMOD**.

| Файл | Назначение |
|------|------------|
| **[SETUP_VPS_PROXY.md](SETUP_VPS_PROXY.md)** | Пошаговая настройка VPS (nginx, VPN не трогаем) |
| `deploy/gemini-proxy.nginx.conf` | Шаблон конфига nginx |
| `src/main.cpp` | Мини-прошивка: `/health`, `/gemini`, `/groq` |
| `include/secrets.h.example` | Wi‑Fi, `PROXY_BASE`, API-ключи |

Основная прошивка **lstepMOD** **не меняется** — это отдельный PlatformIO-проект внутри `tools/`.

> **Контекст TrackFacts:** в lstepMOD без VPS часто хватает **DeepSeek** / **iTunes**; прокси нужен, если в WebUI выбраны **Gemini** или **Groq** и ESP не видит API напрямую (см. README репозитория).

---

## Быстрый старт

### 1. VPS

Следуйте **[SETUP_VPS_PROXY.md](SETUP_VPS_PROXY.md)** (разделы 1–8): nginx на **:80**, проверка `curl http://ВАШ_СЕРВЕР/health` → `ok` (пробный «пинг» прокси, без API-ключа).

### 2. Секреты на ПК

```bash
cd tools/test_proxy_esp/include
copy secrets.h.example secrets.h
```

В `secrets.h` укажите Wi‑Fi, `PROXY_BASE` (тот же URL, что потом в lstepMOD WebUI), `GEMINI_API_KEY` (и при необходимости `GROQ_API_KEY`).

### 3. Сборка и прошивка

Из корня репозитория:

```bash
pio run -d tools/test_proxy_esp -e test-proxy-esp -t upload --upload-port COMx
pio device monitor -d tools/test_proxy_esp -b 115200 --port COMx
```

Или из `tools/test_proxy_esp`:

```bash
pio run -e test-proxy-esp -t upload
pio device monitor -b 115200
```

Плата: **ESP32-S3** с PSRAM (как в lstepMOD, env `yoradio-esp32s3`). COM-порт — свой.

### 4. Serial

Ожидается:

- `[proxy] /health → 200 'ok'`
- `[Gemini] code=200` и блок **ФАКТ**
- при ключе Groq — факт Groq раньше Gemini

### 5. yoRadio lstepMOD

Тот же **базовый URL** → WebUI → **Settings** → **track facts** → **AI proxy URL** → **Apply**. Подробно в SETUP, раздел 10.

---

## Зачем отдельная папка

- Всё для пользователя **lstepMOD** в одном репозитории (smoke-test прокси рядом с основной прошивкой).
- Можно проверить VPS **без** пересборки основной прошивки.
- `secrets.h` и ключи **не коммитятся** (`.gitignore`).

---

## Связь с yoRadio lstepMOD

| test_proxy_esp | lstepMOD |
|----------------|----------|
| `PROXY_BASE` в secrets.h | WebUI «AI proxy URL» или `#define TRACKFACTS_AI_PROXY` |
| HTTP `{base}/gemini?key=` | `GeminiProvider.cpp` |
| HTTP `{base}/groq` + Bearer | Groq provider |
| Модель Gemini 2.5-flash | Задаётся **nginx** на VPS |

Генератор myoptions: [lstep.site/myoptions](https://lstep.site/myoptions/) → ADVANCED → **VPS-прокси** → `#define TRACKFACTS_AI_PROXY ""` открывает поле в WebUI.

Репозиторий: [github.com/ldisco/yoradio_lstepMOD](https://github.com/ldisco/yoradio_lstepMOD)
