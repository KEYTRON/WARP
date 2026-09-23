# WARP

[English version](README.md)

WARP — небольшой пакетный менеджер на C для K1OS и ещё нескольких
дистрибутивов. Скачивает подписанные архивы пакетов, проверяет их и ведёт
версионированное локальное хранилище в `/var/lib/warp` с мгновенным откатом.

![Tests](https://github.com/KEYTRON/WARP/actions/workflows/warp-tests.yml/badge.svg)
![K1OS](https://github.com/KEYTRON/WARP/actions/workflows/warp-k1os.yml/badge.svg)
![Alpine](https://github.com/KEYTRON/WARP/actions/workflows/warp-alpine.yml/badge.svg) ![AlmaLinux](https://github.com/KEYTRON/WARP/actions/workflows/warp-almalinux.yml/badge.svg) ![Arch](https://github.com/KEYTRON/WARP/actions/workflows/warp-arch.yml/badge.svg) ![Artix](https://github.com/KEYTRON/WARP/actions/workflows/warp-artix.yml/badge.svg) ![Devuan](https://github.com/KEYTRON/WARP/actions/workflows/warp-devuan.yml/badge.svg) ![Fedora](https://github.com/KEYTRON/WARP/actions/workflows/warp-fedora.yml/badge.svg) ![Ubuntu](https://github.com/KEYTRON/WARP/actions/workflows/warp-ubuntu.yml/badge.svg) ![Void](https://github.com/KEYTRON/WARP/actions/workflows/warp-void.yml/badge.svg) ![Void Musl](https://github.com/KEYTRON/WARP/actions/workflows/warp-void-musl.yml/badge.svg)

[Все запуски workflow](https://github.com/KEYTRON/WARP/actions) — всё
выполняется на self-hosted раннере лабы K1.

## Модель доверия — «кругом одни враги»

WARP считает враждебным каждый сетевой узел и доверяет ровно одной вещи:
Ed25519-ключу, закреплённому локально.

- Репозиторий — это базовый URL (плюс необязательные зеркала) и **публичный
  ключ**. Ключ закрепляется при добавлении репозитория и никогда не
  передаётся вместе с данными.
- Каждое зеркало отдаёт `index.json` и отделённую подпись `index.json.sig`
  рядом. WARP скачивает оба **с одного и того же зеркала** и проверяет подпись
  закреплённым ключом *до* кэширования и чтения индекса. Нет валидной подписи —
  нет индекса; режима «без подписи» не существует.
- Подписанный индекс — замкнутый мир: имя, версия, URL, размер и `sha256`
  каждого архива и каждой дельты. Скачанные байты сверяются с индексом до
  установки. Зеркала, торренты и P2P-пиры — только транспорт: они не могут
  ни добавить пакет, ни подменить хэш.
- Зависимости тоже объявлены в подписанном индексе (см. ниже); манифест внутри
  архива носит справочный характер и обязан совпадать с индексом.

## Сборка

```bash
make            # или ./build.sh build
```

## Установка

```bash
sudo make install       # или sudo ./build.sh install
```

Бинарник ставится в `/usr/local/bin/warp`; другое место — через `PREFIX`.

## Команды

| Команда | Что делает |
|---|---|
| `warp update` | Обновить индексы всех включённых репозиториев (с проверкой подписей) |
| `warp search <query>` | Поиск по доступным пакетам |
| `warp install <pkg>` | Установить пакет (`repo/pkg` — явно выбрать репозиторий) |
| `warp upgrade [pkg...]` | Обновить установленные пакеты, через дельту, если она опубликована |
| `warp remove <pkg>` | Удалить пакет |
| `warp rollback <pkg>` | Откатиться на предыдущую установленную версию |
| `warp list` / `warp info <pkg>` | Установленные пакеты / подробности |
| `warp repo list\|add\|remove\|enable\|disable` | Управление репозиториями |
| `warp delta <old> <new> <out>` | Построить бинарную дельту между двумя архивами |
| `warp delta-apply <old> <delta> <out>` | Восстановить новый архив из старого и дельты |
| `warp keygen [priv pub]` | Сгенерировать пару ключей Ed25519 |
| `warp sign <file> [priv]` | Записать отделённую base64-подпись `<file>.sig` |
| `warp pack <dir>` | Собрать архив `.warp` из каталога |
| `warp seed` / `warp volunteer` | Раздавать установленные пакеты пирам (P2P-транспорт) |

## Репозитории

Встроенный репозиторий `k1os` (зеркала на GitHub, GitLab и GitVerse, ключ
вкомпилирован в бинарник) есть всегда, удалить его нельзя. Дополнительные
репозитории хранятся в `/var/lib/warp/repos.json`, у каждого свой кэш в
`/var/lib/warp/repos/<name>/`.

```bash
warp repo add lab https://mirror.example/lab --pubkey <hex64> [--mirror <url>]...
warp repo add lab https://mirror.example/lab --pubkey-file lab.pub
warp repo list
warp repo disable lab      # оставить в конфиге, но пропускать
warp repo remove lab
```

Публичный ключ при `add` **обязателен**: понятия «недоверенный репозиторий» у
WARP нет. Имя репозитория — `[a-z0-9_-]`; если два включённых репозитория
публикуют пакет с одним именем, побеждает тот, что идёт первым в списке, а
`warp install other/pkg` выбирает конкретный.

`warp update` завершается с кодом 1, если хотя бы один включённый репозиторий
обновить не удалось (подпись не сошлась или ни одно зеркало не ответило).
Пакеты из остальных репозиториев при этом остаются доступны.

### Первый репозиторий: `keytron`

Первый публичный репозиторий помимо встроенного `k1os` — `keytron` на
[keytron-prime.org](https://keytron-prime.org). Добавить его можно одной командой:

```bash
sudo warp repo add keytron https://keytron-prime.org/packages/keytron --pubkey 53d0a36597b812873cdaa42b11b08592ac3f0998998ccb7e59d6833640f9d883
sudo warp update
```

Ключ можно сверить с опубликованным:
[`pubkey.hex`](https://keytron-prime.org/packages/keytron/pubkey.hex).
Ключ подписи этого репозитория на сервере раздачи не хранится.

### Свой репозиторий

Подходит любой статический HTTP-хостинг. Положите архивы `.warp` в каталог и
выполните:

```bash
warp keygen repo.priv repo.pub                       # один раз
tools/make-index.py <dir> --base-url https://mirror.example/lab --key repo.priv
```

`make-index.py` перечисляет реально присутствующие архивы (настоящие `sha256`
и размер; записи без архива выбрасываются — индекс никогда не обещает того,
чего зеркало не отдаст), строит дельты от старых архивов, оставшихся в
каталоге, и подписывает результат через `warp sign`. Каталог выкладывается как
есть; пользователям раздаётся `repo.pub`.

Индекс и архивы встроенного `k1os` живут в ветке `packages` репозитория
`KEYTRON/K1OS` (Git LFS для `.warp`) и отдаются с зеркал:

1. `https://github.com/KEYTRON/K1OS/raw/packages`
2. `https://gitlab.com/KEYTRON/K1OS/-/raw/packages`
3. `https://gitverse.ru/keytron46/K1OS/raw/branch/packages`

## Обновления и дельта-обновления

`warp upgrade` обновляет индексы и сравнивает `sha256` каждого установленного
архива с индексом. Если в индексе есть дельта, чей `from_sha256` совпадает с
архивом, уже лежащим в хранилище, WARP скачивает только дельту, проверяет её
собственный хэш, локально восстанавливает новый архив через `delta-apply` и
затем сверяет результат с `sha256` полного архива из индекса — дельта не может
дать байты, которые индекс не подписал. Если подходящей дельты нет или зеркало
её не отдаёт, WARP качает полный архив. Предыдущая версия остаётся в хранилище
для `rollback`.

Дельты content-defined: архив режется на переменные куски по скользящей
gear-хэш границе (2–64 KiB, в среднем ~16 KiB), поэтому вставка в начало файла
не сдвигает остальное. Формат `WARPDLT1`: заголовок со старым и новым размером и
`sha256`, затем поток операций `COPY(offset, len)` / `LIT(bytes)`. Чтобы дельты
окупались, архивы должны сжиматься воспроизводимо — паковать через
`gzip -n --rsyncable` (так делают `tools/make-package.sh` и `make-index.py`);
обычный `gzip` перемешивает весь поток после первого изменённого байта.

Типичные цифры из тестов: архив 2 MiB с 50 KiB изменений → дельта ~100 KiB.
Дельты окупаются только когда архив велик относительно изменения — архив
40 KiB с почти полностью переписанным кодом (warp 0.3.3 → 0.4.0) не
выигрывает ничего, поэтому `make-index.py` не публикует дельты размером 90 % и
более от полного архива.

## Зависимости

Зависимости живут в **подписанном индексе**, а не в архиве:

```json
"deps": [ {"name": "openssl", "version": "3.3.2"}, {"name": "libfoo", "version": "1.2", "repo": "lab"} ]
```

Каждая зависимость закреплена именем и точной версией (а значит точным
`sha256`) в том же репозитории, если `repo` явно не указывает другой —
репозиторий не может подтянуть пакеты из репозитория, который не назвал, а
пакет нельзя удовлетворить «похожей» версией из недоверенного источника.
Манифест внутри архива может повторять список для людей; расхождение с
индексом — отказ в установке. Разрешение зависимостей поверх этого замкнутого
мира — следующий пункт плана; сейчас клиент разбирает и показывает `deps`.

## Формат индекса

```json
{
  "timestamp": "2026-09-19",
  "packages": {
    "warp": {
      "version": "0.4.0",
      "description": "...",
      "sha256": "…", "size": 6104096,
      "url": "https://…/warp-0.4.0-x86_64.warp",
      "deltas": [
        {"from_version": "0.3.3", "from_sha256": "…", "url": "https://…/warp-0.3.3-to-0.4.0-x86_64.warpdelta", "sha256": "…", "size": 180439}
      ],
      "deps": [],
      "variants": [ {"kind": "torrent", "url": "magnet:?…", "sha256": "…", "priority": 10} ]
    }
  }
}
```

`variants` необязателен: `kind` — `direct`, `torrent`, `magnet`, `p2p`, `http`,
`https` или `file`; внутри одного класса транспорта побеждает больший
`priority`. Без него используются верхнеуровневые `url`/`sha256`.
`index.json.sig` — base64-подпись Ed25519 сырых байтов `index.json`
(`warp sign index.json`), а не поле внутри JSON: подпись не может покрывать
документ, который содержит её саму.

## Тесты

```bash
sh tests/delta-roundtrip.sh        # сборка/применение дельты, отказ на чужой базе и обрезанной дельте
sh tests/e2e-delta-upgrade.sh      # свой репозиторий по HTTP: install → upgrade через дельту → rollback, в контейнере K1OS
```

Второму тесту нужны Docker и `ghcr.io/keytron/k1os:latest`; оба выполняются в
CI (`warp-tests.yml`).
