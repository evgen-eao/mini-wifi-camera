

```markdown
# Hi3518EV300 + JX-H62 Cloud Camera: Local Streamer & Reverse Engineering

Реверс-инжиниринг бюджетной облачной Wi-Fi камеры на базе процессора **HiSilicon Hi3518EV300** и сенсора **SOI JX-H62** (P2P-платформа PPCS / UID `DGK-*`, приложения *HDWiFiCamPro, LookCam, V380* и аналоги).

Проект позволяет:
1. **Получать прямой видеопоток H.264 (HTTP-FLV / Raw H.264)** по локальной сети без китайских мобильных приложений, облаков и доступа в интернет.
2. **Смотреть видео в любых плеерах** (PotPlayer, VLC, MPC-HC, ffplay) и интегрировать камеру в системы умного дома (Home Assistant, Frigate, go2rtc, MediaMTX).
3. **Использовать извлеченный драйвер сенсора `JX-H62` (DVP 10-bit)** для портирования и сборки альтернативной прошивки **OpenIPC**.

---

## 🛠 Аппаратные характеристики камеры

| Компонент | Описание |
| :--- | :--- |
| **SoC (Процессор)** | HiSilicon `Hi3518EV300` (ARMv7, SDK `Hi3516EV200_MPP_V1.0.1.1`) |
| **ОЗУ (RAM)** | 64 МБ (45 МБ OS Linux + 19 МБ MMZ под медиа-буферы) |
| **Флеш-память (ROM)** | SPI NOR Flash `Winbond W25Q64FV / W25Q64JV` (8 МБ, 64K block) |
| **Сенсор (Матрица)** | **Silicon Optronics (SOI) JX-H62** (1 МП, 1280×720 @ 30 fps) |
| **Интерфейс матрицы** | Параллельный **DVP 10-bit** (`VI_DATA2..VI_DATA11`, `VI_CLK`, `VI_HS`, `VI_VS`), I2C0 (`0x60`) |
| **Wi-Fi модуль** | Realtek `RTL8188FU` (USB 2.0, 2.4 ГГц) |
| **Кнопка Mode / Reset** | `GPIO0_0` (активный уровень `0`) |
| **Файловая система** | `/mnt/userdata` (раздел `app`, JFFS2, чтение/запись), основной бинарник: `/mnt/userdata/app/target` |

### Разметка MTD (SPI NOR 8 MB)
```text
bootargs: mem=45M console=ttyAMA0,115200 root=/dev/mtdblock2 coherent_pool=2M rootfstype=squashfs
mtdparts=hi_sfc:320K(u-boot.bin),2176K(kernel),4480K(rootfs),1216K(app)
```

---

## 📡 Расшифрованный локальный протокол камеры

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

## 🚀 Быстрый старт (Python HTTP-FLV / H.264 Прокси-сервер)

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

## 🧩 Данные драйвера матрицы SOI JX-H62 для OpenIPC

В стоковой прошивке скрипт `/mnt/userdata/config/start.sh` загружает конфигурацию пинов ядра командой:
```sh
./load3518ev300 -i -sensor0 sc2235 -osmem 45
```
Однако сам драйвер матрицы **JX-H62** слинкован статически внутри `/mnt/userdata/app/target`. Ниже приведены извлеченные из бинарного кода структуры и таблица инициализации регистров для сборки `libsns_jxh62.so` под `hi3516ev200 / hi3518ev300`.

### 1. Таблица инициализации I2C (`720p @ 30fps, DVP 10-bit, MCLK 24MHz`)
* **Шина I2C:** `i2c-0`, 8-битный адрес: `0x60` (7-битный `0x30`), `addr_byte = 1`, `data_byte = 1`.
* **Важно:** Регистры `0x1D = 0xFF` и `0x1E = 0x9F` принудительно включают параллельный выход DVP (`DATA[9:0]` + синхросигналы).

```c
void soi_sensor_720p30_init(VI_PIPE ViPipe)
{
    usleep(100000);
    soi_sensor_write_register(ViPipe, 0x12, 0x40); // Software Reset / Sleep
    soi_sensor_write_register(ViPipe, 0x0E, 0x11);
    soi_sensor_write_register(ViPipe, 0x0F, 0x08);
    soi_sensor_write_register(ViPipe, 0x10, 0x20);
    soi_sensor_write_register(ViPipe, 0x11, 0x80);
    soi_sensor_write_register(ViPipe, 0x19, 0x68);
    soi_sensor_write_register(ViPipe, 0x20, 0x60); // HTS L (0x0960 = 2400)
    soi_sensor_write_register(ViPipe, 0x21, 0x09); // HTS H
    soi_sensor_write_register(ViPipe, 0x22, 0xEE); // VTS L (0x02EE = 750 lines @ 30fps)
    soi_sensor_write_register(ViPipe, 0x23, 0x02); // VTS H
    soi_sensor_write_register(ViPipe, 0x24, 0x00);
    soi_sensor_write_register(ViPipe, 0x25, 0xD0);
    soi_sensor_write_register(ViPipe, 0x26, 0x25);
    soi_sensor_write_register(ViPipe, 0x27, 0xEE);
    soi_sensor_write_register(ViPipe, 0x28, 0x15);
    soi_sensor_write_register(ViPipe, 0x29, 0x02);
    soi_sensor_write_register(ViPipe, 0x2A, 0x70);
    soi_sensor_write_register(ViPipe, 0x2B, 0x21);
    soi_sensor_write_register(ViPipe, 0x2C, 0x08);
    soi_sensor_write_register(ViPipe, 0x2D, 0x01);
    soi_sensor_write_register(ViPipe, 0x2E, 0xBB);
    soi_sensor_write_register(ViPipe, 0x2F, 0xC0);
    soi_sensor_write_register(ViPipe, 0x41, 0x88);
    soi_sensor_write_register(ViPipe, 0x42, 0x12);
    soi_sensor_write_register(ViPipe, 0x39, 0x90);
    soi_sensor_write_register(ViPipe, 0x1D, 0xFF); // Enable DVP DATA[7:0]
    soi_sensor_write_register(ViPipe, 0x1E, 0x9F); // Enable DVP DATA[9:8] & Sync
    soi_sensor_write_register(ViPipe, 0x7A, 0x80);
    soi_sensor_write_register(ViPipe, 0x31, 0x0C);
    soi_sensor_write_register(ViPipe, 0x33, 0x4C);
    soi_sensor_write_register(ViPipe, 0x34, 0x2F);
    soi_sensor_write_register(ViPipe, 0x35, 0xA3);
    soi_sensor_write_register(ViPipe, 0x36, 0x05);
    soi_sensor_write_register(ViPipe, 0x38, 0x53);
    soi_sensor_write_register(ViPipe, 0x3A, 0x08);
    soi_sensor_write_register(ViPipe, 0x56, 0x02);
    soi_sensor_write_register(ViPipe, 0x60, 0x02);
    soi_sensor_write_register(ViPipe, 0x0D, 0x50);
    soi_sensor_write_register(ViPipe, 0x57, 0x80);
    soi_sensor_write_register(ViPipe, 0x58, 0x33);
    soi_sensor_write_register(ViPipe, 0x5A, 0x04);
    soi_sensor_write_register(ViPipe, 0x5B, 0xB6);
    soi_sensor_write_register(ViPipe, 0x5C, 0x08);
    soi_sensor_write_register(ViPipe, 0x5D, 0x67);
    soi_sensor_write_register(ViPipe, 0x5E, 0x04);
    soi_sensor_write_register(ViPipe, 0x5F, 0x08);
    soi_sensor_write_register(ViPipe, 0x66, 0x28);
    soi_sensor_write_register(ViPipe, 0x67, 0xF8);
    soi_sensor_write_register(ViPipe, 0x68, 0x04);
    soi_sensor_write_register(ViPipe, 0x69, 0x74);
    soi_sensor_write_register(ViPipe, 0x6A, 0x3F);
    soi_sensor_write_register(ViPipe, 0x63, 0x82);
    soi_sensor_write_register(ViPipe, 0x6C, 0xC0);
    soi_sensor_write_register(ViPipe, 0x6E, 0x5C);
    soi_sensor_write_register(ViPipe, 0x82, 0x01);
    soi_sensor_write_register(ViPipe, 0x0C, 0x00);
    soi_sensor_write_register(ViPipe, 0x46, 0xC2);
    soi_sensor_write_register(ViPipe, 0x48, 0x7E);
    soi_sensor_write_register(ViPipe, 0x62, 0x40);
    soi_sensor_write_register(ViPipe, 0x7D, 0x57);
    soi_sensor_write_register(ViPipe, 0x7E, 0x28);
    soi_sensor_write_register(ViPipe, 0x80, 0x00);
    soi_sensor_write_register(ViPipe, 0x4A, 0x05);
    soi_sensor_write_register(ViPipe, 0x49, 0x10);
    soi_sensor_write_register(ViPipe, 0x13, 0x81);
    soi_sensor_write_register(ViPipe, 0x59, 0x97);
    soi_sensor_write_register(ViPipe, 0x12, 0x00); // Wake up
    soi_sensor_write_register(ViPipe, 0x47, 0x47);
    usleep(500000);
    soi_sensor_write_register(ViPipe, 0x47, 0x44);
    soi_sensor_write_register(ViPipe, 0x1F, 0x01);
}
```

### 2. Управление экспозицией и усилением (AE Callbacks)
* **Выдержка (`cmos_inttime_update`):**
  * Регистр `0x01` = `u32IntTime & 0xFF`
  * Регистр `0x02` = `(u32IntTime >> 8) & 0xFF`
* **Аналоговое усиление (`cmos_gains_update`):**
  * Регистр `0x00` = `u32Again`
* **Тайминги (`cmos_fps_set`):**
  * `u32FullLinesStd = 750` (`0x02EE` при 30 fps), `u32MaxIntTime = u32FullLines - 4`.
  * Регистр `0x22` = `u32Lines & 0xFF`, Регистр `0x23` = `(u32Lines >> 8) & 0xFF`.

### 3. Конфигурация профиля сенсора для OpenIPC (`/etc/sensors/jxh62_dvp.ini`)
```ini
[sensor]
model=jxh62
fps=30
width=1280
height=720

