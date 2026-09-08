"""Self signed certificate used by the phone facing HTTPS server.

Browsers only grant camera access on secure origins, so the local server has to
speak TLS even on a LAN address.
"""

from __future__ import annotations

import datetime
import ipaddress
import os
import ssl
from collections.abc import Iterable
from pathlib import Path

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID


def data_dir() -> Path:
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~/.local/share")
    path = Path(base) / "OnlyCam"
    path.mkdir(parents=True, exist_ok=True)
    return path


def _san_addresses(cert: x509.Certificate) -> list[str]:
    try:
        extension = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName)
    except x509.ExtensionNotFound:
        return []
    return [str(value) for value in extension.value.get_values_for_type(x509.IPAddress)]


def ensure_certificate(addresses: Iterable[str]) -> tuple[Path, Path]:
    """Returns (cert_path, key_path), regenerating when an address is missing."""
    wanted = [address for address in addresses if _is_ip(address)]
    directory = data_dir()
    cert_path = directory / "server.crt"
    key_path = directory / "server.key"

    if cert_path.exists() and key_path.exists():
        try:
            cert = x509.load_pem_x509_certificate(cert_path.read_bytes())
            covered = set(_san_addresses(cert))
            not_expired = cert.not_valid_after_utc > datetime.datetime.now(datetime.timezone.utc)
            if not_expired and all(address in covered for address in wanted):
                return cert_path, key_path
        except Exception:
            pass

    _generate(cert_path, key_path, wanted)
    return cert_path, key_path


def _is_ip(value: str) -> bool:
    try:
        ipaddress.ip_address(value)
        return True
    except ValueError:
        return False


def _generate(cert_path: Path, key_path: Path, addresses: list[str]) -> None:
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    subject = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "OnlyCam"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "OnlyCam"),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    alternative_names: list[x509.GeneralName] = [x509.DNSName("localhost")]
    alternative_names += [x509.IPAddress(ipaddress.ip_address(address)) for address in addresses]

    certificate = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=825))
        .add_extension(x509.SubjectAlternativeName(alternative_names), critical=False)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .sign(key, hashes.SHA256())
    )

    key_path.write_bytes(
        key.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            encryption_algorithm=serialization.NoEncryption(),
        )
    )
    cert_path.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))


def ssl_context(addresses: Iterable[str]) -> ssl.SSLContext:
    cert_path, key_path = ensure_certificate(addresses)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(str(cert_path), str(key_path))
    return context
