# fileshare

Консольный клиент-серверный файлообменник на **C++20** со своим бинарным протоколом поверх TCP.
Сервер раздаёт заранее указанные файлы, клиент видит их список и скачивает — с прогрессом,
докачкой и проверкой целостности. Проект прошёл путь от чистой логики до **трёх взаимозаменяемых
серверных архитектур** (поток-на-соединение → epoll + пул потоков → C++20-корутины).

> Это **не** BitTorrent: один сервер — источник истины, клиенты только скачивают у него
> (архитектурно ближе к FTP со своим протоколом). P2P-раздача между клиентами сознательно вне scope.

Построен как упражнение по системному программированию (бинарные протоколы, concurrency, epoll,
устойчивость к обрывам и враждебному вводу), но это рабочая, протестированная реализация.

---

## 🚀 v2 — «FShare Commander» (сервер-демон + TUI-клиент в стиле Midnight Commander)

Поверх v1 вырос **протокол v2** и полноценная клиент-серверная система (`src/v2/`,
`include/fileshare/v2/`). Реализованы этапы **M7–M11**:

- **`fileshare-daemon`** — неинтерактивный сервер: раздаёт **дерево директорий**
  (VFS над share-root, а не плоский список), аутентификация challenge–response
  (пароль не ходит по сети; SCRAM-подобная, `StoredKey` в `users.json`),
  **push-события** об изменениях в файлах (inotify → `EVENT_FS`),
  **живое управление без перезапуска** (лимит скорости и др. применяются на лету),
  graceful shutdown, SIGHUP-reload.
