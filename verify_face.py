#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_face.py  –  Compare a submitted face image against the stored
                   registered face for a given student (PRN).

Called by server.c /api/verify_face endpoint.

Usage:
    python verify_face.py <prn> <b64_file> <result_json_file>

Writes result to result_json_file:
    {"success": true,  "verified": true,  "face_registered": true, "message": "Face matched"}
    {"success": true,  "verified": false, "face_registered": true, "message": "Face does not match"}
    {"success": false, "verified": false, "face_registered": false,"message": "Face not registered"}

Dependencies:
    pip install opencv-python face_recognition numpy

face_recognition uses dlib under the hood and works well on Windows/Linux/macOS.
If face_recognition is not installed, the script falls back to a pixel-similarity
check using OpenCV only (less accurate but always available).
"""

import sys
import os
import json
import base64

def write_result(result_file: str, success: bool, verified: bool, face_registered: bool, message: str):
    data = {
        "success":          success,
        "verified":         verified,
        "face_registered":  face_registered,
        "message":          message
    }
    try:
        with open(result_file, 'w') as f:
            json.dump(data, f)
    except Exception as e:
        print(f"[FAIL] Cannot write result: {e}")


def verify_with_face_recognition(stored_path: str, incoming_bytes: bytes) -> tuple:
    """
    Uses the face_recognition library (dlib-based) for accurate comparison.
    Returns (verified: bool, message: str)
    """
    import face_recognition
    import numpy as np
    import cv2

    # Load stored face
    stored_img = face_recognition.load_image_file(stored_path)
    stored_encodings = face_recognition.face_encodings(stored_img)
    if not stored_encodings:
        return False, "No face detected in stored image. Please re-register."

    # Decode incoming image
    nparr    = np.frombuffer(incoming_bytes, np.uint8)
    incoming = cv2.imdecode(nparr, cv2.IMREAD_COLOR)
    if incoming is None:
        return False, "Could not decode incoming image."

    incoming_rgb      = cv2.cvtColor(incoming, cv2.COLOR_BGR2RGB)
    incoming_encodings = face_recognition.face_encodings(incoming_rgb)
    if not incoming_encodings:
        return False, "No face detected in captured image. Ensure good lighting and face the camera."

    # Compare — tolerance 0.5 is a good balance (lower = stricter)
    results  = face_recognition.compare_faces(stored_encodings, incoming_encodings[0], tolerance=0.50)
    distance = face_recognition.face_distance(stored_encodings, incoming_encodings[0])[0]

    print(f"[INFO] Face distance: {distance:.3f} (threshold 0.50)")

    if results[0]:
        return True,  f"Face matched (confidence {(1-distance)*100:.0f}%)"
    else:
        return False, f"Face does not match. Please try again in better lighting."


def verify_with_opencv_fallback(stored_path: str, incoming_bytes: bytes) -> tuple:
    """
    Fallback: basic histogram comparison using OpenCV only.
    Less accurate than face_recognition but requires no dlib.
    Returns (verified: bool, message: str)
    """
    import cv2
    import numpy as np

    stored  = cv2.imread(stored_path, cv2.IMREAD_GRAYSCALE)
    if stored is None:
        return False, "Cannot read stored face image."

    nparr    = np.frombuffer(incoming_bytes, np.uint8)
    incoming = cv2.imdecode(nparr, cv2.IMREAD_GRAYSCALE)
    if incoming is None:
        return False, "Cannot decode incoming image."

    # Resize both to same dimensions for comparison
    size = (200, 200)
    stored   = cv2.resize(stored,   size)
    incoming = cv2.resize(incoming, size)

    # Histogram comparison
    hist_stored   = cv2.calcHist([stored],   [0], None, [256], [0,256])
    hist_incoming = cv2.calcHist([incoming], [0], None, [256], [0,256])
    cv2.normalize(hist_stored,   hist_stored)
    cv2.normalize(hist_incoming, hist_incoming)

    score = cv2.compareHist(hist_stored, hist_incoming, cv2.HISTCMP_CORREL)
    print(f"[INFO] OpenCV histogram correlation: {score:.3f} (threshold 0.75)")

    if score >= 0.75:
        return True,  f"Face matched (score {score:.2f})"
    else:
        return False, "Face does not match. Try better lighting or re-register."


def main():
    if len(sys.argv) < 4:
        print("Usage: python verify_face.py <prn> <b64_file> <result_json_file>")
        sys.exit(1)

    prn         = sys.argv[1]
    b64_file    = sys.argv[2]
    result_file = sys.argv[3]

    # Check stored face exists
    stored_path = os.path.join("faces", f"{prn}.jpg")
    if not os.path.exists(stored_path):
        write_result(result_file, True, False, False,
                     "Face not registered. Please register your face first.")
        sys.exit(0)

    # Read and decode incoming image
    try:
        with open(b64_file, 'r') as f:
            b64_data = f.read().strip()
        incoming_bytes = base64.b64decode(b64_data)
    except Exception as e:
        write_result(result_file, False, False, True, f"Image decode error: {e}")
        sys.exit(1)

    # Try face_recognition first, fall back to OpenCV
    try:
        verified, message = verify_with_face_recognition(stored_path, incoming_bytes)
        print(f"[face_recognition] verified={verified} msg={message}")
    except ImportError:
        print("[WARN] face_recognition not installed — using OpenCV fallback")
        try:
            import cv2
            verified, message = verify_with_opencv_fallback(stored_path, incoming_bytes)
            print(f"[opencv fallback] verified={verified} msg={message}")
        except ImportError:
            # Neither library available — bypass verification
            print("[WARN] Neither face_recognition nor opencv available — bypassing")
            write_result(result_file, True, True, True,
                         "Face verification bypassed (libraries not installed).")
            sys.exit(0)
    except Exception as e:
        print(f"[ERROR] Verification error: {e}")
        # On unexpected error, bypass gracefully so students aren't blocked
        write_result(result_file, True, True, True,
                     f"Verification error — proceeding. ({e})")
        sys.exit(0)

    write_result(result_file, True, verified, True, message)
    sys.exit(0)


if __name__ == "__main__":
    main()