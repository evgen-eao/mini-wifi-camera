# Hi3518EV300 + JX-H62 Cloud Camera: Local Streamer & OpenIPC Port

Реверс-инжиниринг бюджетной облачной Wi-Fi камеры на базе процессора **HiSilicon Hi3518EV300** и сенсора **SOI JX-H62** (P2P-платформа PPCS / UID `DGK-*`, приложения *HDWiFiCamPro, LookCam, V380* и аналоги).

Проект позволяет:
1. **Получать прямой видеопоток H.264 (HTTP-FLV / Raw H.264)** на стоковой прошивке по локальной сети без китайских мобильных приложений, облаков и доступа в интернет.
2. **Смотреть видео в любых плеерах** (PotPlayer, VLC, MPC-HC, ffplay) и интегрировать камеру в системы умного дома (Home Assistant, Frigate, go2rtc, MediaMTX).
3. **Установить альтернативную прошивку OpenIPC** с помощью готового драйвера сенсора `libsns_jxh62.so` (DVP 10-bit) и заводских таблиц цветокоррекции из папки [`openipc/`](./openipc/).

---

## 🛠 Аппаратные характеристики камеры

| Компонент | Описание |
| :--- | :--- |
| **SoC (Процессор)** | HiSilicon `Hi3518EV300` (ARMv7, SDK `Hi3516EV200_MPP_V1.0.1.1`) |
| **ОЗУ (RAM)** | 64 МБ (45 МБ OS Linux + 19 МБ MMZ под медиа-буферы на стоке) |
| **Флеш-память (ROM)** | SPI NOR Flash `Winbond W25Q64FV / W25Q64JV` (8 МБ, 64K block) |
| **Сенсор (Матрица)** | **Silicon Optronics (SOI) JX-H62** (1 МП, 1280×720 @ 30 fps, Bayer `BGGR`) |
| **Интерфейс матрицы** | Параллельный **DVP 10-bit** (`VI_DATA0..VI_DATA9`, `VI_CLK`, `VI_HS`, `VI_VS`), I2C0 (`0x60`) |
| **Wi-Fi модуль** | Realtek `RTL8188FU` (USB 2.0, 2.4 ГГц) |
| **Кнопка Mode / Reset** | `GPIO0_0` (активный уровень `0`) |
| **Файловая система (сток)** | `/mnt/userdata` (раздел `app`, JFFS2, чтение/запись), основной бинарник: `/mnt/userdata/app/target` |

### Разметка MTD на стоковой прошивке (SPI NOR 8 MB)
```text
bootargs: mem=45M console=ttyAMA0,115200 root=/dev/mtdblock2 coherent_pool=2M rootfstype=squashfs
mtdparts=hi_sfc:320K(u-boot.bin),2176K(kernel),4480K(rootfs),1216K(app)
```

---

## 📡 Расшифрованный локальный протокол камеры (Стоковая прошивка)

Монолитное приложение `/mnt/userdata/app/target` помимо облачного протокола PPCS поднимает локальные сервисы:
* **UDP `8600`** (`0x2198`) — `Discovery Service` (регистрация IP-адреса клиента во внутренней битовой маске разрешенных адресов).
* **UDP `13140`** (`0x3354`) — `LocalUDP Control Service` (управление потоком, разрешением, звуком и подсветкой).
* **TCP `10002`** (`0x2712`) — `Live Video TCP Server` (отдача видеопотока H.264).
* **TCP `23`** — `Telnet` (включен по умолчанию, логин `root` без пароля).

### Алгоритм запуска видеопотока (Handshake)
1. Клиент устанавливает TCP-соединение с портом **`10002`** камеры. Камера регистрирует сокет, но ставит поток на паузу.
2. Клиент отправляет любой 4-байтный UDP-пакет (`01 00 00 00`) на порт **`8600`**. Камера вызывает `SetupDeviceID()` и заносит IP клиента в белый список.
3. Клиент отправляет управляющий UDP-пакет длиной **216 байт** на порт **`13140`**:
   * Байты `0..N`: ASCII-строка с локальным IP-адресом клиента, завершающаяся нулем (например, `"192.168.1.163\0"`), которая проверяется камерой через `inet_addr()`.
   * Байты `200..203` (`offset 0xC8`, `uint32_t LE`): Код команды (`cmd`):
     * `15` (`0x0F`) — **Start Live Video** (активирует передачу кадров в TCP-сокет `10002` и вызывает `HI_MPI_VENC_RequestIDR`).
     * `16` (`0x10`) — **Stop Live Video**.
     * `17` (`0x11`) — **Audio Toggle** (вкл/выкл звук).
     * `18` (`0x12`) — **Switch Resolution** (переключение канала `1280x720` / `640x480`).

