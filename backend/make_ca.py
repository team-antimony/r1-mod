"""Generate a local CA + server certificate for the backend's HTTPS auth on 127.0.0.1:443.

    uv run --with cryptography python make_ca.py [<game_dir>]

Writes pki/ca.pem and pki/server.pem (cert + key, never committed). With <game_dir> (your unpacked QA game folder)
ca.pem is also copied over its cacert.pem, the client's trust store; the first original is kept as cacert.pem.orig.
"""
from pathlib import Path
import datetime
import shutil
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID

PKI = Path(__file__).resolve().parent / "pki"
PKI.mkdir(exist_ok=True)

now = datetime.datetime.now(datetime.timezone.utc)

ca_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
ca_name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "codol-local-ca")])
ca_cert = (
    x509.CertificateBuilder()
    .subject_name(ca_name).issuer_name(ca_name).public_key(ca_key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - datetime.timedelta(days=1))
    .not_valid_after(now + datetime.timedelta(days=3650))
    .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
    .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False),
                   critical=True)
    .sign(ca_key, hashes.SHA256())
)

srv_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
srv_cert = (
    x509.CertificateBuilder()
    .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "127.0.0.1")]))
    .issuer_name(ca_name).public_key(srv_key.public_key())
    .serial_number(x509.random_serial_number())
    .not_valid_before(now - datetime.timedelta(days=1))
    .not_valid_after(now + datetime.timedelta(days=3650))
    .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
    .add_extension(x509.SubjectAlternativeName([x509.IPAddress(
        __import__("ipaddress").IPv4Address("127.0.0.1"))]), critical=False)
    .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),
                   critical=False)
    .sign(ca_key, hashes.SHA256())
)

(PKI / "ca.pem").write_bytes(ca_cert.public_bytes(serialization.Encoding.PEM))
(PKI / "server.pem").write_bytes(
    srv_cert.public_bytes(serialization.Encoding.PEM)
    + srv_key.private_bytes(serialization.Encoding.PEM,
                            serialization.PrivateFormat.TraditionalOpenSSL,
                            serialization.NoEncryption()))
print("wrote", PKI / "ca.pem", "and", PKI / "server.pem")
if len(sys.argv) > 1:
    trust = Path(sys.argv[1]) / "cacert.pem"
    if trust.exists() and not trust.with_suffix(".pem.orig").exists():
        shutil.copyfile(trust, trust.with_suffix(".pem.orig"))
    shutil.copyfile(PKI / "ca.pem", trust)
    print("copied ca.pem over", trust)