[isp]
bayer=BGGR
fps=30
width=1280
height=720

[mipi]
input_mode=INPUT_MODE_CMOS
data_type=RAW_10BIT
width=1280
height=720

[vi_dev]
intf_mode=VI_MODE_DIGITAL_CAMERA
work_mode=VI_WORK_MODE_1Multiplex
mask_0=0xFFC00000
mask_1=0x0
scan_mode=VI_SCAN_PROGRESSIVE
data_seq=VI_DATA_SEQ_YVYU
vsync=VI_VSYNC_PULSE
vsync_neg=VI_VSYNC_NEG_HIGH
hsync=VI_HSYNC_VALID_SINGNAL
hsync_neg=VI_HSYNC_NEG_HIGH
vsync_valid=VI_VSYNC_VALID_SINGNAL
vsync_valid_neg=VI_VSYNC_VALID_NEG_HIGH
timing_blank_hsync_hfb=0
timing_blank_hsync_act=1280
timing_blank_hsync_hbb=0
timing_blank_vsync_vfb=0
timing_blank_vsync_vact=720
timing_blank_vsync_vbb=0
width=1280
height=720
data_rate=DATA_RATE_X1

[vi_pipe]
width=1280
height=720
pix_fmt=PIXEL_FORMAT_RGB_BAYER_10BPP
compress_mode=COMPRESS_MODE_NONE

[vi_chn]
width=1280
height=720
pix_fmt=PIXEL_FORMAT_YVU_SEMIPLANAR_420
```
