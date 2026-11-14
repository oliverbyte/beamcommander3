#!/usr/bin/env python3
"""Writes a disk image to an SD card on macOS (run as root, via make_sdcard.sh).

    sudo python3 -I write_image.py <image.img> <diskN>

Plain `dd` fails on recent macOS with "Resource busy": while the card is
being overwritten, macOS (DiskArbitration + the FSKit FAT driver) remounts
its partitions. This writer:
- opens the raw device with an exclusive lock (like Raspberry Pi Imager),
- writes the partition table (first 4 MiB) last, so macOS only sees the new
  partitions once everything else is on the card,
- on EBUSY/EIO unmounts the card and retries the same chunk,
- prints the write speed, fsyncs, and verifies the card by reading back
  (everything else before the partition table is written, so macOS can't
  mount and modify the boot partition in between).
"""
import errno
import hashlib
import os
import subprocess
import sys
import time

CHUNK = 4 * 1024 * 1024
RETRIES = 15


def unmount(disk):
    subprocess.run(["diskutil", "unmountDisk", "force", f"/dev/{disk}"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def open_raw(disk, flags):
    raw = f"/dev/r{disk}"
    for attempt in range(RETRIES):
        try:
            return os.open(raw, flags | os.O_EXLOCK)
        except OSError as e:
            if e.errno not in (errno.EBUSY, errno.EAGAIN, errno.EWOULDBLOCK, errno.EIO, errno.ENXIO, errno.ENOENT):
                raise
            print(f"[write] {raw} busy ({e.strerror}) - unmounting, retry {attempt + 1}", flush=True)
            unmount(disk)
            time.sleep(1 + attempt)
    sys.exit(f"[write] could not open {raw}")


def read_chunk(img, i):
    img.seek(i * CHUNK)
    data = img.read(CHUNK)
    if len(data) % 512:  # raw devices need whole sectors
        data += b"\0" * (512 - len(data) % 512)
    return data


def write_chunks(fd, disk, img, indices, size, total_done=0):
    start = time.time()
    written = 0
    for n, i in enumerate(indices, 1):
        data = read_chunk(img, i)
        for attempt in range(RETRIES):
            try:
                os.lseek(fd, i * CHUNK, os.SEEK_SET)
                view = memoryview(data)
                while view:
                    view = view[os.write(fd, view):]
                break
            except OSError as e:
                if e.errno not in (errno.EBUSY, errno.EIO, errno.ENXIO):
                    raise
                print(f"\n[write] chunk {i}: {e.strerror} - unmounting, retry {attempt + 1}", flush=True)
                try:
                    os.close(fd)
                except OSError:
                    pass
                unmount(disk)
                time.sleep(1 + attempt)
                fd = open_raw(disk, os.O_RDWR)
        else:
            sys.exit(f"[write] chunk {i} failed {RETRIES} times - card or reader faulty?")
        written += len(data)
        if n % 16 == 0 or n == len(indices):
            secs = max(time.time() - start, 0.001)
            print(f"\r[write] {(total_done + written) >> 20} / {size >> 20} MiB  {written / secs / 1e6:.1f} MB/s",
                  end="", flush=True)
    os.fsync(fd)
    return fd


def verify_chunks(fd, img, indices, size):
    bad = []
    for n, i in enumerate(indices, 1):
        data = read_chunk(img, i)
        os.lseek(fd, i * CHUNK, os.SEEK_SET)
        got = b""
        while len(got) < len(data):
            part = os.read(fd, len(data) - len(got))
            if not part:
                break
            got += part
        if hashlib.sha1(got).digest() != hashlib.sha1(data).digest():
            bad.append(i)
        if n % 64 == 0 or n == len(indices):
            print(f"\r[verify] {n * CHUNK >> 20} / {size >> 20} MiB", end="", flush=True)
    print()
    if bad:
        sys.exit(f"[verify] {len(bad)} chunk(s) differ, at MiB {[i * 4 for i in bad][:20]} - card or reader faulty")


def main():
    image, disk = sys.argv[1], sys.argv[2]
    size = os.path.getsize(image)
    chunks = (size + CHUNK - 1) // CHUNK
    rest = list(range(1, chunks))
    t0 = time.time()

    unmount(disk)
    fd = open_raw(disk, os.O_RDWR)
    with open(image, "rb") as img:
        # 1. Everything but the partition table, verified while the card
        #    still has no (new) partitions macOS could mount and modify.
        fd = write_chunks(fd, disk, img, rest, size)
        print()
        verify_chunks(fd, img, rest, size)
        # 2. The partition table last - from here on macOS may mount the
        #    new boot partition (and write its own metadata to it).
        fd = write_chunks(fd, disk, img, [0], size, total_done=(chunks - 1) * CHUNK)
        print()
        verify_chunks(fd, img, [0], size)
        os.close(fd)
    print(f"[write] done in {time.time() - t0:.0f} s, verified OK")


if __name__ == "__main__":
    main()
