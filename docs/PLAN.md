# План реализации `fileshare`

Рабочий план по ТЗ ([fileshare_tz.md](fileshare_tz.md)). Согласован 2026-07-13.

## Ключевые решения (уточнения к ТЗ)

| Тема | Решение | Обоснование |
|---|---|---|
| Окружение | Разработка в **Visual Studio** (Windows 10). Клиент — кросс-платформенный (Windows+Linux). Сервер — основной в **Docker/Linux** (epoll). | Deploy на Timeweb Cloud (Linux); тестирование локально на Windows. |
| Сервер до M3 | Портируемый (blocking / thread-per-connection на `net`-шиме) — **тестируется целиком на Windows из VS**. С **M4** уходит в Docker/Linux (epoll не портируется). | Максимальная скорость итераций на ранних этапах; шим всё равно нужен клиенту. |
| Checksum | **CRC32** (свой табличный, IEEE 802.3), апгрейд на SHA-256 позже. | Ноль внешних crypto-зависимостей на старте. |
| Формат checksum на проводе | Фиксированное поле **32 байта** (`CHECKSUM_LEN=32`). CRC32 занимает первые 4 байта (big-endian), остальное — нули. | Апгрейд на SHA-256 не потребует менять формат фрейма и тесты протокола. |
| Зависимости | `nlohmann/json` + `GoogleTest` через `FetchContent` (с `FIND_PACKAGE_ARGS` — использует системные пакеты, если есть). **OpenSSL убрана** на старте. | Кросс-платформенно (MSVC+GCC), самодостаточно. |
| Сериализация | Явными функциями `write/read_uNNbe`, никакого `reinterpret_cast` структур. | Требование ТЗ §4.1 (strict aliasing / padding). |

## Схема сборки

- Единый CMake, открывается в Visual Studio как CMake-проект. `CMakePresets.json`: `windows-msvc-debug`, `linux-debug` (+ASan/TSan).
- Кросс-платформенные таргеты: `fileshare_core` (protocol/config/crc32/checksum), `fileshare_client`, тесты.
- `fileshare_server` — портируемая версия до M3; epoll-исходники компилируются только под Linux (`if(UNIX)`), основной запуск — в Docker.

## Этапы

| Этап | Содержание | Критерий готовности |
|---|---|---|
| **M0** ✅ | Протокол (framing, сериализация) + `config.json` + CRC32 — чистая логика, без сети. Юнит-тесты (GoogleTest). | `ctest` зелёный: round-trip encode/decode, отказ на битый/oversize/усечённый фрейм, load/save конфига. Ноль варнингов. **Готово** (commit `7f27a6f`). |
| **M1** ✅ | `net`-шим (Winsock/POSIX). Blocking-сервер на 1 клиента: `LIST` + `DOWNLOAD` одного файла end-to-end, прогресс, сверка checksum. | Реальный файл передан, checksum совпал. **Готово** (commit `486ffa5`): 46 тестов + двухпроцессный TCP-смоук. |
| **M2** ✅ | Thread-per-connection: несколько клиентов параллельно. Admin-консоль отдельным потоком → потокобезопасная очередь команд. Команды `add/remove/list/clients/kick/status/shutdown/help`. | 2+ клиента качают параллельно, консоль отзывчива. TSan чистый. **Готово** (commit `f42aee8`): 52 теста, ASan + TSan (×3) чистые. |
| **M3** ✅ | Нагрузочный тест (`scripts/loadtest`): N параллельных закачек, замер throughput, потолок thread-per-connection. | Цифры + вывод, обосновывающий M4. **Готово**: пик на N≈числу ядер, плато + деградация дальше — см. [loadtest-results.md](loadtest-results.md). |
| **M4** | epoll + thread pool (Linux-only). `Dockerfile` + `docker-compose.yml`, проброс порта на `localhost`. Клиент на Windows коннектится к контейнеру. | Функционал = M2 на epoll; нагрузочный тест лучше. TSan чистый. |
| **M5** | Устойчивость: битые фреймы (рвём только это соединение), обрывы (`recv==0`/`ECONNRESET`) без падения, `ERROR FILE_NOT_FOUND`/`UNSUPPORTED_OFFSET`, `kick`, graceful `shutdown`. `remove` во время закачки — документированное поведение. | Fault-injection тесты проходят. |
| **M6** (стретч) | Докачка по `offset`; опционально event loop на C++20 корутинах; апгрейд CRC32→SHA-256. | По желанию. |

## Структура репозитория

```
CMakeLists.txt · CMakePresets.json · tests/CMakeLists.txt
Dockerfile · docker-compose.yml            # с M4
include/fileshare/{types,protocol,crc32,checksum,config,net,server,client}.hpp
src/{protocol,crc32,checksum,config,net,server,client,server_main,client_main}.cpp
tests/{test_protocol,test_config,test_crc32}.cpp
scripts/loadtest.*  ·  share/.gitkeep  ·  docs/{fileshare_tz.md, PLAN.md}
```

## Отклонения от ТЗ (задокументировано)

1. **CRC32 вместо SHA-256** на старте (§4.4 ТЗ явно это допускает). Поле на проводе — 32 байта для forward-compat.
2. `config.json`: добавлено поле `checksum_algo` (`"crc32"`/`"sha256"`), значение checksum в `checksum` (hex), вместо жёсткого ключа `sha256`.
3. `CMake ≥ 3.24` (вместо 3.20) — ради `FetchContent ... FIND_PACKAGE_ARGS`. VS 2022 и Ubuntu 24.04 удовлетворяют.
