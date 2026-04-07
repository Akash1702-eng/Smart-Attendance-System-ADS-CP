#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
https_proxy.py  —  HTTPS Reverse Proxy for Smart Attendance System
===================================================================
WHY THIS EXISTS:
  Mobile browsers (Chrome/Safari on Android/iOS) block camera access
  on plain HTTP pages. This script wraps the C attendance server
  (http://localhost:8080) with HTTPS so camera works on student phones.

HOW TO USE:
  1. Start your C server first:  server.exe
  2. Then run this:              python https_proxy.py
  3. Share this link with students:
     https://<your-ip>:8443/face-register?prn=...&name=...&email=...

  Students will see ONE "Not secure" browser warning.
  They tap:  Advanced  →  Proceed to site  (or "Accept Risk")
  After that, camera works perfectly.

INSTALL (one-time, if cert generation fails):
  pip install cryptography
"""

import ssl
import socket
import http.client
import http.server
import os
import sys
import subprocess
import time

TARGET_HOST = "127.0.0.1"
TARGET_PORT = 8080       # Your C server port
PROXY_PORT  = 8443       # HTTPS port students will connect to
CERT_FILE   = "cert.pem"
KEY_FILE    = "key.pem"

# ─── Get local IP ─────────────────────────────────────────────────────────────

def get_local_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

# ─── Certificate generation  ──────────────────────────────────────────────────

def gen_cert_via_cryptography(ip):
    from cryptography import x509
    from cryptography.x509.oid import NameOID
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    import datetime
    import ipaddress

    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)

    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "SmartAttendance")])
    san  = x509.SubjectAlternativeName([
        x509.DNSName("localhost"),
        x509.IPAddress(ipaddress.IPv4Address("127.0.0.1")),
        x509.IPAddress(ipaddress.IPv4Address(ip)),
    ])
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.utcnow())
        .not_valid_after(datetime.datetime.utcnow() + datetime.timedelta(days=730))
        .add_extension(san, critical=False)
        .sign(key, hashes.SHA256())
    )

    with open(KEY_FILE, "wb") as f:
        f.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption()
        ))
    with open(CERT_FILE, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    return True


def gen_cert_via_pyopenssl(ip):
    from OpenSSL import crypto

    key = crypto.PKey()
    key.generate_key(crypto.TYPE_RSA, 2048)

    cert = crypto.X509()
    cert.get_subject().CN = "SmartAttendance"
    cert.set_serial_number(1)
    cert.gmtime_adj_notBefore(0)
    cert.gmtime_adj_notAfter(730 * 24 * 3600)
    cert.set_issuer(cert.get_subject())
    cert.set_pubkey(key)
    san = f"IP:{ip},IP:127.0.0.1,DNS:localhost".encode()
    cert.add_extensions([
        crypto.X509Extension(b"subjectAltName", False, san)
    ])
    cert.sign(key, "sha256")

    with open(CERT_FILE, "wb") as f:
        f.write(crypto.dump_certificate(crypto.FILETYPE_PEM, cert))
    with open(KEY_FILE, "wb") as f:
        f.write(crypto.dump_privatekey(crypto.FILETYPE_PEM, key))
    return True


def gen_cert_via_openssl_cli(ip):
    ext = f"subjectAltName=IP:{ip},IP:127.0.0.1,DNS:localhost"
    result = subprocess.run([
        "openssl", "req", "-x509", "-newkey", "rsa:2048",
        "-keyout", KEY_FILE, "-out", CERT_FILE,
        "-days", "730", "-nodes",
        "-subj", "/CN=SmartAttendance",
        "-addext", ext
    ], capture_output=True, timeout=30)
    return result.returncode == 0


def ensure_certificate():
    if os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE):
        print("[OK]  Using existing cert.pem / key.pem")
        return

    ip = get_local_ip()
    print(f"[..] Generating self-signed certificate for IP {ip} ...")

    methods = [
        ("cryptography library", lambda: gen_cert_via_cryptography(ip)),
        ("pyOpenSSL library",    lambda: gen_cert_via_pyopenssl(ip)),
        ("openssl CLI",          lambda: gen_cert_via_openssl_cli(ip)),
    ]

    for name, fn in methods:
        try:
            if fn():
                print(f"[OK]  Certificate generated using {name}")
                return
        except ImportError:
            pass
        except Exception as e:
            print(f"[WARN] {name} failed: {e}")

    print()
    print("=" * 56)
    print("  ERROR: Cannot generate SSL certificate.")
    print()
    print("  Fix: run ONE of these commands and try again:")
    print("    pip install cryptography")
    print("    pip install pyopenssl")
    print("=" * 56)
    sys.exit(1)


# ─── Proxy request handler  ───────────────────────────────────────────────────

class ProxyHandler(http.server.BaseHTTPRequestHandler):
    """Forwards every HTTPS request to the C server over plain HTTP."""

    # 8 MB limit — face images are ~60-100 KB after resize, so this is generous
    MAX_BODY_BYTES = 8 * 1024 * 1024

    def forward(self, method):
        try:
            # Read incoming body
            content_length = int(self.headers.get("Content-Length", 0))
            if content_length > self.MAX_BODY_BYTES:
                self.send_error(413, "Request body too large")
                return
            body = self.rfile.read(content_length) if content_length > 0 else None

            # Forward headers (strip hop-by-hop)
            skip = {"host", "connection", "keep-alive",
                    "proxy-connection", "upgrade", "te", "trailers"}
            fwd_headers = {k: v for k, v in self.headers.items()
                           if k.lower() not in skip}
            if body is not None:
                fwd_headers["Content-Length"] = str(len(body))

            # ── KEY FIX: pass the real student device IP to the C server ──────
            # Without this, all students appear to come from 127.0.0.1 (the
            # proxy itself) and the unique_device_session constraint blocks every
            # student after the first one.
            real_client_ip = self.client_address[0]
            fwd_headers["X-Forwarded-For"] = real_client_ip

            # Send to C server
            conn = http.client.HTTPConnection(
                TARGET_HOST, TARGET_PORT, timeout=60)
            conn.request(method, self.path, body=body, headers=fwd_headers)
            resp = conn.getresponse()
            data = resp.read()

            # Return response to browser
            self.send_response(resp.status)
            skip_resp = {"transfer-encoding", "connection", "keep-alive"}
            for k, v in resp.getheaders():
                if k.lower() not in skip_resp:
                    self.send_header(k, v)
            # Allow camera / fetch from HTTPS page
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "*")
            self.end_headers()
            self.wfile.write(data)
            conn.close()

        except ConnectionRefusedError:
            self._error(502,
                b'{"error":"C server not reachable. Is server.exe running on port 8080?"}')
        except TimeoutError:
            self._error(504, b'{"error":"C server timed out."}')
        except Exception as e:
            self._error(500, f'{{"error":"Proxy error: {e}"}}'.encode())

    def _error(self, code, msg):
        try:
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(msg)
        except Exception:
            pass

    def do_GET(self):     self.forward("GET")
    def do_POST(self):    self.forward("POST")
    def do_HEAD(self):    self.forward("HEAD")
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin",  "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,HEAD,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def log_message(self, fmt, *args):
        try:
            print(f"  [{args[1]}]  {self.command}  {self.path}")
        except Exception:
            pass


# ─── Main ─────────────────────────────────────────────────────────────────────

def wait_for_c_server(timeout=10):
    for _ in range(timeout):
        try:
            conn = http.client.HTTPConnection(TARGET_HOST, TARGET_PORT, timeout=1)
            conn.request("GET", "/")
            conn.getresponse()
            conn.close()
            return True
        except Exception:
            time.sleep(1)
    return False


def main():
    print()
    print("=" * 56)
    print("  Smart Attendance — HTTPS Proxy")
    print("=" * 56)

    ensure_certificate()

    print("[..] Checking if C server is running on port 8080 ...")
    if wait_for_c_server(timeout=3):
        print("[OK]  C server is running.")
    else:
        print("[WARN] C server NOT detected on port 8080.")
        print("       Start server.exe first, or requests will return 502.")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT_FILE, KEY_FILE)

    httpd = http.server.HTTPServer(("0.0.0.0", PROXY_PORT), ProxyHandler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    ip = get_local_ip()

    print()
    print("=" * 56)
    print(f"  HTTPS Proxy listening on port {PROXY_PORT}")
    print(f"  Forwarding  →  http://127.0.0.1:{TARGET_PORT}")
    print()
    print(f"  Teacher portal:   https://{ip}:{PROXY_PORT}/")
    print(f"  Student portal:   https://{ip}:{PROXY_PORT}/student")
    print(f"  Face-register:    https://{ip}:{PROXY_PORT}/face-register")
    print()
    print("  Students see a 'Not Secure' warning once.")
    print("  They tap:  Advanced  ->  Proceed  (Android/Chrome)")
    print("         or: Show Details -> visit website (iPhone/Safari)")
    print("  Camera works after that.")
    print()
    print("  Press Ctrl+C to stop.")
    print("=" * 56)
    print()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[STOPPED] HTTPS proxy shut down.")


if __name__ == "__main__":
    main()