### Структура видеопакета на TCP-порту `10002`
Каждый кадр предваряется **32-байтным заголовком**, за которым следует чистый поток **H.264 Annex-B** (`00 00 00 01 ...`):

| Смещение | Размер | Тип | Описание |
| :--- | :--- | :--- | :--- |
| `0x00` (0) | 4 байта | `byte[4]` | Сигнатура заголовка (`55 AA 15 A8`) |
| `0x04` (4) | 4 байта | `uint32 LE` | Тип кадра: **`0`** = Ключевой кадр (`SPS + PPS + SEI + IDR`), **`1`** = `P-Frame` |
| `0x08` (8) | 8 байт | `uint64 LE` | Таймштамп кадра `PTS` в миллисекундах со старта камеры |
| `0x10` (16)| 4 байта | `uint32 LE` | **Длина полезной нагрузки (`frame_len`)** в байтах |
| `0x14` (20)| 12 байт | `byte[12]` | Служебные поля (тип потока, FPS/GOP `0x0F`) |
| `0x20` (32)| `frame_len` | `byte[]` | Полезная нагрузка: сырой кадр **H.264 Annex-B** |

---

## 🚀 Быстрый старт на стоковой прошивке (Python HTTP-FLV / H.264 Прокси-сервер)

Скрипт `cam_server.py` работает без сторонних зависимостей (используется только стандартная библиотека Python 3). Он автоматически проходит UDP-авторизацию, держит соединение с камерой, кэширует последний ключевой кадр (`SPS/PPS/IDR`) для мгновенного запуска плееров и на лету упаковывает видео с аппаратными таймштампами в потоковый контейнер **FLV**.

### 1. Запуск сервера
Укажите IP-адрес вашей камеры в переменной `CAM_IP` внутри `cam_server.py` и запустите:
```bash
python cam_server.py
```

### 2. Просмотр видеопотока
* **PotPlayer / VLC / MPC-HC / Браузеры (рекомендуется контейнер FLV):**
  Откройте URL:
  ```text
  http://127.0.0.1:8888/live.flv
  ```
* **FFplay (минимальная задержка, сырой H.264):**
  ```bash
  ffplay -fflags nobuffer -flags low_delay -framedrop http://127.0.0.1:8888/live.h264
  ```
* **Проброс в MediaMTX / go2rtc / Home Assistant (RTSP):**
  ```yaml
  streams:
    camera_h62:
      - ffmpeg:http://127.0.0.1:8888/live.flv#video=copy
  ```

---

## 🧩 Драйвер и конфигурация SOI JX-H62 для OpenIPC

В папке [`openipc/`](./openipc/) находятся готовая скомпилированная библиотека сенсора и конфигурационные файлы для прошивки **OpenIPC (`hi3518ev300` / `hi3516ev200` / `gk7205v200`)**:
* `openipc/libsns_jxh62.so` — готовый бинарный драйвер матрицы для `majestic`.
* `openipc/jxh62_full.c` — полный исходный код драйвера с заводскими таблицами AE/AWB/CCM/Saturation/BLC и автонастройкой пинов DVP.
* `openipc/jxh62_dvp.ini` — конфигурация интерфейса `VI` / `DVP 10-bit` (`bayer=BGGR`).
* `openipc/jxh62.ini` — профиль настройки изображения (IQ Profile).

### Установка на камеру с OpenIPC
1. Скопируйте файлы из папки `openipc/` на камеру:
   * `libsns_jxh62.so` $\rightarrow$ `/usr/lib/sensors/libsns_jxh62.so` (`chmod 755`)
   * `jxh62_dvp.ini` $\rightarrow$ `/etc/sensors/jxh62_dvp.ini`
   * `jxh62.ini` $\rightarrow$ `/etc/sensors/iq/jxh62.ini`
2. В файле `/etc/majestic.yaml` укажите пути к конфигу сенсора и IQ-профилю:
   ```yaml
   isp:
     sensorConfig: /etc/sensors/jxh62_dvp.ini
     iqProfile: /etc/sensors/iq/jxh62.ini
   ```
3. Перезапустите `majestic`:
   ```sh
   killall -9 majestic && majestic &
   ```

