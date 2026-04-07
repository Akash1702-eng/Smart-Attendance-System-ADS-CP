#!/usr/bin/env python3
import sys
import io
import os
import re
import socket
import platform
import subprocess
import smtplib
import time
import mysql.connector
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from email.mime.image import MIMEImage
import qrcode

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

SMTP_SERVER     = "smtp.gmail.com"
SMTP_PORT       = 587
SENDER_EMAIL    = "attendance.system.project.vit@gmail.com"
SENDER_PASSWORD = "yenwaxckqbyfbgmo"

DB_CONFIG = {
    "host":     "localhost",
    "user":     "root",
    "password": "12345",
    "database": "attendance_db",
}

HTTPS_PORT = 8443


def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        print(f"[OK] Server IPv4: {ip}")
        return ip
    except Exception:
        print("[WARN] Could not detect IPv4. Using localhost.")
        return "localhost"


def get_wifi_details():
    try:
        system = platform.system()
        if system == "Windows":
            result = subprocess.run(
                ["netsh", "wlan", "show", "interfaces"],
                capture_output=True, text=True, encoding="utf-8", errors="replace",
            )
            ssid_match = re.search(r"^\s+SSID\s*:\s*(.+)$", result.stdout, re.MULTILINE)
            if ssid_match:
                ssid = ssid_match.group(1).strip()
                pass_result = subprocess.run(
                    ["netsh", "wlan", "show", "profile", ssid, "key=clear"],
                    capture_output=True, text=True, encoding="utf-8", errors="replace",
                )
                pass_match = re.search(r"Key Content\s*:\s*(.+)", pass_result.stdout)
                return ssid, (pass_match.group(1).strip() if pass_match else "")
        elif system == "Linux":
            result = subprocess.run(
                ["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"],
                capture_output=True, text=True,
            )
            for line in result.stdout.splitlines():
                if line.startswith("yes:"):
                    ssid = line.split(":", 1)[1]
                    pass_result = subprocess.run(
                        ["nmcli", "-s", "-g", "802-11-wireless-security.psk",
                         "connection", "show", ssid],
                        capture_output=True, text=True,
                    )
                    return ssid, pass_result.stdout.strip()
        elif system == "Darwin":
            result = subprocess.run(
                ["/System/Library/PrivateFrameworks/Apple80211.framework"
                 "/Versions/Current/Resources/airport", "-I"],
                capture_output=True, text=True,
            )
            ssid_match = re.search(r"SSID: (.+)", result.stdout)
            if ssid_match:
                ssid = ssid_match.group(1).strip()
                try:
                    pass_result = subprocess.run(
                        ["security", "find-generic-password",
                         "-D", "AirPort network password", "-a", ssid, "-w"],
                        capture_output=True, text=True,
                    )
                    return ssid, pass_result.stdout.strip()
                except Exception:
                    return ssid, ""
    except Exception as e:
        print(f"[WARN] WiFi detection error: {e}")

    print("[WARN] Using fallback WiFi credentials")
    return "Attendance_WiFi", "Attendance@123"


def generate_wifi_qr(ssid, password, session_id):
    wifi_string = f"WIFI:S:{ssid};T:WPA;P:{password};;" if password else f"WIFI:S:{ssid};T:nopass;;"
    qr = qrcode.QRCode(version=1, error_correction=qrcode.constants.ERROR_CORRECT_H, box_size=10, border=4)
    qr.add_data(wifi_string)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white")
    filename = f"wifi_qr_{session_id}.png"
    img.save(filename)
    print(f"[OK] QR generated: {filename}")
    return filename


