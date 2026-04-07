#!/usr/bin/env python3
import sys
import io
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from urllib.parse import quote

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

SMTP_SERVER     = "smtp.gmail.com"
SMTP_PORT       = 587
SENDER_EMAIL    = "attendance.system.project.vit@gmail.com"
SENDER_PASSWORD = "yenwaxckqbyfbgmo"

HTTPS_PORT = 8443


def send_face_registration_email(student_prn, student_name, student_email, server_ip):
    try:
        face_reg_link = (
            f"https://{server_ip}:{HTTPS_PORT}/face-register"
            f"?prn={quote(student_prn)}"
            f"&email={quote(student_email)}"
            f"&name={quote(student_name)}"
        )

        msg = MIMEMultipart("alternative")
        msg["From"]    = SENDER_EMAIL
        msg["To"]      = student_email
        msg["Subject"] = "Face Registration - Smart Attendance System"

        plain = (
            f"Hello {student_name},\n\n"
            "Please register your face for the Smart Attendance System.\n\n"
            f"Registration link:\n{face_reg_link}\n\n"
            "Instructions:\n"
            "  1. Open the link in your phone or laptop browser\n"
            "  2. Your browser will show a security warning once - tap Advanced then Proceed\n"
            "  3. Allow camera access when prompted\n"
            "  4. Look straight at the camera in good lighting\n"
            "  5. Click Capture and Register\n"
            "  6. Wait for the confirmation message\n\n"
            "You must register your face BEFORE you can mark attendance.\n\n"
            "- Smart Attendance System"
        )

        html = f"""<!DOCTYPE html>
<html lang="en">
<body style="margin:0;padding:0;background:#f4f6fa;font-family:Arial,sans-serif;">
<div style="max-width:560px;margin:30px auto;background:white;border-radius:12px;
            overflow:hidden;box-shadow:0 4px 16px rgba(0,0,0,0.1);">
  <div style="background:linear-gradient(135deg,#667eea,#764ba2);padding:28px 30px;text-align:center;">
    <h1 style="margin:0;color:white;font-size:22px;">Face Registration Required</h1>
    <p style="margin:8px 0 0;color:rgba(255,255,255,0.85);font-size:14px;">
      Smart Attendance System
    </p>
  </div>
  <div style="padding:28px 30px;">
    <p>Hello <strong>{student_name}</strong>,</p>
    <p>
      To mark attendance in the <strong>Smart Attendance System</strong>,
      you must first register your face. This is a one-time step.
    </p>
    <p style="background:#fff3cd;border-left:4px solid #ffc107;padding:10px 14px;
              border-radius:0 6px 6px 0;font-size:13px;">
      <strong>Important:</strong> Register your face BEFORE attending any session.
    </p>
    <p style="background:#e8f4fd;border-left:4px solid #3b82f6;padding:10px 14px;
              border-radius:0 6px 6px 0;font-size:13px;margin-top:10px;">
      <strong>Security Notice:</strong> The link uses HTTPS with a self-signed certificate.
      Your browser will show a warning once. Tap <em>Advanced</em> then <em>Proceed to site</em>
      (Chrome/Android) or <em>Show Details</em> then <em>visit this website</em> (Safari/iPhone).
      This is safe and expected.
    </p>
    <div style="background:#f0f4ff;border-left:4px solid #667eea;padding:14px 18px;
                border-radius:0 8px 8px 0;margin:20px 0;">
      <strong style="color:#667eea;">Your Details</strong><br>
      PRN: <code>{student_prn}</code><br>
      Name: {student_name}<br>
      Email: {student_email}
    </div>
    <h3 style="color:#333;margin-top:24px;">How to Register (takes 1 minute)</h3>
    <ol style="color:#444;line-height:1.9;font-size:14px;">
      <li>Tap the button below</li>
      <li>Accept the browser security warning once (tap Advanced then Proceed)</li>
      <li>Allow camera access when prompted</li>
      <li>Sit in good lighting, face the camera directly</li>
      <li>Tap <em>Capture and Register</em></li>
      <li>Wait for the green success message</li>
    </ol>
    <div style="text-align:center;margin:28px 0;">
      <a href="{face_reg_link}"
         style="display:inline-block;background:#f59e0b;color:white;padding:14px 32px;
                text-decoration:none;border-radius:8px;font-size:16px;font-weight:bold;">
        Register My Face
      </a>
    </div>
    <p style="font-size:12px;color:#888;border-top:1px solid #eee;padding-top:16px;">
      Button not working? Copy and paste:<br>
      <a href="{face_reg_link}" style="color:#667eea;word-break:break-all;">{face_reg_link}</a>
    </p>
    <p style="font-size:12px;color:#aaa;margin-top:12px;">
      Your face data is stored securely and used only for attendance verification.
    </p>
  </div>
</div>
</body>
</html>"""

        msg.attach(MIMEText(plain, "plain", "utf-8"))
        msg.attach(MIMEText(html, "html", "utf-8"))

        with smtplib.SMTP(SMTP_SERVER, SMTP_PORT) as server:
            server.ehlo()
            server.starttls()
            server.ehlo()
            server.login(SENDER_EMAIL, SENDER_PASSWORD)
            server.send_message(msg)

        print(f"[OK] Face reg email sent to {student_name} ({student_email})")
        return True

    except Exception as e:
        print(f"[FAIL] {student_email}: {e}")
        return False


def main():
    if len(sys.argv) < 5:
        print("Usage: python send_face_reg_email.py <prn> <name> <email> <server_ip>")
        sys.exit(1)
    prn       = sys.argv[1]
    name      = sys.argv[2]
    email     = sys.argv[3]
    server_ip = sys.argv[4]
    ok = send_face_registration_email(prn, name, email, server_ip)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()