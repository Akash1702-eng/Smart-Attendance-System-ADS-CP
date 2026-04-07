#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
save_face_image.py  –  Decode a base64 face image and save it as a JPEG.
Called by server.c when a student registers their face.

Usage:
    python save_face_image.py <prn> <b64_file>

Saves image to: faces/<prn>.jpg
"""

import sys
import os
import base64
import io

def main():
    if len(sys.argv) < 3:
        print("Usage: python save_face_image.py <prn> <b64_file>")
        sys.exit(1)

    prn      = sys.argv[1]
    b64_file = sys.argv[2]

    # Read base64 string from temp file
    try:
        with open(b64_file, 'r') as f:
            b64_data = f.read().strip()
    except Exception as e:
        print(f"[FAIL] Cannot read b64 file: {e}")
        sys.exit(1)

    # Decode base64 to bytes
    try:
        img_bytes = base64.b64decode(b64_data)
    except Exception as e:
        print(f"[FAIL] Base64 decode failed: {e}")
        sys.exit(1)

    # Save to faces/ directory
    os.makedirs("faces", exist_ok=True)
    out_path = os.path.join("faces", f"{prn}.jpg")

    try:
        with open(out_path, 'wb') as f:
            f.write(img_bytes)
        print(f"[OK] Face image saved: {out_path} ({len(img_bytes)} bytes)")
        sys.exit(0)
    except Exception as e:
        print(f"[FAIL] Cannot save image: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()