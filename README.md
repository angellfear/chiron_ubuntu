# Сборка на базе Ubuntu 26.04 для Duo Max

Репозиторий интеграции поддержки платы Chiron в официальную систему сборки
Khadas Fenix. Результат — образ Ubuntu 26.04 arm64 с рабочим столом Wayland
и интерфейсом управления устройством (профиль `graphical`).

Репозиторий не содержит деревья исходников upstream, собранные образы и данные
доступа к устройствам. Fenix, Khadas Linux, `common_drivers`, драйвер Wi-Fi W2
хранятся отдельно и зафиксированы точными коммитами в
[`sources.lock`](sources.lock). Изменения upstream оформлены очередями в
[`patches/`](patches) и применяются через `git am` во временном workspace
сборщика.


## Что это и чего здесь нет

Это операционная система и система её сборки. Средств доступа к устройству
здесь нет — ни инструмента, ни инструкции, ни ссылки на них, — и добавлять
их сюда не нужно.

Плата должна уже уметь запускать код, не подписанный производителем. Это
условие на входе, а не то, что делает этот репозиторий; подойдёт поэтому не
всякий экземпляр, и помочь с этим репозиторий ничем не может.

Предполагается, что устройство ваше собственное и систему на нём вы меняете
себе. За недостатки, вызванные этой заменой, производитель не отвечает.

Проект независимый и не связан ни с ООО «Яндекс», ни с Canonical, ни с
Khadas, ни с Amlogic. Их названия упоминаются только для того, чтобы сказать,
с чем эта сборка работает; товарные знаки принадлежат их владельцам. Это не
официальная Ubuntu, а сборка на её пакетах.


## Первая загрузка

Образ поставляется без учётной записи — она создаётся при первой загрузке из
[`etc/default/chiron-firstboot`](rootfs/graphical/etc/default/chiron-firstboot):

| Пользователь | Пароль |
|---|---|
| `user` | `1234` |

Пароль опубликован и его стоит сменить — `passwd` в терминале на плате. Сессия
входит автоматически, поэтому принудительная смена при первом входе выключена:
просроченный пароль не даёт greetd запустить сессию, и плата без клавиатуры
осталась бы с чёрным экраном.

Чтобы выпустить образ с другой учётной записью, отредактируйте тот же файл —
пересобирать образ не нужно, он читается на самой плате при загрузке.

Те же логин и пароль лежат на рабочем столе в файле `Логин и пароль.txt`:
плате без клавиатуры и без второй машины больше негде их показать. Файл
пишется один раз, в тот момент, когда учётная запись создаётся, — после
смены пароля он был бы неверен, поэтому удалённым и остаётся удалённым.

SSH установлен и настроен, но выключен. Пароль опубликован, а плата,
отвечающая на 22-й порт сразу после включения, доступна всем в сети раньше,
чем до неё дойдут руки владельца. Включает его ярлык «Включить SSH» на
рабочем столе: он срабатывает один раз и исчезает, а SSH работает дальше, в
том числе после перезагрузки. Обратно — `systemctl disable --now ssh.socket`.

Ключи хоста SSH и `machine-id` в образе намеренно отсутствуют: каждая плата
создаёт свои при первой загрузке. Иначе все копии образа отвечали бы одним
ключом и были бы неразличимы.

При первой загрузке плата один раз перезагружается сама: она переносит
вендорские прошивки и настройки усилителей со своих разделов Android. Потом,
как только плата подключится к сети, она скачивает у Khadas библиотеки
Amlogic для аппаратного видео, камеры и NPU. Пока этого не произошло, камеры,
аппаратного кодирования и NPU нет; всё остальное работает. Перезагружаться
для этого не нужно — на рабочий стол придёт уведомление.

## Состояние оборудования

