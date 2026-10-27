import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CAM_IP = "192.168.1.108"
CAM_TCP_PORT = 10002
CAM_DISC_PORT = 8600
CAM_UDP_PORT = 13140
LOCAL_HTTP_PORT = 8888

clients = []
clients_lock = threading.Lock()
last_keyframe_item = None  # (ftype, pts_ms, frame)
MAGIC = b'\x55\xaa\x15\xa8'


def send_udp_cmd(udp, my_ip_str, cmd):
    pkt = bytearray(216)
    ip_ascii = my_ip_str.encode('ascii') + b'\x00'
    pkt[0:len(ip_ascii)] = ip_ascii
    struct.pack_into("<I", pkt, 200, cmd)
    udp.sendto(pkt, (CAM_IP, CAM_UDP_PORT))


def split_annexb_nals(frame_bytes):
    """Разбивает Annex-B кадр (00 00 00 01 ...) на отдельные NAL-единицы"""
    parts = frame_bytes.split(b'\x00\x00\x00\x01')
    return [p for p in parts if p]


def make_flv_tag(tag_type, timestamp_ms, body):
    """Создает один FLV Tag + PreviousTagSize (4 байта)"""
    data_size = len(body)
    ts_low = timestamp_ms & 0xFFFFFF
    ts_ext = (timestamp_ms >> 24) & 0xFF
    header = struct.pack(
        ">BBBBBBBBBBB",
        tag_type,
        (data_size >> 16) & 0xFF, (data_size >> 8) & 0xFF, data_size & 0xFF,
        (ts_low >> 16) & 0xFF, (ts_low >> 8) & 0xFF, ts_low & 0xFF,
        ts_ext,
        0, 0, 0  # StreamID = 0
    )
    prev_size = struct.pack(">I", 11 + data_size)
    return header + body + prev_size


def make_flv_avc_seq_header(sps, pps):
    """Формирует стартовый тег конфигурации декодера H.264 для контейнера FLV"""
    avcc = bytearray([
        0x01, sps[1], sps[2], sps[3], 0xFF,
        0xE1, (len(sps) >> 8) & 0xFF, len(sps) & 0xFF
    ]) + sps + bytearray([
        0x01, (len(pps) >> 8) & 0xFF, len(pps) & 0xFF
    ]) + pps
    body = b'\x17\x00\x00\x00\x00' + bytes(avcc)
    return make_flv_tag(9, 0, body)


def make_flv_video_frame(is_keyframe, timestamp_ms, nals):
    """Упаковывает H.264 кадр в FLV Video Tag"""
    avc_payload = bytearray()
    sps, pps = None, None
    for nal in nals:
        nal_type = nal[0] & 0x1F
        if nal_type == 7:
            sps = nal
        elif nal_type == 8:
            pps = nal
        elif nal_type in (1, 5, 6):
            avc_payload += struct.pack(">I", len(nal)) + nal

    out = bytearray()
    if sps and pps and timestamp_ms == 0:
        out += make_flv_avc_seq_header(sps, pps)

    if avc_payload:
        prefix = b'\x17\x01\x00\x00\x00' if is_keyframe else b'\x27\x01\x00\x00\x00'
        out += make_flv_tag(9, timestamp_ms, prefix + bytes(avc_payload))
    return bytes(out), sps, pps