- **`fileshare-commander`** — полноэкранный TUI на [FTXUI](https://github.com/ArthurSonzogni/FTXUI):
  две панели (локальная + удалённая), горячие клавиши как в MC, скачивание с
  прогрессом/докачкой, **подсветка нового жёлтым**, индикатор связи и
  авто-реконнект, **админ-панель по F9** (обзор / клиенты+kick / живое
  редактирование настроек). Плюс `--batch` для скриптов.

```bash
cmake -S . -B build -G Ninja && cmake --build build     # соберёт и v1, и v2 (+FTXUI через FetchContent)
./build/fileshare-daemon --share-root ./data/share --port 5555     # сервер (no-auth bootstrap → admin)
./build/fileshare-daemon --add-user vit --role admin               # завести пользователя (пароль без эха)
./build/fileshare-commander                                        # TUI: экран входа → командер
# скриптом:
./build/fileshare-commander --batch --host 127.0.0.1 --port 5555 --login vit --list /
```

Флаг `-DFILESHARE_BUILD_TUI=OFF` отключает сборку TUI (например, на headless-сервере).
~90 тестов v2 (протокол/VFS/crypto/интеграция/события/админ/TUI) — зелёные под
AddressSanitizer и ThreadSanitizer. Полное ТЗ, архитектура и план — в
**[docs/v2/](docs/v2/)** (роадмап и статус — [docs/v2/08-roadmap.md](docs/v2/08-roadmap.md)).
Перспектива (M12–M14): пользователи/квоты, upload, TLS.

Ниже — документация исходной v1 (плоский файлообменник, на котором всё построено).

---

## Возможности

- **Свой бинарный протокол** поверх голого TCP: framing из 5-байтового заголовка + payload,
  явная big-endian сериализация без `reinterpret_cast`.
- **Три реализации сервера** на общем ядре, выбираются флагами сборки:
  - поток-на-соединение (портируемый, Windows + Linux);
  - **epoll + пул потоков** (Linux, дефолт для деплоя);
  - **C++20-корутины** поверх epoll (Linux, опция).
- **Кросс-платформенный клиент** (Windows/MSVC + Linux/GCC): список файлов, закачка с прогрессом,
  **докачка** прерванной передачи, сверка контрольной суммы.
- **Параллельная admin-консоль** сервера: `add / remove / list / clients / kick / status / shutdown`,
  не блокируется сетевым движком.
- **Целостность**: CRC32 по умолчанию или **SHA-256** (OpenSSL) — выбор на этапе сборки.
- **Устойчивость**: битые/oversize фреймы и обрывы рвут только одно соединение; graceful shutdown
  с дренажом активных закачек и обработкой `SIGTERM`/`SIGINT`.
- **Персистентность**: каталог раздачи в `config.json` (nlohmann/json), переживает перезапуск.
- **Docker**: multi-stage образ + `docker-compose.yml` для деплоя epoll-сервера.
- Ноль варнингов под `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` (и `/W4 /permissive-` на MSVC),
  проверка под **AddressSanitizer** и **ThreadSanitizer**.

## Стек и зависимости

| | |
|---|---|
| Язык / стандарт | C++20 |
| Сборка | CMake ≥ 3.24 (+ `CMakePresets.json`) |
| JSON (конфиг) | [nlohmann/json](https://github.com/nlohmann/json) — через `FetchContent` или системный пакет |
| Тесты | [GoogleTest](https://github.com/google/googletest) |
| Криптография (опция) | OpenSSL (`libssl-dev`) — только при `FILESHARE_USE_SHA256=ON` |
| Сеть | epoll + `accept4` (Linux); Winsock2 / BSD sockets в клиенте и портируемом сервере |
| Деплой | Docker (multi-stage) |

---

## Как это работает

```
┌──────────────┐        TCP, свой бинарный протокол        ┌───────────────────────┐
│  Client #1    │ ───────────────────────────────────────► │       Server           │
│  (консоль)     │ ◄─────────────────────────────────────── │  ┌──────────────────┐  │
└──────────────┘                                             │  │ ServerCore        │  │
┌──────────────┐                                             │  │ (каталог, реестр, │  │
│  Client #N    │ ◄────────────────────────────────────────►│  │  admin, статистика)│ │
└──────────────┘                                             │  └──────────────────┘  │
                                                             │  config.json · share/  │
                                                             └───────────────────────┘
```

### Протокол

Каждое сообщение — заголовок 5 байт + payload:

```
┌────────┬──────────────────────────┬───────────┐
│msg_type│  payload_length (u32, BE) │  payload  │
│ 1 байт │        4 байта            │  N байт   │
└────────┴──────────────────────────┴───────────┘
```

| Тип | Код | Направление | Payload |
|---|---|---|---|
| `LIST_REQUEST`     | `0x01` | C → S | пусто |
| `LIST_RESPONSE`    | `0x02` | S → C | `count:u32`, затем на каждый файл: `alias_len:u16`+alias, `size:u64`, `checksum:32 байта` |
| `DOWNLOAD_REQUEST` | `0x03` | C → S | `alias_len:u16`+alias, `offset:u64` (0 = сначала, >0 = докачка) |
| `CHUNK_DATA`       | `0x04` | S → C | сырые байты файла (до `CHUNK_SIZE` за раз) |
| `DOWNLOAD_DONE`    | `0x05` | S → C | `checksum:32 байта` — клиент сверяет с посчитанным локально |
| `ERROR`            | `0x06` | обе    | `code:u16`, `message_len:u16`+message |
| `PING` / `PONG`    | `0x07`/`0x08` | | пусто |

Лимиты: `MAX_ALIAS_LEN` = 255 Б, `MAX_CONTROL_PAYLOAD` = 1 MiB, `CHUNK_SIZE` = 64 KiB. Поле checksum
на проводе всегда 32 байта: SHA-256 занимает все 32, CRC32 — первые 4 (остальное нули), так что
формат фрейма не зависит от выбранного алгоритма.

Сценарий закачки: `DOWNLOAD_REQUEST` → сервер шлёт N `CHUNK_DATA` подряд → `DOWNLOAD_DONE`.

### Три реализации сервера

Все три делят **`ServerCore`** (каталог файлов, потокобезопасный реестр клиентов, статистика,
очередь admin-команд, обработка запросов) — «что делает сервер» живёт в одном месте, различается
только «как двигаются байты»:

- **`Server`** — поток на соединение, блокирующие сокеты. Кросс-платформенный (Winsock ↔ POSIX через
  тонкий `net`-шим). Используется для разработки на Windows и в тестах.
- **`EpollServer`** *(Linux)* — один reactor-поток крутит `epoll_wait` и раздаёт готовые соединения
  пулу воркеров; `EPOLLONESHOT` гарантирует, что соединение обрабатывает ровно один воркер за раз
  (состояние соединения не требует блокировок). Неблокирующие сокеты, per-connection state machine
  (сборка фреймов из частичных чтений, стриминг файла с backpressure по `EPOLLOUT`). Дефолт в Docker.
- **`CoroServer`** *(Linux, опция)* — то же событийное ядро, но каждое соединение — **C++20-корутина**:
  тело читается линейно (`co_await` готовности сокета) вместо явной машины состояний.

`server_main` выбирает реализацию: корутины (если `FILESHARE_USE_COROUTINES`) → epoll (Linux) →
портируемый сервер (иначе).

### Модель конкурентности

Admin-консоль читает stdin в своём потоке и кладёт команды в потокобезопасную очередь; сетевой
движок вычитывает её между итерациями — консоль остаётся отзывчивой во время закачек. Каталог под
мьютексом, статистика — атомики. Долгая закачка копирует запись каталога под локом и стримит без
него, поэтому `add`/`remove` не ждут передачу, а уже открытый файл доигрывается даже после `remove`.

### Устойчивость и завершение

- Неизвестный `msg_type`, `payload_length` больше лимита, усечённый фрейм → рвётся **только это**
  соединение, остальные не затрагиваются.
- Обрыв клиента (`recv==0`/`ECONNRESET`) → сервер освобождает ресурсы, не падает.
- `kick <id>` — принудительный обрыв соединения; `shutdown` / `SIGTERM` / `SIGINT` — **graceful**:
  перестать принимать новые подключения, дать активным закачкам доиграть (с таймаутом), затем выйти.

---

## Структура репозитория

```
CMakeLists.txt · CMakePresets.json · Dockerfile · docker-compose.yml
include/fileshare/       # публичные заголовки
  types, protocol, crc32, sha256, checksum, config,   # M0: логика
  net, client,                                        # клиент + сокет-шим
  server_core, client_registry,                       # общее ядро сервера
  server, epoll_server, epoll_coro_server, thread_pool, cli
src/                     # реализации (те же модули)
tests/                   # GoogleTest: протокол, конфиг, crc32/sha256, cli,
                         # интеграция, конкурентность, fault-injection, epoll,
                         # корутины, докачка, thread pool
scripts/loadtest.sh      # оркестрация нагрузочного теста
docs/                    # fileshare_tz.md (ТЗ), PLAN.md (план), loadtest-results.md
```

## Сборка

Требуется CMake ≥ 3.24, компилятор с C++20 (GCC 11+/Clang 14+/MSVC 2022) и интернет для
`FetchContent` (или системные `nlohmann-json3-dev` + GoogleTest).

### Опции CMake

| Опция | По умолч. | Описание |
|---|---|---|
| `FILESHARE_BUILD_TESTS`   | `ON`  | собирать юнит-тесты (GoogleTest) |
| `FILESHARE_USE_SHA256`    | `OFF` | SHA-256 (OpenSSL) вместо CRC32 |
| `FILESHARE_USE_COROUTINES`| `OFF` | корутинный epoll-сервер (Linux) |
| `FILESHARE_ENABLE_ASAN`   | `OFF` | AddressSanitizer |
| `FILESHARE_ENABLE_TSAN`   | `OFF` | ThreadSanitizer |

### Linux

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# бинарники: build/fileshare_server, build/fileshare_client, build/fileshare_loadtest
```

С SHA-256 и корутинным сервером:

```bash
cmake -S . -B build -G Ninja -DFILESHARE_USE_SHA256=ON -DFILESHARE_USE_COROUTINES=ON
cmake --build build
```

### Windows (Visual Studio)

Открыть папку как CMake-проект (`CMakePresets.json` даёт готовые конфигурации) и собрать пресет
`windows-msvc-debug`. Собираются **клиент**, ядро и тесты; epoll/coro-серверы — только под Linux,
поэтому на Windows поднимается портируемый поток-на-соединение сервер. Типичный сценарий: клиент
запускается нативно на Windows, сервер — в Docker (см. ниже), клиент коннектится на `localhost`.

### Docker (деплой сервера)

```bash
docker compose up --build            # epoll-сервер, порт 5555 проброшен на localhost
```

Файлы для раздачи и `config.json` кладутся в `./data`. Собрать образ с SHA-256:

```bash
docker build --build-arg USE_SHA256=ON -t fileshare-server .
```

## Запуск и использование

### Сервер

```bash
fileshare_server [--port N] [--config PATH] [--add PATH[=ALIAS]]...
```

Admin-команды (интерактивно в stdin):

| Команда | Описание |
|---|---|
| `add <path> [alias]` | добавить файл в раздачу (считает checksum, пишет в `config.json`) |
| `remove <alias>`     | убрать из раздачи (файл на диске не трогается) |
| `list`               | каталог: alias, размер, checksum |
| `clients`            | подключённые: id, адрес, что качает, сколько байт |
| `kick <id>`          | оборвать соединение |
| `status`             | аптайм, отдано байт, завершённых закачек, активных соединений |
| `shutdown`           | graceful-остановка |
| `help`               | список команд |

### Клиент

Интерактивный REPL:

```
connect <host> <port>        подключиться
list                          список доступных файлов
download <alias> [as <path>] скачать (с прогрессом; докачивает, если есть <path>.part)
disconnect                    закрыть соединение
help · quit
```

Или одноразовый режим (для скриптов):

```bash
fileshare_client --host 127.0.0.1 --port 5555 --get <alias> [--out PATH]
```

### Пример

```
# Сервер (в Docker или локально на Linux)
$ fileshare_server --port 5555 --add /data/ubuntu-24.04.iso
added: ubuntu-24.04.iso (4881539072 bytes)
serving on port 5555 -- admin console ready (type 'help')

# Клиент (например, на Windows)
> connect 127.0.0.1 5555
connected to 127.0.0.1:5555
> list
1) ubuntu-24.04.iso   4.5 GiB   e3b0c442
> download ubuntu-24.04.iso
  34% (1.5 GiB / 4.5 GiB)
done: 4.5 GiB -> ubuntu-24.04.iso, checksum OK
```

## Тестирование

```bash
# AddressSanitizer + прогон через ctest
cmake -S . -B build-asan -G Ninja -DFILESHARE_ENABLE_ASAN=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Под **ThreadSanitizer** тестовый бинарь запускается напрямую (в контейнерах у TSan бывает
`unexpected memory mapping` из-за ASLR — помогает `setarch -R`):

```bash
cmake -S . -B build-tsan -G Ninja -DFILESHARE_ENABLE_TSAN=ON
cmake --build build-tsan --target fileshare_tests
setarch -R build-tsan/tests/fileshare_tests
```

Тесты покрывают: сериализацию/десериализацию протокола и отказ на битом вводе, `config.json`,
CRC32/SHA-256, парсинг CLI, end-to-end закачку, конкурентные закачки, fault-injection (враждебные
фреймы, обрывы, `remove`/`shutdown` во время закачки), докачку, epoll- и корутинный серверы.

## Нагрузочные результаты

`fileshare_loadtest` гоняет N параллельных закачек и меряет throughput. Кратко: поток-на-соединение
масштабируется до N ≈ числа ядер, затем проседает; epoll держит пик без деградации на тех же ядрах,
используя горстку воркеров вместо сотен потоков. Подробности и цифры — [docs/loadtest-results.md](docs/loadtest-results.md).

## Этапы разработки

| Этап | Что | 
|---|---|
| **M0** | Протокол (framing/сериализация) + `config.json` + CRC32, юнит-тесты |
| **M1** | `net`-шим (Winsock/POSIX), blocking-сервер, клиент со стримингом, end-to-end |
| **M2** | Поток-на-соединение + параллельная admin-консоль, реестр клиентов, `kick` |
| **M3** | Нагрузочный тест: потолок thread-per-connection |
| **M4** | epoll + пул потоков, Docker-деплой |
| **M5** | Устойчивость: graceful shutdown с дренажом, сигналы, fault-injection |
| **M6** | Стретч: SHA-256, докачка по `offset`, event loop на C++20-корутинах |

Полный план и принятые решения — [docs/PLAN.md](docs/PLAN.md); исходное ТЗ — [docs/fileshare_tz.md](docs/fileshare_tz.md).

## Осознанно вне scope

Настоящий P2P между клиентами (DHT, piece-selection, choking), шифрование трафика (TLS),
аутентификация/авторизация, NAT traversal, GUI.
