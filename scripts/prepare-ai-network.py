#!/usr/bin/env python3
"""Verify/extract pinned AI network dependencies; download only on request."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEPENDENCIES = {
    "lwip": {
        "version": "2.2.1", "license": "BSD-3-Clause", "license_file": "COPYING",
        "archive": "lwip-STABLE-2_2_1_RELEASE.zip",
        "url": "https://codeload.github.com/lwip-tcpip/lwip/zip/refs/tags/STABLE-2_2_1_RELEASE",
        "sha256": "2fdef2de21a907f7d590e91d46ff041e2bd0407f0ab1e82c37d7d39d60cbfd78",
        "prefix": "lwip-STABLE-2_2_1_RELEASE",
        "version_file": "src/include/lwip/init.h",
        "version_macros": {"LWIP_VERSION_MAJOR": 2, "LWIP_VERSION_MINOR": 2,
                           "LWIP_VERSION_REVISION": 1},
    },
    "mbedtls": {
        "version": "4.1.1", "license": "Apache-2.0 OR GPL-2.0-or-later",
        "license_file": "LICENSE",
        "archive": "mbedtls-4.1.1.tar.bz2",
        "url": "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.1.1/mbedtls-4.1.1.tar.bz2",
        "sha256": "3359a349e23db3d5536fcee032ae7b2ecbfc08972fab643089b5cbf2a375c98c",
        "prefix": "mbedtls-4.1.1", "version_file": "include/mbedtls/build_info.h",
        "version_macros": {"MBEDTLS_VERSION_MAJOR": 4, "MBEDTLS_VERSION_MINOR": 1,
                           "MBEDTLS_VERSION_PATCH": 1},
    },
}


def destination(output, name, prefix):
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise ValueError(f"unsafe archive member: {name}")
    if relative.parts[0] != prefix:
        raise ValueError(f"unexpected archive root: {name}")
    result = output.joinpath(*relative.parts[1:]).resolve()
    if not result.is_relative_to(output.resolve()):
        raise ValueError(f"archive member escapes destination: {name}")
    return result


def extract(archive, output, prefix):
    output.mkdir(parents=True, exist_ok=True)
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as source:
            for member in source.infolist():
                target = destination(output, member.filename, prefix)
                if member.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                    continue
                if (member.external_attr >> 16) & 0o170000 == 0o120000:
                    raise ValueError(f"symbolic link is unsupported: {member.filename}")
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(member) as src, target.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
    else:
        with tarfile.open(archive, "r:*") as source:
            for member in source:
                target = destination(output, member.name, prefix)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                elif member.isfile():
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with source.extractfile(member) as src, target.open("wb") as dst:
                        shutil.copyfileobj(src, dst)
                elif member.issym() and member.name.startswith(
                        prefix + "/tf-psa-crypto/drivers/pqcp/mldsa-native/examples/"):
                    # These example-only Unix links include directory links.
                    # Keep the complete archive; the firmware library build
                    # does not use these examples. Do not require Windows admin.
                    continue
                else:
                    raise ValueError(f"non-regular archive member: {member.name}")


def prepare(output, cache, download, selected):
    output = output.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    manifest = {"schema": 1, "dependencies": {}}
    for name in selected:
        entry = DEPENDENCIES[name]
        archive = cache / entry["archive"]
        if not archive.exists():
            if not download:
                raise FileNotFoundError(f"Missing {archive}; rerun with --download or provide the pinned archive")
            with urllib.request.urlopen(entry["url"], timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != entry["sha256"]:
                raise ValueError(f"Downloaded {name} SHA256 mismatch")
            archive.write_bytes(data)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError(f"Cached {name} SHA256 mismatch: {archive}")
        target = output / name
        extract(archive, target, entry["prefix"])
        header = (target / entry["version_file"]).read_text(encoding="utf-8")
        for macro, value in entry["version_macros"].items():
            if not re.search(rf"#define\s+{macro}\s+{value}\b", header):
                raise ValueError(f"Unexpected {name} version: {macro}")
        license_path = target / entry["license_file"]
        if not license_path.is_file():
            raise ValueError(f"Missing license: {license_path}")
        if name == "mbedtls" and not (target / "tf-psa-crypto/core/psa_crypto.c").is_file():
            raise ValueError("Mbed TLS archive lacks TF-PSA-Crypto; use the official complete release")
        manifest["dependencies"][name] = {
            key: value for key, value in entry.items()
            if key in ("version", "license", "archive", "url", "sha256")
        }
        manifest["dependencies"][name].update(root=str(target), license_file=str(license_path))
        if name == "mbedtls":
            manifest["dependencies"][name]["omitted"] = "example-only symlinks under tf-psa-crypto/drivers/pqcp/mldsa-native/examples; complete release archive retained"
        print(f"Prepared {name} {entry['version']}: {target}")
    (output / "SOURCE.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cache", type=Path, default=ROOT / "downloads/ai-network")
    parser.add_argument("--download", action="store_true")
    parser.add_argument("--dependency", choices=tuple(DEPENDENCIES), action="append")
    args = parser.parse_args()
    prepare(args.output, args.cache, args.download, args.dependency or list(DEPENDENCIES))