### Сборка `libsns_jxh62.so` из исходников
```bash
arm-linux-gnueabi-gcc -shared -fPIC -O2 -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
  -Iinclude -Ilibraries/include openipc/jxh62_full.c -o libsns_jxh62.so
arm-linux-gnueabi-strip --strip-unneeded libsns_jxh62.so
```

---

### Технические детали портирования драйвера

#### 1. Особенность мультиплексирования пинов DVP 10-bit в OpenIPC
В стоковой прошивке скрипт `/mnt/userdata/config/start.sh` инициализирует пины процессора командой:
```sh
./load3518ev300 -i -sensor0 sc2235 -osmem 45
```
Профиль `sc2235` в стоковом `load3518ev300` переводит все 10 линий данных параллельной шины (`VI_DATA0..VI_DATA9`, регистры `0x112c0018..0x112c003c`) в режим `0x1002` (`VI_DATA`).
В стандартном скрипте `/usr/bin/load_hisilicon` в OpenIPC функция `vi_pins_dvp` содержит ошибку и пропускает 4 регистра (`0x112c0018`, `0x112c001c`, `0x112c0020`, `0x112c0024`), оставляя их в режиме GPIO (`0x1000`), из-за чего теряются 4 бита каждого пикселя и изображение становится размытым и тёмно-зелёным.

В нашем драйвере `jxh62_full.c` функция `jxh62_dvp_pinmux_init()` автоматически выставляет эти 4 регистра в `0x1002` через `/dev/mem` при инициализации сенсора (`jxh62_init`), поэтому ручная правка `/usr/bin/load_hisilicon` не требуется.

#### 2. Таблица инициализации I2C (`720p @ 30fps, DVP 10-bit, MCLK 24MHz, BGGR`)
* **Шина I2C:** `i2c-0`, 8-битный адрес: `0x60` (7-битный `0x30`), `addr_byte = 1`, `data_byte = 1`.
* **Важно:** Регистры `0x1D = 0xFF` и `0x1E = 0x9F` принудительно включают параллельный выход DVP (`DATA[9:0]` + синхросигналы).

