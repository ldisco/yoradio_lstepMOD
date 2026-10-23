# Настройка VPS-прокси для yoRadio **lstepMOD** (TrackFacts: Gemini и Groq)

Пошаговая инструкция для **конечного пользователя** прошивки **[yoRadio lstepMOD](https://github.com/ldisco/yoradio_lstepMOD)** — форка и модернизации [YoRadio](https://github.com/e2002/yoradio) (база v0.9.720), **не** для «стокового» yoRadio из Arduino IDE / upstream без модов lstep.

Схема: свой VPS → nginx на порту **80** → ESP32 с **lstepMOD** ходит по **HTTP** на ваш сервер, сервер сам обращается к Google Gemini и Groq по HTTPS.

### О какой прошивке речь

| | **yoRadio lstepMOD** (этот репозиторий) | Стоковый upstream yoRadio |
|--|----------------------------------------|----------------------------|
| TrackFacts, AI-провайдеры | да, WebUI **track facts** | нет / иначе |
| Поле **AI proxy URL (VPS)** | да (`TRACKFACTS_AI_PROXY` в `myoptions.h`) | нет |
| Плагин Gemini через HTTP-прокси | `GeminiProvider` + nginx `/gemini` | не описано в этой инструкции |
| Рекомендация без VPS (из README проекта) | **DeepSeek**, **iTunes** — часто стабильнее с домашнего Wi‑Fi | — |
| Зачем VPS | **Gemini**, **Groq** (и аналоги) — с ESP часто **не доступны** напрямую из‑за блокировок; ПК с VPN **не заменяет** проверку для платы |

Если у вас **не** lstepMOD — эта инструкция и `tools/test_proxy_esp` могут не совпасть с прошивкой.

**Где лежит в проекте**

| Путь | Назначение |
|------|------------|
| **`tools/test_proxy_esp/`** | Эта инструкция, nginx-шаблон, smoke-test прошивка |
| **`tools/test_proxy_esp/README.md`** | Как прошить тестовую прошивку |
| **`src/`** | Основная прошивка **lstepMOD**: TrackFacts, WebUI «AI proxy URL» |
| **[lstep.site/myoptions](https://lstep.site/myoptions/)** | Генератор `myoptions.h` (ADVANCED → proxy); в репозиторий не входит |

---

## 0. Зачем это нужно (в контексте lstepMOD)

В **lstepMOD** плагин **TrackFacts** выводит факты о треке через выбранный в WebUI провайдер. Для **Gemini** и **Groq** типичная картина в РФ/СНГ:

- С **домашнего Wi‑Fi** ESP32 **часто не достучится** до `generativelanguage.googleapis.com` и `api.groq.com` напрямую (блокировки, фильтры провайдера) — это совпадает с [описанием проекта](https://github.com/ldisco/yoradio_lstepMOD#-главные-отличия-и-новые-функции).
- **DeepSeek** и **iTunes** у многих работают без VPS; эта инструкция — для тех, кто сознательно выбрал **Gemini** или **Groq** и хочет стабильный путь с ESP.
- С **ПК через VPN** API может открываться — **не значит**, что ESP тоже сможет: **плата VPN не использует**.
- Решение в lstepMOD: **ESP → ваш VPS (HTTP :80) → VPS → Google/Groq (HTTPS)**; URL прокси — в WebUI (**Settings → track facts → AI proxy URL**) или в `myoptions.h`.

Модель Gemini **2.5-flash** и путь `/gemini` задаются **в nginx на VPS** (см. шаг 7). Менять модель на радио без правки nginx нельзя.

**Рекомендуемый порядок:** VPS (шаги 1–8) → smoke-test `tools/test_proxy_esp` (шаг 9) → **lstepMOD** WebUI (шаг 10).

---

## 1. Что понадобится

1. **VPS** с root-доступом по SSH (Timeweb, AdminVPS, PLAY2GO, JustHost и т.п.).
2. **С домашнего ПК без VPN** до сервера должен открываться **TCP-порт 80** (см. шаг 2).
3. **С самого VPS** должен открываться интернет и HTTPS к Google/Groq (см. шаг 5).
4. **API-ключ** [Google AI Studio](https://aistudio.google.com/) (Gemini). Для Groq — [console.groq.com](https://console.groq.com/) (опционально).
5. Прошивка **yoRadio lstepMOD** с TrackFacts; в `myoptions.h` часто `#define TRACKFACTS_AI_PROXY ""` — **открывает поле** proxy в WebUI, URL задаёте сами (генератор: [lstep.site/myoptions](https://lstep.site/myoptions/), ADVANCED).

---

## 2. Проверка VPS **до** настройки (с домашнего ПК)

**VPN на ПК выключите.** ESP идёт тем же путём, что и ПК без VPN.

Подставьте **свой** IP или домен вместо `ВАШ_СЕРВЕР`:

**Windows (PowerShell):**

```powershell
curl.exe -sS --connect-timeout 5 -I http://ВАШ_СЕРВЕР/
```

**Linux / macOS:**

```bash
curl -sS --connect-timeout 5 -I http://ВАШ_СЕРВЕР/
```

| Результат | Что это значит |
|-----------|----------------|
| Любой HTTP-ответ (200, 301, 404…) | TCP до порта 80 **живой** — можно продолжать |
| `Connection reset` / timeout | С домашнего интернета этот сервер **не подойдёт** для ESP — другой VPS или локация |

> **Важно:** проверяйте **без VPN**, иначе настроите сервер, который ESP не увидит.

---

## 3. Два типичных случая на сервере

### A. «Пустой» или почти пустой сервер

- nginx ещё не стоит, или на `:80` ничего критичного нет.
- Шаги **4 → 7** — один конфиг на порт 80.

### B. Уже есть VPN (xray, 3x-ui, WireGuard и т.п.)

- **Порт 443 и конфиги VPN не трогаем.** Прокси TrackFacts — **только HTTP :80**.
- На `:80` может быть редирект на HTTPS — добавьте `/health`, `/gemini`, `/groq` **выше** `location / { return 301 … }`.
- Перед правками — **бэкап** (шаг 7.1).

**Правило:** не ломаем VPN; добавляем три location на `:80`.

---

## 4. Вход на сервер и разведка

```bash
ssh root@ВАШ_СЕРВЕР
```

```bash
uname -a
which nginx
nginx -v 2>&1
ss -tlnp | head -40
ls -la /etc/nginx/sites-enabled/
cat /etc/nginx/sites-enabled/80.conf 2>/dev/null
```

Запишите: что на **:443** (не менять), есть ли редирект с **:80** на HTTPS.

---

## 5. Проверка: VPS видит Google и Groq

```bash
curl -sS --connect-timeout 8 -o /dev/null -w "google=%{http_code}\n" \
  https://generativelanguage.googleapis.com/

curl -sS --connect-timeout 8 -o /dev/null -w "groq=%{http_code}\n" \
  https://api.groq.com/
```

- `google=404` — **нормально** (не timeout, не SSL error).
- Timeout / SSL error — VPS **не подходит** как прокси.

---

## 6. Установка nginx (если ещё нет)

**Debian / Ubuntu:**

```bash
apt update
apt install -y nginx
systemctl enable nginx
systemctl start nginx
```

---

## 7. Конфиг прокси на порту 80

### 7.1. Бэкап

```bash
cp -a /etc/nginx/sites-enabled/80.conf /root/80.conf.bak.$(date +%F)
```

Бэкап только в **`/root/`**, не в `sites-enabled/`.

### 7.2. Записать конфиг

Готовый файл в репозитории: **`tools/test_proxy_esp/deploy/gemini-proxy.nginx.conf`**.

Или одним блоком на сервере:

```bash
cat > /etc/nginx/sites-enabled/80.conf <<'EOF'
server {
    listen 80;
    server_name _;

    client_max_body_size 1m;

    location = /health {
        default_type text/plain;
        return 200 "ok\n";
    }

    location = /gemini {
        resolver 8.8.8.8 1.1.1.1 ipv6=off valid=300s;
        resolver_timeout 5s;
        proxy_pass https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent$is_args$args;
        proxy_ssl_server_name on;
        proxy_ssl_name generativelanguage.googleapis.com;
        proxy_set_header Host generativelanguage.googleapis.com;
        proxy_http_version 1.0;
        proxy_set_header Connection close;
        proxy_connect_timeout 20s;
        proxy_send_timeout 90s;
        proxy_read_timeout 90s;
        proxy_buffering on;
        proxy_request_buffering on;
    }

    location = /groq {
        resolver 8.8.8.8 1.1.1.1 ipv6=off valid=300s;
        resolver_timeout 5s;
        proxy_pass https://api.groq.com/openai/v1/chat/completions;
        proxy_ssl_server_name on;
        proxy_ssl_name api.groq.com;
        proxy_set_header Host api.groq.com;
        proxy_set_header Authorization $http_authorization;
        proxy_set_header Content-Type $content_type;
        proxy_http_version 1.0;
        proxy_set_header Connection close;
        proxy_connect_timeout 20s;
        proxy_send_timeout 90s;
        proxy_read_timeout 90s;
        proxy_buffering on;
        proxy_request_buffering on;
    }

    location / {
        return 301 https://$host$request_uri;
    }
}
EOF
```

**Почему `resolver`:** без него часто **502** (`no resolver defined to resolve generativelanguage.googleapis.com`).

> **Что такое `/health`?** Это **пробный адрес** на вашем VPS: запрос `http://ВАШ_СЕРВЕР/health` должен вернуть текст **`ok`**. Ключ API не нужен, до Google запрос **не идёт** — только проверка, что с вашего интернета (или с ESP) **достучались до сервера на порту 80**. Сначала проверяем `/health`, потом `/gemini`. В обычной работе **lstepMOD** ходит на `/gemini` и `/groq`; `/health` — для настройки и smoke-test (`tools/test_proxy_esp`).

### 7.3. Применить

```bash
nginx -t && systemctl reload nginx
curl -sS http://127.0.0.1/health
```

Ожидание: `syntax is ok` и `ok`.

---

## 8. Проверка с домашнего ПК (без VPN)

Проверяем **`/health`** — «пинг» прокси (см. пояснение в шаге 7.2). Ждём **`ok`**, не HTML с редиректом 301.

```powershell
curl.exe -sS --connect-timeout 5 http://ВАШ_СЕРВЕР/health
```

Ждём текст **`ok`**.

### Gemini

```powershell
[System.IO.File]::WriteAllText("$env:TEMP\gemini.json", '{"contents":[{"parts":[{"text":"Reply with one word: ok"}]}],"generationConfig":{"temperature":0.32,"thinkingConfig":{"thinkingBudget":0}}}')

curl.exe -sS -X POST "http://ВАШ_СЕРВЕР/gemini?key=ВАШ_GEMINI_KEY" -H "Content-Type: application/json" --data-binary "@$env:TEMP\gemini.json"
```

### Groq (опционально)

```powershell
[System.IO.File]::WriteAllText("$env:TEMP\groq.json", '{"model":"openai/gpt-oss-20b","temperature":0.55,"max_completion_tokens":1024,"reasoning_effort":"low","include_reasoning":false,"messages":[{"role":"user","content":"Reply with one word: ok"}]}')

curl.exe -sS -X POST "http://ВАШ_СЕРВЕР/groq" -H "Content-Type: application/json" -H "Authorization: Bearer ВАШ_GROQ_KEY" --data-binary "@$env:TEMP\groq.json"
```

---

## 9. Smoke-test с ESP (`tools/test_proxy_esp`)

Перед yoRadio можно прошить **мини-тест** из этой же папки:

1. `include/secrets.h.example` → `include/secrets.h` (Wi‑Fi, `PROXY_BASE`, ключи).
2. `pio run -d tools/test_proxy_esp -e test-proxy-esp -t upload`
3. Serial: `[proxy] /health → 200`, `[Gemini] code=200`, блок **ФАКТ**.

Подробно: **`tools/test_proxy_esp/README.md`**.

Если тест OK — тот же `PROXY_BASE` переносите в **lstepMOD** WebUI.

---

## 10. Настройка yoRadio **lstepMOD** (основная прошивка)

| Назначение | URL |
|------------|-----|
| Проверка | `{база}/health` |
| Gemini | `{база}/gemini?key=…` |
| Groq | `{база}/groq` + `Authorization: Bearer …` |

**Базовый URL** — без `/gemini` в конце, например `http://ваш-сервер.example.com`.

### 10.1. WebUI (рекомендуется)

1. WebUI → **Settings** → **track facts**.
2. **enable track facts**, провайдер Gemini (или Groq).
3. **API key** + **AI proxy URL (VPS)** = ваш базовый URL.
4. **Apply**.

WebUI **важнее** compile-time в `myoptions.h`.

### 10.2. myoptions.h (опционально)

```cpp
#define TRACKFACTS_AI_PROXY "http://ваш-сервер.example.com"
```

Пустая строка `""` — URL только из WebUI.

### 10.3. Serial lstepMOD

| Строка | Ожидание |
|--------|----------|
| `[Gemini] POST start` | Запрос пошёл |
| HTTP **200** | Прокси и ключ OK |
| Факт на TFT / WebUI | TrackFacts работает |

---

## 11. Если что-то пошло не так

### `duplicate default_server`

- Уберите `.bak` из `sites-enabled/`, бэкапы в `/root/`.

### `/health` → **301**

- `location = /health` должен быть **до** редиректа; `nginx -t && systemctl reload nginx`.

### **502** на `/gemini`

```bash
tail -n 30 /var/log/nginx/error.log
```

Часто: нет `resolver` — см. шаг 7.2.

### С дома reset, с VPS OK

- Другой VPS/регион; проверка **без VPN** (шаг 2).

### ESP timeout, ПК OK

- Одна Wi‑Fi с платой; URL: **http://**, порт **80**; длина URL ≤ 63 символа.

### Groq: пустой content

- `max_completion_tokens`: 1024, модель `openai/gpt-oss-20b`.

---

## 12. Шпаргалка

| Где | Команда / действие | Ожидание |
|-----|-------------------|----------|
| Дома, без VPN | `curl http://ВАШ_СЕРВЕР/health` | `ok` |
| VPS | `curl http://127.0.0.1/health` | `ok` |
| test_proxy_esp | Serial `/health`, Gemini 200 | факт в Serial |
| lstepMOD WebUI | proxy URL + Apply | факт на радио |

---

## 13. Чего **не** делать

- Не править **VPN на :443**.
- Не класть `.bak` в `sites-enabled/`.
- Не проверять только с ПК **через VPN**.
- Не публиковать ключи и адрес сервера в git.

---

## 14. Файлы в `tools/test_proxy_esp`

| Файл | Назначение |
|------|------------|
| `SETUP_VPS_PROXY.md` | Эта инструкция |
| `README.md` | Прошивка smoke-test |
| `deploy/gemini-proxy.nginx.conf` | Шаблон nginx |
| `platformio.ini` | env `test-proxy-esp` |
| `src/main.cpp` | Тест `/health`, `/gemini`, `/groq` |
| `include/secrets.h.example` | Wi‑Fi, `PROXY_BASE`, ключи |

---

## 15. Чеклист «сел и настроил»

- [ ] VPS, SSH, порт 80 с дома **без VPN**
- [ ] nginx: `/health`, `/gemini`, `/groq` на **:80**; **:443 не трогали**
- [ ] curl с ПК: `/health` → `ok`, POST `/gemini` → JSON
- [ ] (опционально) `test_proxy_esp` → Serial OK
- [ ] **lstepMOD**: TrackFacts + ключ + **AI proxy URL** + Apply
- [ ] Факт на радио при смене трека или вручную в WebUI

Готово — прокси, ключи и адрес сервера только у вас.
