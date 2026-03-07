#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
Smart Attendance System
WiFi QR Generator + Email Sender
Sends attendance link with teacher IPv4 address
"""

import sys
import io

# ── Force UTF-8 stdout/stderr so Unicode chars don't crash on Windows cp1252 ──
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

import qrcode
import smtplib
import mysql.connector
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.mime.image import MIMEImage
import subprocess
import platform
import re
import socket

# ================================
# EMAIL CONFIGURATION
# ================================

SMTP_SERVER = "smtp.gmail.com"
SMTP_PORT = 587

SENDER_EMAIL = "attendance.system.project.vit@gmail.com"
SENDER_PASSWORD = "yenwaxckqbyfbgmo"

# ================================
# DATABASE CONFIG
# ================================

DB_CONFIG = {
    "host": "localhost",
    "user": "root",
    "password": "12345",
    "database": "attendance_db"
}

# ================================
# GET TEACHER IPV4 ADDRESS
# ================================

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        print(f"[OK] Server IPv4 detected: {ip}")
        return ip
    except Exception:
        print("[WARN] Could not detect IPv4. Using localhost.")
        return "localhost"


# ================================
# WIFI DETECTION
# ================================

def get_wifi_details():

    try:
        system = platform.system()

        # WINDOWS
        if system == "Windows":

            result = subprocess.run(
                ["netsh", "wlan", "show", "interfaces"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace"
            )

            output = result.stdout
            ssid_match = re.search(r"SSID\s*:\s*(.+)", output)

            if ssid_match:
                ssid = ssid_match.group(1).strip()

                pass_result = subprocess.run(
                    ["netsh", "wlan", "show", "profile", ssid, "key=clear"],
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    errors="replace"
                )

                pass_match = re.search(r"Key Content\s*:\s*(.+)", pass_result.stdout)

                if pass_match:
                    password = pass_match.group(1).strip()
                    return ssid, password

        # LINUX
        elif system == "Linux":

            result = subprocess.run(
                ["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"],
                capture_output=True, text=True
            )

            for line in result.stdout.split("\n"):
                if line.startswith("yes:"):
                    ssid = line.split(":")[1]

                    pass_result = subprocess.run(
                        ["nmcli", "-s", "-g",
                         "802-11-wireless-security.psk",
                         "connection", "show", ssid],
                        capture_output=True, text=True
                    )

                    return ssid, pass_result.stdout.strip()

        # MAC
        elif system == "Darwin":

            result = subprocess.run(
                ["/System/Library/PrivateFrameworks/Apple80211.framework"
                 "/Versions/Current/Resources/airport", "-I"],
                capture_output=True, text=True
            )

            ssid_match = re.search(r" SSID: (.+)", result.stdout)

            if ssid_match:
                ssid = ssid_match.group(1).strip()

                try:
                    pass_result = subprocess.run(
                        ["security", "find-generic-password",
                         "-D", "AirPort network password",
                         "-a", ssid, "-w"],
                        capture_output=True, text=True
                    )
                    return ssid, pass_result.stdout.strip()
                except Exception:
                    pass

    except Exception as e:
        print(f"[WARN] WiFi detection error: {e}")

    print("[WARN] Using fallback WiFi credentials")
    return "Attendance_WiFi", "Attendance@123"


# ================================
# GENERATE WIFI QR
# ================================

def generate_wifi_qr(ssid, password, session_id):

    wifi_string = f"WIFI:S:{ssid};T:WPA;P:{password};;"

    qr = qrcode.QRCode(
        version=1,
        error_correction=qrcode.constants.ERROR_CORRECT_H,
        box_size=10,
        border=4
    )

    qr.add_data(wifi_string)
    qr.make(fit=True)

    img = qr.make_image(fill_color="black", back_color="white")

    filename = f"wifi_qr_{session_id}.png"
    img.save(filename)

    print(f"[OK] QR generated: {filename}")
    return filename


# ================================
# SEND EMAIL
# ================================

def send_email_to_student(
        student_email,
        student_name,
        ssid,
        password,
        session_id,
        teacher_name,
        session_name,
        qr_file,
        server_ip
):

    try:
        attendance_link = f"http://{server_ip}:8080/student?session_id={session_id}"

        msg = MIMEMultipart("related")
        msg["From"]    = SENDER_EMAIL
        msg["To"]      = student_email
        msg["Subject"] = f"Attendance Session: {session_name}"

        html = f"""
        <html>
        <body style="font-family:Arial;padding:20px;background:#f4f4f4">
        <div style="background:white;padding:25px;border-radius:10px">

        <h2>Attendance Session</h2>

        <p>Hello <b>{student_name}</b>,</p>
        <p>A new session has started.</p>

        <p><b>Subject:</b> {session_name}</p>
        <p><b>Teacher:</b> {teacher_name}</p>
        <p><b>Session ID:</b> {session_id}</p>

        <hr>

        <h3>Connect to WiFi</h3>
        <p><b>Network:</b> {ssid}</p>
        <p><b>Password:</b> {password}</p>
        <p>Scan QR to connect:</p>
        <img src="cid:wifi_qr" width="220">

        <hr>

        <h3>Mark Attendance</h3>
        <p>
          <a href="{attendance_link}"
             style="background:#4CAF50;color:white;padding:12px 20px;
                    text-decoration:none;border-radius:5px">
            Mark Attendance
          </a>
        </p>
        <p>If the button does not work, open this link:</p>
        <p>{attendance_link}</p>

        </div>
        </body>
        </html>
        """

        msg.attach(MIMEText(html, "html", "utf-8"))

        with open(qr_file, "rb") as f:
            qr_img = MIMEImage(f.read())
            qr_img.add_header("Content-ID", "<wifi_qr>")
            qr_img.add_header("Content-Disposition", "inline",
                               filename=qr_file)
            msg.attach(qr_img)

        with smtplib.SMTP(SMTP_SERVER, SMTP_PORT) as server:
            server.starttls()
            server.login(SENDER_EMAIL, SENDER_PASSWORD)
            server.send_message(msg)

        print(f"[OK] Email sent -> {student_email}")
        return True

    except Exception as e:
        print(f"[FAIL] {student_email} : {e}")
        return False


# ================================
# MAIN
# ================================

def main(session_id):

    print("")
    print("==============================")
    print(" Smart Attendance Email Sender")
    print("==============================")
    print("")

    server_ip = get_local_ip()

    print("\nDetecting WiFi...")
    ssid, password = get_wifi_details()
    print(f"[OK] SSID     : {ssid}")
    print(f"[OK] Password : {password}")
    print("")

    conn   = mysql.connector.connect(**DB_CONFIG)
    cursor = conn.cursor(dictionary=True)

    cursor.execute(
        "UPDATE sessions SET wifi_ssid=%s, wifi_password=%s WHERE id=%s",
        (ssid, password, session_id)
    )
    conn.commit()

    cursor.execute(
        "SELECT teacher_name, session_name FROM sessions WHERE id=%s",
        (session_id,)
    )
    session = cursor.fetchone()

    if not session:
        print(f"[ERROR] Session {session_id} not found in database")
        cursor.close()
        conn.close()
        sys.exit(1)

    teacher_name = session["teacher_name"]
    session_name = session["session_name"]

    qr_file = generate_wifi_qr(ssid, password, session_id)

    cursor.execute("SELECT name, email FROM students")
    students = cursor.fetchall()

    if not students:
        print("[WARN] No students found in database")
        cursor.close()
        conn.close()
        sys.exit(0)

    print(f"Sending emails to {len(students)} students\n")

    success = 0
    for s in students:
        if send_email_to_student(
                s["email"], s["name"],
                ssid, password,
                session_id, teacher_name, session_name,
                qr_file, server_ip):
            success += 1

    print("")
    print("==============================")
    print(f"Emails Sent: {success}/{len(students)}")
    print("==============================")
    print("")

    cursor.close()
    conn.close()

    # Exit with error code if any email failed so C server can detect it
    if success < len(students):
        sys.exit(1)


# ================================
# RUN
# ================================

if __name__ == "__main__":

    if len(sys.argv) < 2:
        print("Usage: python send_emails.py <session_id>")
        sys.exit(1)

    main(sys.argv[1])