def camera_reader_thread():
    global last_keyframe_item
    while True:
        tcp = None
        udp = None
        try:
            print(f"[CAM] Подключаемся к {CAM_IP}:{CAM_TCP_PORT}...")
            tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            tcp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            tcp.settimeout(5.0)
            tcp.connect((CAM_IP, CAM_TCP_PORT))

            my_ip = tcp.getsockname()[0]
            udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            udp.sendto(b'\x01\x00\x00\x00', (CAM_IP, CAM_DISC_PORT))
            time.sleep(0.1)
            send_udp_cmd(udp, my_ip, 15)

            print("[CAM] Поток запущен! Готов к раздаче в PotPlayer / VLC / ffplay.")
            buf = bytearray()

            while True:
                chunk = tcp.recv(32768)
                if not chunk:
                    raise ConnectionError("Камера закрыла соединение")
                buf.extend(chunk)

                while len(buf) >= 32:
                    if buf[:4] != MAGIC:
                        idx = buf.find(MAGIC)
                        if idx == -1:
                            del buf[:-3]
                            break
                        else:
                            del buf[:idx]
                            if len(buf) < 32:
                                break

                    frame_type_flag = struct.unpack_from("<I", buf, 4)[0]  # 0 = I-frame, 1 = P-frame
                    pts_ms = struct.unpack_from("<Q", buf, 8)[0]
                    frame_len = struct.unpack_from("<I", buf, 16)[0]

                    if frame_len > 500000:
                        del buf[:4]
                        continue

                    if len(buf) < 32 + frame_len:
                        break

                    frame = bytes(buf[32:32 + frame_len])
                    del buf[:32 + frame_len]

                    if frame_type_flag in (0, 1):
                        item = (frame_type_flag, pts_ms, frame)
                        if frame_type_flag == 0:
                            last_keyframe_item = item

                        with clients_lock:
                            for q in list(clients):
                                if len(q) < 90:
                                    q.append(item)

        except Exception as e:
            print(f"[CAM] Ошибка: {e}. Переподключение...")
            time.sleep(1)
        finally:
            if tcp:
                try:
                    tcp.close()
                except Exception:
                    pass
            if udp:
                try:
                    udp.close()
                except Exception:
                    pass


class StreamHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        use_flv = not self.path.endswith(".h264")
        print(f"[HTTP] Плеер подключился: {self.client_address} ({self.path}) [{'FLV' if use_flv else 'RAW H264'}]")

        self.send_response(200)
        self.send_header("Content-Type", "video/x-flv" if use_flv else "video/h264")
        self.send_header("Cache-Control", "no-cache, no-store")
        self.send_header("Connection", "close")
        self.end_headers()

        my_queue = []
        if last_keyframe_item:
            my_queue.append(last_keyframe_item)

        with clients_lock:
            clients.append(my_queue)

        try:
            started = False
            base_pts = None
            sent_seq_hdr = False

            if use_flv:
                # Заголовок контейнера FLV (9 байт + PreviousTagSize0 = 4 байта)
                self.wfile.write(b'FLV\x01\x01\x00\x00\x00\x09\x00\x00\x00\x00')

            while True:
                if my_queue:
                    ftype, pts_ms, frame = my_queue.pop(0)
                    if not started:
                        if ftype != 0:
                            continue
                        started = True
                        base_pts = pts_ms

                    if not use_flv:
                        self.wfile.write(frame)
                        self.wfile.flush()
                    else:
                        rel_ts = max(0, int(pts_ms - base_pts))
                        nals = split_annexb_nals(frame)
                        if not sent_seq_hdr:
                            sps = next((n for n in nals if (n[0] & 0x1F) == 7), None)
                            pps = next((n for n in nals if (n[0] & 0x1F) == 8), None)
                            if sps and pps:
                                self.wfile.write(make_flv_avc_seq_header(sps, pps))
                                sent_seq_hdr = True
                        flv_bytes, _, _ = make_flv_video_frame(ftype == 0, rel_ts, nals)
                        if flv_bytes:
                            self.wfile.write(flv_bytes)
                            self.wfile.flush()
                else:
                    time.sleep(0.002)
        except Exception:
            print(f"[HTTP] Плеер отключился: {self.client_address}")
        finally:
            with clients_lock:
                if my_queue in clients:
                    clients.remove(my_queue)

    def log_message(self, format, *args):
        pass


if __name__ == "__main__":
    t = threading.Thread(target=camera_reader_thread, daemon=True)
    t.start()

    server = ThreadingHTTPServer(("0.0.0.0", LOCAL_HTTP_PORT), StreamHandler)
    print("=================================================================")
    print("Универсальный сервер запущен!")
    print(f"1. Для PotPlayer и VLC (рекомендуется!): http://127.0.0.1:{LOCAL_HTTP_PORT}/live.flv")
    print(f"2. Сырой H.264 (для ffplay):             http://127.0.0.1:{LOCAL_HTTP_PORT}/live.h264")
    print("=================================================================")
    server.serve_forever()