```c
void jxh62_linear_720p30_init(VI_PIPE ViPipe)
{
    usleep(100000);
    jxh62_write_register(ViPipe, 0x12, 0x40); // Software Reset / Sleep
    jxh62_write_register(ViPipe, 0x0E, 0x11);
    jxh62_write_register(ViPipe, 0x0F, 0x08);
    jxh62_write_register(ViPipe, 0x10, 0x20);
    jxh62_write_register(ViPipe, 0x11, 0x80);
    jxh62_write_register(ViPipe, 0x19, 0x68);
    jxh62_write_register(ViPipe, 0x20, 0x60); // HTS L (0x0960 = 2400)
    jxh62_write_register(ViPipe, 0x21, 0x09); // HTS H
    jxh62_write_register(ViPipe, 0x22, 0xEE); // VTS L (0x02EE = 750 lines @ 30fps)
    jxh62_write_register(ViPipe, 0x23, 0x02); // VTS H
    jxh62_write_register(ViPipe, 0x24, 0x00);
    jxh62_write_register(ViPipe, 0x25, 0xD0);
    jxh62_write_register(ViPipe, 0x26, 0x25);
    jxh62_write_register(ViPipe, 0x27, 0xEE);
    jxh62_write_register(ViPipe, 0x28, 0x15);
    jxh62_write_register(ViPipe, 0x29, 0x02);
    jxh62_write_register(ViPipe, 0x2A, 0x70);
    jxh62_write_register(ViPipe, 0x2B, 0x21);
    jxh62_write_register(ViPipe, 0x2C, 0x08);
    jxh62_write_register(ViPipe, 0x2D, 0x01);
    jxh62_write_register(ViPipe, 0x2E, 0xBB);
    jxh62_write_register(ViPipe, 0x2F, 0xC0);
    jxh62_write_register(ViPipe, 0x41, 0x88);
    jxh62_write_register(ViPipe, 0x42, 0x12);
    jxh62_write_register(ViPipe, 0x39, 0x90);
    jxh62_write_register(ViPipe, 0x1D, 0xFF); // Enable DVP DATA[7:0]
    jxh62_write_register(ViPipe, 0x1E, 0x9F); // Enable DVP DATA[9:8] & Sync
    jxh62_write_register(ViPipe, 0x7A, 0x80);
    jxh62_write_register(ViPipe, 0x31, 0x0C);
    jxh62_write_register(ViPipe, 0x33, 0x4C);
    jxh62_write_register(ViPipe, 0x34, 0x2F);
    jxh62_write_register(ViPipe, 0x35, 0xA3);
    jxh62_write_register(ViPipe, 0x36, 0x05);
    jxh62_write_register(ViPipe, 0x38, 0x53);
    jxh62_write_register(ViPipe, 0x3A, 0x08);
    jxh62_write_register(ViPipe, 0x56, 0x02);
    jxh62_write_register(ViPipe, 0x60, 0x02);
    jxh62_write_register(ViPipe, 0x0D, 0x50);
    jxh62_write_register(ViPipe, 0x57, 0x80);
    jxh62_write_register(ViPipe, 0x58, 0x33);
    jxh62_write_register(ViPipe, 0x5A, 0x04);
    jxh62_write_register(ViPipe, 0x5B, 0xB6);
    jxh62_write_register(ViPipe, 0x5C, 0x08);
    jxh62_write_register(ViPipe, 0x5D, 0x67);
    jxh62_write_register(ViPipe, 0x5E, 0x04);
    jxh62_write_register(ViPipe, 0x5F, 0x08);
    jxh62_write_register(ViPipe, 0x66, 0x28);
    jxh62_write_register(ViPipe, 0x67, 0xF8);
    jxh62_write_register(ViPipe, 0x68, 0x04);
    jxh62_write_register(ViPipe, 0x69, 0x74);
    jxh62_write_register(ViPipe, 0x6A, 0x3F);
    jxh62_write_register(ViPipe, 0x63, 0x82);
    jxh62_write_register(ViPipe, 0x6C, 0xC0);
    jxh62_write_register(ViPipe, 0x6E, 0x5C);
    jxh62_write_register(ViPipe, 0x82, 0x01);
    jxh62_write_register(ViPipe, 0x0C, 0x00);
    jxh62_write_register(ViPipe, 0x46, 0xC2);
    jxh62_write_register(ViPipe, 0x48, 0x7E);
    jxh62_write_register(ViPipe, 0x62, 0x40);
    jxh62_write_register(ViPipe, 0x7D, 0x57);
    jxh62_write_register(ViPipe, 0x7E, 0x28);
    jxh62_write_register(ViPipe, 0x80, 0x00);
    jxh62_write_register(ViPipe, 0x4A, 0x05);
    jxh62_write_register(ViPipe, 0x49, 0x10);
    jxh62_write_register(ViPipe, 0x13, 0x81);
    jxh62_write_register(ViPipe, 0x59, 0x97);
    jxh62_write_register(ViPipe, 0x12, 0x00); // Wake up
    jxh62_write_register(ViPipe, 0x47, 0x47);
    usleep(500000);
    jxh62_write_register(ViPipe, 0x47, 0x44);
    jxh62_write_register(ViPipe, 0x1F, 0x01);
}
```

#### 3. Управление экспозицией, усилением и цветокоррекцией (ISP / AE / AWB Callbacks)
* **Выдержка (`cmos_inttime_update`):**
  * Регистр `0x01` = `u32IntTime & 0xFF`
  * Регистр `0x02` = `(u32IntTime >> 8) & 0xFF`
* **Аналоговое усиление (`cmos_gains_update` + `cmos_again_calc_table`):**
  * Регистр `0x00` = индекс в таблице `again_table` (`1024 .. 22924`).
* **Цифровое усиление (`cmos_dgain_calc_table`):**
  * Фиксируется на `1x` (`*pu32DgainLin = 1024; *pu32DgainDb = 0;` при `AE_ACCURACY_TABLE`), чтобы исключить переполнение расчётного ISO в алгоритме AE HiSilicon.
* **Тайминги (`cmos_fps_set`):**
  * `u32FullLinesStd = 750` (`0x02EE` при 30 fps), `u32MaxIntTime = u32FullLines - 4`.
  * Регистр `0x22` = `u32Lines & 0xFF`, Регистр `0x23` = `(u32Lines >> 8) & 0xFF`.
* **Калибровка ISP (`cmos_get_isp_black_level`, `cmos_get_awb_default`):**
  * Уровень чёрного (`Black Level`): `68` (`0x44`) по всем 4 каналам.
  * Параметры AWB: `u16WbRefTemp = 4950`, `au16GainOffset = {350, 256, 256, 405}`, `as32WbPara = {207, -141, -190, 186726, 128, -136275}`.
  * Матрицы цветокоррекции (`CCM`): 4 калибровочные таблицы для `6150K`, `4850K`, `3650K`, `2650K` и таблица насыщенности (`AutoSat` до `105` на базовом ISO).
