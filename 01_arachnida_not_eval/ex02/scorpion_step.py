#!/usr/bin/env python3
import sys
import os
import datetime
import struct

def human_size(size):
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size < 1024:
            if unit == "B":
                return f"{size} {unit}"
            return f"{size:.2f} {unit}"
        size /= 1024
    return f"{size:.2f} PB"

def detect_format(data):
    if data[:3] == b"\xff\xd8\xff":
        return "JPEG"
    elif data[:8] == b"\x89PNG\r\n\x1a\n":
        return "PNG"
    elif data[:6] in (b"GIF87a", b"GIF89a"):
        return "GIF"
    elif data[:2] == b"BM":
        return "BMP"
    else:
        return "UNKNOWN"
    

def dimensions(fmt, data):
    try:
        if fmt == "PNG":
            w, h = struct.unpack(">II", data[16:24])
            return w, h
        if fmt == "GIF":
            w, h = struct.unpack("<HH", data[6:10])
            return w, h
        if fmt == "BMP":
            w, h = struct.unpack("<ii", data[18:26])
            return w, abs(h)
        if fmt == "JPEG":
            i = 2
            while i + 9 < len(data):
                if data[i] != 0xFF:
                    i += 1
                    continue
                marker = data[i + 1]
                # SOF markers carry the frame dimensions
                if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
                    h, w = struct.unpack(">HH", data[i + 5:i + 9])
                    return w, h
                if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
                    i += 2
                    continue
                seg_len = struct.unpack(">H", data[i + 2:i + 4])[0]
                i += 2 + seg_len
    except (struct.error, IndexError):
        pass
    return None

def report(path):
    print("=" * 60)
    print(path)
    print("=" * 60)
    try:
        st = os.stat(path)
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        print(f"  cannot read: {e}\n")
        return
    fmt = detect_format(data)
    print("  Basic attributes:")
    print(f"    Format        : {fmt}")
    print(f"    File size     : {human_size(st.st_size)}")
    dim = dimensions(fmt, data)
    if dim:
        print(f"    Dimensions    : {dim[0]} x {dim[1]} px")
    mtime = datetime.datetime.fromtimestamp(st.st_mtime)
    ctime = datetime.datetime.fromtimestamp(st.st_ctime)
    print(f"    Modified      : {mtime:%Y-%m-%d %H:%M:%S}")
    print(f"    Inode changed : {ctime:%Y-%m-%d %H:%M:%S}")

def main():
    if len(sys.argv) < 2:
        print("Usage: scorpion.py FILE1 [FILE2 ...]", file=sys.stderr)
        sys.exit(1)
    for path in sys.argv[1:]:
        report(path)



if __name__ == "__main__":
    main()