| Устройство | Статус | Примечание |
|---|---|---|
| eMMC и таблица разделов Amlogic MPT | Работает | MPT разбирается собственным кодом; узел `partitions` в device tree не используется. |
| Wi-Fi W2, SDIO fullmac | Работает | Диапазоны 2,4 и 5 ГГц. |
| Bluetooth W2, UART H4 | Работает | Ограничения совместной работы с Wi-Fi описаны в разделе [Bluetooth и Wi-Fi](#ru-bluetooth). |
| MIPI-DSI дисплей и подсветка | Работает | Панель 1200×1920 выбирается параметром `panel_type=mipi_2`. |
| GPU Mali-G52, Panfrost | Работает | Композитинг рабочего стола выполняется на GPU; тесты производительности не проводились. |
| Сенсорный экран Himax HX83102E | Работает | — |
| Аудиосистема 2.1, два FS2105 на TDM-B | Работает | — |
| Массив микрофонов, 8-канальный PDM | Работает | — |
| LED-кольцо AW21036 | Работает | Четыре контроллера, 144 канала и 153 узла в `/sys/class/leds`. |
| Кнопки громкости | Работает | — |
| Шторка камеры и выключатель микрофона | Принято | — |
| Датчик освещённости | Работает | — |
| Акселерометр и автоповорот | Работает | — |
| Аппаратные видеокодеки VDEC/VENC/GE2D | Работает | — |
| Камера OV13B10 | Работает | — |
| OP-TEE | Работает | — |
| NPU ADLA | Работает | Доступен `/dev/adla0`; inference runtime в образ пока не входит. |
| Zigbee Telink ZBHCI | Работает | Координатор доступен через `/dev/ttyS3`. |
| USB host| Работает | — |
| USB gadget| Не реализован | — |

## Программные компоненты

| Компонент | Статус | Примечание |
|---|---|---|
| `chiron-privacy` | Проверено на устройстве | Настройки и сценарии работы описаны в разделе [Chiron Privacy](#ru-privacy).  |

<a id="ru-bluetooth"></a>
### Bluetooth и Wi-Fi

При работе Wi-Fi в диапазоне 2,4 ГГц поток A2DP испытывает нехватку эфирного
времени: оба радиомодуля используют общий тракт, а арбитр делит время, а не
частоту. Измеренная доля Bluetooth составляет 26–31% — 17–28 ACL-пакетов/с при
необходимых для aptX примерно 71. В диапазоне 5 ГГц получено 73 пакета/с без
пауз длиннее 30 мс. Во время установления соединения контроллер запрашивает
приоритет и получает около 81%, но во время воспроизведения такой запрос не
выполняется. HFP/SCO не выдаёт звук.

<a id="ru-privacy"></a>
### Chiron Privacy

`chiron-privacy` — нативный компонент на C/GTK для аппаратных переключателей,
датчика освещённости и политики экрана. Он предоставляет:

- отдельные индикаторы закрытой камеры и выключенного микрофона;
- окно настроек с крупными элементами управления для 10-дюймового сенсорного
  экрана;
- включение и отключение ALS, редактируемую кривую «освещённость → яркость» и
  готовые профили;
- действия по таймауту: погасить подсветку, показать часы или оставить экран
  включённым;
- полноэкранные часы с выбором шрифта, размера и русского или английского языка;
- ночной режим с красным текстом ниже настраиваемого порога освещённости;
- сохранение настроек между запусками.

При погашении Wayland output остаётся включённым, а физическая подсветка
переводится в `0`. Полноэкранный input overlay поглощает первое касание, чтобы
оно не попало в приложение под чёрным экраном. Касание, `Volume+`, `Volume−` и
оба положения переключателей камеры и микрофона пробуждают экран. `wlopm`
используется только для настоящего suspend/resume (не реализовано).

## Сборка

Сборка из checkout этого репозитория:

```bash
./scripts/builder/prepare-workspace.sh <run-name>
./scripts/builder/apply-production-patches.sh /home/user/ubuntu/<run-name>
./scripts/builder/build-profile.sh /home/user/ubuntu/<run-name> graphical
```

`prepare-workspace` создаёт новый workspace с зафиксированными
upstream-деревьями, `apply-production-patches` применяет очереди из `patches/`,
а `build-profile` собирает ядро и rootfs через Fenix, после чего формирует
загрузочные файлы и `<profile>.sqfs` в `artifacts/<profile>/`. Третий аргумент
`kernel-only` останавливает процесс после сборки ядра, device tree и модулей.

### Кэш пакетных слоёв

Установка пакетов под `qemu-user` — самая длительная часть сборки образа.
`assemble-output.sh` сохраняет воспроизводимый результат этой стадии вне
workspace, в `$MIRROR_ROOT/rootfs-layers`:

```text
Fenix base rootfs
  → источники APT, preferences и keyrings
  → базовые пакеты          → base-<key>.tar.lz4
  → пакеты профиля          → <profile>-<key>.tar.lz4
```

Ключ слоя — SHA-256 всех входов, влияющих на его содержимое: базового rootfs,
списков пакетов, конфигурации APT, версии Ubuntu и архитектуры. Изменение любого
входа создаёт новый ключ и повторяет установку пакетов. Ядро, модули и
overlay `rootfs/` накладываются после восстановления слоя и в кэш не
входят. Использованный слой записывается как `ROOTFS_LAYER` в `manifest.env`.
Слои создаются через временное имя и атомарное переименование; автоматически
они не удаляются.

---

## Лицензия

Всё, что написано для этого проекта, — под **0BSD**
([`LICENSES/0BSD.txt`](LICENSES/0BSD.txt)): берите, меняйте, собирайте,
распространяйте, продавайте — с упоминанием автора или без, разрешения
спрашивать не нужно, условий нет. Это подарок, а не обмен.

Исключение — `patches/` и `drivers/`: это правки к ядру Linux и модули к нему,
то есть производные от кода под GPL-2.0, и они под **GPL-2.0**
([`LICENSES/GPL-2.0.txt`](LICENSES/GPL-2.0.txt)). Тут выбора нет — этого
требует сам код, от которого они производны.

0BSD покрывает то, что написано здесь, и только это. В тех же каталогах лежит
один файл данных, который проекту не принадлежит, — заводская калибровка
камеры, измеренная не нами; этой лицензией он не раздаётся. Он назван в
[`THIRD_PARTY.md`](THIRD_PARTY.md).

Точный ответ для каждого файла — в его собственном заголовке
`SPDX-License-Identifier`. Карта целиком — в [`LICENSE`](LICENSE).

Образ, собранный из этого репозитория, содержит изменённое ядро Linux. GPL-2.0
§3 обязывает того, кто такой образ распространяет, предоставить соответствующий
исходный код. `sources.lock` фиксирует каждое upstream-дерево по коммиту, а
`patches/` хранит все изменения, но сами деревья лежат в чужих репозиториях и
могут оттуда исчезнуть. Поэтому к образу прикладывайте архив исходников:
`scripts/builder/export-sources.sh <workspace>` собирает его из того же
workspace, из которого собран образ.

Чужие материалы, их происхождение и условия — в
[`THIRD_PARTY.md`](THIRD_PARTY.md). Прав на распространение вендорских
прошивок и библиотек у проекта нет. Поэтому прошивки и настройки усилителей
плата берёт из своих разделов Android при первой загрузке, а библиотеки Amlogic
скачивает у Khadas сама. Исключение одно — прошивка Wi-Fi W2
(`wifi_w2_fw_sdio.bin` и `aml_wifi_rf.txt`), взятая из публичного
репозитория драйвера Khadas. Заводская прошивка с платы с этим драйвером
не работает, это проверено, а без Wi-Fi плата не может ничего скачать.
Почему так — в [`THIRD_PARTY.md`](THIRD_PARTY.md).

Если вы обладаете правами на что-либо в этом репозитории — указано оно в
[`THIRD_PARTY.md`](THIRD_PARTY.md) или нет — и возражаете против его
использования здесь, это будет убрано. Заведите issue или, если не хотите
публично, воспользуйтесь каналом жалоб самой площадки. Доказывать право
через суд заранее не нужно — достаточно обоснованного обращения
правообладателя или его представителя. Проект некоммерческий; по такому
обращению материал удаляется или переносится в first-boot import (файл даёт
плата, репозиторий хранит только контрольную сумму) настолько быстро,
насколько это возможно.


---

# An Ubuntu 26.04-based system for Duo Max

Integration repository that adds Chiron board support to the official Khadas
Fenix build system. It produces an Ubuntu 26.04 arm64 image with a Wayland
desktop and device controls (the `graphical` profile).

The repository does not contain upstream source trees, generated images or
device credentials. Fenix, Khadas Linux, `common_drivers` and the W2 Wi-Fi
driver remain separate repositories pinned to exact commits in
[`sources.lock`](sources.lock). Upstream changes are stored as queues in
[`patches/`](patches) and applied with `git am` in a temporary builder
workspace.

## What this is, and what is not here

This is an operating system and the system that builds it. Nothing here gets
into a device — no tool, no instructions, no link to either — and nothing of
that kind belongs here.

The board has to already be able to run code the manufacturer did not sign.
That is a precondition, not something this repository does; not every unit
qualifies, and this repository cannot help with that.

The assumption is that the device is your own and you are changing the system
on it for yourself. The manufacturer does not answer for faults that change
causes.

This is an independent project, not affiliated with Yandex, Canonical, Khadas
or Amlogic. Their names appear only to say what this works with; the
trademarks belong to their owners. This is not an official Ubuntu release but
a system built from its packages.

## First boot

The image ships without an account. One is created on the first boot from
[`etc/default/chiron-firstboot`](rootfs/graphical/etc/default/chiron-firstboot):

| User | Password |
|---|---|
| `user` | `1234` |

That password is published, so change it — `passwd` in a terminal on the board.
The session logs in automatically, which is why a forced change at first login
is off: an expired password stops greetd from starting the session at all, and
a board with no keyboard would be left at a black screen.

To ship the image with a different account, edit that same file. No rebuild is
needed; it is read on the board itself at boot.

The same login and password are on the desktop, in `Логин и пароль.txt`: a
board with no keyboard and no second machine has nowhere else to show them.
It is written once, at the moment the account is created — after a password
change it would be wrong — so deleting it keeps it deleted.

SSH is installed and configured, and switched off. The password is published,
and a board answering port 22 the moment it is plugged in is reachable by
everyone on that network before its owner has touched it. The "Включить SSH"
shortcut on the desktop turns it on: it works once and then removes itself,
and SSH keeps running, reboots included. Turning it back off is
`systemctl disable --now ssh.socket`.

SSH host keys and the machine id are deliberately absent from the image, and
each board generates its own on the first boot. Otherwise every copy would
answer with the same key and be indistinguishable from every other board.

On the first boot the board reboots once by itself: it copies the vendor
firmware and amplifier tuning off its own Android volumes. Then, as soon as it
is on a network, it downloads the Amlogic libraries for hardware video, the
camera and the NPU from Khadas. Until that has happened there is no camera,
no hardware encoding and no NPU; everything else works. No reboot is needed
for it -- a desktop notification says when it is done.

## Hardware status

| Device | Status | Notes |
|---|---|---|
| eMMC and Amlogic MPT partition table | Works | MPT is parsed by dedicated code; no `partitions` node is used in the device tree. |
| W2 Wi-Fi, SDIO fullmac | Works | 2.4 and 5 GHz bands. |
| W2 Bluetooth, UART H4 | Works | Coexistence limitations are documented under [Bluetooth and Wi-Fi](#en-bluetooth). |
| MIPI-DSI display and backlight | Works | The 1200×1920 panel is selected with `panel_type=mipi_2`. |
| Mali-G52 GPU, Panfrost | Works | Desktop composition runs on the GPU; no performance benchmark has been completed. |
| Himax HX83102E touchscreen | Works | — |
| 2.1 audio, two FS2105 amplifiers on TDM-B | Works | — |
| Eight-channel PDM microphone array | Works | — |
| AW21036 LED ring | Works | Four controllers, 144 channels and 153 nodes under `/sys/class/leds`. |
| Volume buttons | Works | — |
| Camera cover and microphone switch | Accepted | — |
| Ambient-light sensor | Works | — |
| Accelerometer and autorotation | Works | — |
| VDEC/VENC/GE2D hardware video codecs | Works | — |
| OV13B10 camera | Works | — |
| OP-TEE | Works | — |
| ADLA NPU | Works | `/dev/adla0` is available; no inference runtime is included yet. |
| Telink ZBHCI Zigbee | Works | The coordinator is exposed as `/dev/ttyS3`. |
| USB host | Works | — |
| USB gadget | Not implemented | — |

## Software components

| Component | Status | Notes |
|---|---|---|
| `chiron-privacy` | Validated on device | Settings and usage scenarios are documented under [Chiron Privacy](#en-privacy). |

<a id="en-bluetooth"></a>
### Bluetooth and Wi-Fi

With Wi-Fi operating on 2.4 GHz, A2DP is starved of airtime. Both radios share
one front end and the arbiter divides time rather than frequency. Bluetooth was
measured at 26–31% of airtime, producing 17–28 ACL packets/s against roughly 71
required by aptX. On 5 GHz it reached 73 packets/s with no gap above 30 ms. The
controller requests priority during connection setup and receives about 81%,
but does not make that request while streaming. HFP/SCO produces no audio.

<a id="en-privacy"></a>
### Chiron Privacy

`chiron-privacy` is a native C/GTK component for hardware privacy switches,
ambient-light control and display policy. It provides:

- separate indicators for a covered camera and muted microphone;
- a touch-oriented settings window for the 10-inch display;
- ALS enable/disable, editable illuminance-to-brightness curves and presets;
- timeout actions to blank the backlight, show the clock or keep the display on;
- a fullscreen clock with configurable font, size and language (Russian or
  English);
- a red night mode below a configurable illuminance threshold;
- persistent settings.

Blanking keeps the Wayland output enabled and writes `0` only to the physical
backlight. A fullscreen input overlay consumes the first touch so it cannot
reach the application below the black screen. Touch, `Volume+`, `Volume−` and
both positions of the camera and microphone switches wake the display.
`wlopm` is used only for real suspend/resume, which is not implemented.

## Building

Build from a checkout of this repository:

```bash
./scripts/builder/prepare-workspace.sh <run-name>
./scripts/builder/apply-production-patches.sh /home/user/ubuntu/<run-name>
./scripts/builder/build-profile.sh /home/user/ubuntu/<run-name> graphical
```

`prepare-workspace` creates a fresh temporary workspace with the pinned
upstream trees, `apply-production-patches` applies the queues from `patches/`,
and `build-profile` builds the kernel and rootfs through Fenix before producing
the boot files and `<profile>.sqfs` under `artifacts/<profile>/`. Passing `kernel-only` as
the third argument stops after the kernel, device trees and modules.

### Package-layer cache

Package installation under `qemu-user` is the slowest image-build stage.
`assemble-output.sh` stores a reproducible result outside the workspace under
`$MIRROR_ROOT/rootfs-layers`:

```text
Fenix base rootfs
  → APT sources, preferences and keyrings
  → base packages             → base-<key>.tar.lz4
  → profile packages          → <profile>-<key>.tar.lz4
```

The layer key is a SHA-256 of every input that affects its contents: the base
rootfs, package lists, APT configuration, Ubuntu release and architecture. Any
input change creates a new key and reruns package installation. The kernel,
modules and the `rootfs/` overlay are applied after restoring
the layer and are not cached in it. The selected layer is recorded as
`ROOTFS_LAYER` in `manifest.env`. Layers are written under a temporary name and
renamed atomically; they are never pruned automatically.

## Licence

Everything written for this project is under **0BSD**
([`LICENSES/0BSD.txt`](LICENSES/0BSD.txt)): take it, change it, build on it,
ship it, sell it, with or without credit, without asking and without conditions.
It is a gift, not a trade.

The exception is `patches/` and `drivers/` — kernel modifications and kernel
modules, which are derivative works of GPL-2.0 code and are therefore under
**GPL-2.0** ([`LICENSES/GPL-2.0.txt`](LICENSES/GPL-2.0.txt)). That is not a
choice made here; it is what the code they derive from already requires.

0BSD covers what was written here, and only that. One data file in the same
directories is not this project's to give away -- the camera's factory
calibration, measured by others. It is named in
[`THIRD_PARTY.md`](THIRD_PARTY.md).

The authoritative answer for any single file is its own
`SPDX-License-Identifier` header. The whole map is in [`LICENSE`](LICENSE).

An image built from this repository contains a modified Linux kernel. GPL-2.0 §3
obliges whoever distributes such an image to provide the corresponding source.
`sources.lock` pins every upstream tree by commit and `patches/` carries every
change, but the trees themselves live in other people's repositories and can
disappear from there. So ship the source archive with the image:
`scripts/builder/export-sources.sh <workspace>` packs it from the same
workspace the image was built in.

Third-party material, where it came from and on what terms, is recorded in
[`THIRD_PARTY.md`](THIRD_PARTY.md). This project has no right to redistribute
vendor firmware or libraries. So the board copies the firmware and amplifier
tuning off its own Android volumes on first boot, and downloads the Amlogic
libraries from Khadas itself. There is one exception: the W2 Wi-Fi firmware
(`wifi_w2_fw_sdio.bin` and `aml_wifi_rf.txt`), taken from Khadas' public
driver repository. The board's own factory firmware does not work with this
driver -- that was tested -- and without Wi-Fi the board cannot download
anything. [`THIRD_PARTY.md`](THIRD_PARTY.md) explains why.

If you hold rights in anything in this repository -- whether or not it is
named in [`THIRD_PARTY.md`](THIRD_PARTY.md) -- and object to its use here, it
will be taken down. Open an issue, or use the hosting platform's report channel
if you would rather not do so in public; you do not need to establish the claim
in court first. This project is non-commercial, and on such a request the
material is removed or moved to the first-boot import as promptly as possible.
See **Reporting** in [`THIRD_PARTY.md`](THIRD_PARTY.md).