def send_attendance_email(student_email, student_name, ssid, password, session_id,
                          teacher_name, session_name, session_date, qr_file, server_ip):
    try:
        attendance_link = f"https://{server_ip}:{HTTPS_PORT}/student?session_id={session_id}"
        date_display    = session_date if session_date else "Today"
        password_row    = (
            f"<p><b>Password:</b> {password}</p>" if password
            else "<p><b>Network:</b> Open (no password needed)</p>"
        )

        msg = MIMEMultipart("related")
        msg["From"]    = SENDER_EMAIL
        msg["To"]      = student_email
        msg["Subject"] = f"Attendance Session: {session_name}"

        html = f"""
        <html>
        <body style="font-family:Arial;padding:20px;background:#f4f4f4">
        <div style="background:white;padding:25px;border-radius:10px;max-width:600px;margin:auto;">
          <h2 style="color:#1e3c72;">Attendance Session Notification</h2>
          <p>Hello <b>{student_name}</b>,</p>
          <p>A new attendance session has started. Please mark your attendance promptly.</p>
          <table style="width:100%;border-collapse:collapse;margin-bottom:16px;">
            <tr><td style="padding:6px;font-weight:bold;color:#4a6cf7;width:40%;">Subject</td>
                <td style="padding:6px;">{session_name}</td></tr>
            <tr><td style="padding:6px;font-weight:bold;color:#4a6cf7;">Teacher</td>
                <td style="padding:6px;">{teacher_name}</td></tr>
            <tr><td style="padding:6px;font-weight:bold;color:#4a6cf7;">Date</td>
                <td style="padding:6px;">{date_display}</td></tr>
            <tr><td style="padding:6px;font-weight:bold;color:#4a6cf7;">Session ID</td>
                <td style="padding:6px;">{session_id}</td></tr>
          </table>
          <hr style="border:1px solid #eee;">
          <h3 style="color:#1e3c72;">Connect to Classroom Wi-Fi</h3>
          <p><b>Network:</b> {ssid}</p>
          {password_row}
          <p>Scan the QR code to connect instantly:</p>
          <img src="cid:wifi_qr" width="200" style="border-radius:8px;">
          <hr style="border:1px solid #eee;">
          <h3 style="color:#1e3c72;">Mark Your Attendance</h3>
          <p style="font-size:13px;color:#555;">
            You will need to verify your face before attendance is marked.
            Make sure your face is already registered in the system.
          </p>
          <p>
            <a href="{attendance_link}"
               style="background:#4CAF50;color:white;padding:12px 24px;
                      text-decoration:none;border-radius:6px;font-weight:bold;">
              Mark Attendance
            </a>
          </p>
          <p style="font-size:12px;color:#888;margin-top:16px;">
            Button not working? Copy this link:<br>
            <b>Attendance:</b> {attendance_link}
          </p>
          <p style="font-size:11px;color:#aaa;">
            Note: The link uses HTTPS. Your browser may show a security warning once.
            Tap Advanced and then Proceed to continue — this is safe and expected.
          </p>
        </div>
        </body>
        </html>
        """

        msg.attach(MIMEText(html, "html", "utf-8"))

        with open(qr_file, "rb") as f:
            qr_img = MIMEImage(f.read())
            qr_img.add_header("Content-ID", "<wifi_qr>")
            qr_img.add_header("Content-Disposition", "inline", filename=qr_file)
            msg.attach(qr_img)

        with smtplib.SMTP(SMTP_SERVER, SMTP_PORT) as server:
            server.ehlo()
            server.starttls()
            server.ehlo()
            server.login(SENDER_EMAIL, SENDER_PASSWORD)
            server.send_message(msg)

        print(f"[OK] Attendance email -> {student_email}")
        return True

    except Exception as e:
        print(f"[FAIL] {student_email}: {e}")
        return False


def main(session_id):
    print()
    print("=" * 40)
    print(" Smart Attendance - Attendance Email Sender")
    print("=" * 40)

    server_ip = get_local_ip()

    print("Detecting Wi-Fi ...")
    ssid, password = get_wifi_details()
    print(f"[OK] SSID     : {ssid}")
    print(f"[OK] Password : {'(none)' if not password else password}")

    conn = mysql.connector.connect(**DB_CONFIG)
    try:
        cursor = conn.cursor(dictionary=True, buffered=True)

        cursor.execute(
            "UPDATE sessions SET wifi_ssid=%s, wifi_password=%s WHERE id=%s",
            (ssid, password, session_id),
        )
        conn.commit()

        cursor.execute(
            "SELECT teacher_name, session_name, session_date FROM sessions WHERE id=%s",
            (session_id,),
        )
        session = cursor.fetchone()

        if not session:
            print(f"[ERROR] Session {session_id} not found")
            sys.exit(1)

        teacher_name = session["teacher_name"] or ""
        session_name = session["session_name"] or ""
        session_date = str(session["session_date"]) if session["session_date"] else ""

        qr_file = generate_wifi_qr(ssid, password, session_id)

        cursor.execute("SELECT name, email FROM students")
        students = cursor.fetchall()

    finally:
        cursor.close()
        conn.close()

    if not students:
        print("[WARN] No students found")
        try:
            os.remove(qr_file)
        except Exception:
            pass
        sys.exit(0)

    print(f"Sending attendance emails to {len(students)} student(s)...\n")

    success = 0
    try:
        for s in students:
            ok = send_attendance_email(
                s["email"], s["name"],
                ssid, password,
                session_id, teacher_name, session_name, session_date,
                qr_file, server_ip,
            )
            if ok:
                success += 1
            time.sleep(0.15)
    finally:
        try:
            os.remove(qr_file)
            print(f"[CLEANUP] Removed {qr_file}")
        except Exception:
            pass

    print()
    print("=" * 40)
    print(f"Emails sent: {success}/{len(students)}")
    print("=" * 40)

    sys.exit(0 if success == len(students) else 1)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python send_emails.py <session_id>")
        sys.exit(1)
    main(int(sys.argv[1]))