"""Build pinned TLS, test only localhost, and report retained MIPS sections.

No cloud credentials or public network access. The certificate fixtures and
private keys are upstream TEST DATA, never production trust roots.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import socket
import ssl
import subprocess
import threading

ROOT = Path(__file__).resolve().parents[4]
TLS = ROOT / "projects/halley7m-ai/tls"


def run(command, **kwargs):
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def loopback(executable, fixtures, version, certificate, key, ca, hostname="localhost",
             expected=0, flags=0, utc=1792713600):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = version
    context.maximum_version = version
    context.set_ciphers("ECDHE+AESGCM")
    context.load_cert_chain(fixtures / certificate, fixtures / key)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    listener.settimeout(15)
    port = listener.getsockname()[1]
    outcome = []

    def serve():
        try:
            connection, _ = listener.accept()
            connection.settimeout(10)
            with context.wrap_socket(connection, server_side=True) as stream:
                request = b""
                while b"\r\n\r\n" not in request:
                    data = stream.recv(4096)
                    if not data:
                        raise RuntimeError("client truncated the request")
                    request += data
                if not request.startswith(b"GET / HTTP/1.1\r\n"):
                    raise RuntimeError("unexpected loopback request")
                stream.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 15\r\n\r\ntls-loopback-ok\n")
                stream.unwrap().close()
                outcome.append("ok")
        except ssl.SSLError as error:
            outcome.append(error)
        except Exception as error:
            outcome.append(error)
        finally:
            listener.close()

    worker = threading.Thread(target=serve, daemon=True)
    worker.start()
    try:
        run([executable, fixtures / ca, port, hostname, utc, expected, flags], timeout=20)
    finally:
        worker.join(timeout=12)
        listener.close()
    if worker.is_alive() or not outcome:
        raise RuntimeError("loopback server did not finish")
    if expected == 0 and outcome != ["ok"]:
        raise RuntimeError(f"loopback server: {outcome}")
    if expected != 0 and not isinstance(outcome[0], (ssl.SSLError, ConnectionResetError)):
        raise RuntimeError(f"expected certificate alert: {outcome}")
    print(f"PASS {version.name}: {certificate}, host={hostname}, CA={ca}, expected={expected}")


def host(dependencies):
    if os.name != "nt":
        raise RuntimeError("Current host harness requires Windows BCrypt and Winsock")
    spec = importlib.util.spec_from_file_location("test_support", ROOT / "scripts/test-player-amp.py")
    support = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(support)
    env = support.msvc_environment()
    env["VSLANG"] = "1033"
    output = ROOT / "build/ai-tls-host"
    run(["cmake", "-S", TLS, "-B", output, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         "-DCMAKE_TRY_COMPILE_CONFIGURATION=Release", "-DCMAKE_C_FLAGS_DEBUG=/Od /Z7",
         "-DAI_TLS_BUILD_HOST_TESTS=ON", f"-DAI_MBEDTLS_SOURCE={dependencies / 'mbedtls'}"], env=env)
    with (output / "build.log").open("w", encoding="utf-8") as log:
        built = subprocess.run(["cmake", "--build", str(output), "--target", "ai_tls_host_test", "-j", "8"],
                               env=env, stdout=log, stderr=subprocess.STDOUT)
    if built.returncode:
        print("\n".join((output / "build.log").read_text("utf-8", errors="replace").splitlines()[-40:]))
        raise RuntimeError("TLS host build failed")
    executable = output / "ai_tls_host_test.exe"
    fixtures = dependencies / "mbedtls/framework/data_files"
    run([executable, fixtures / "test-ca-sha256.crt"])
    for version in (ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_3):
        loopback(executable, fixtures, version, "server2-sha256.crt", "server2.key", "test-ca-sha256.crt")
        loopback(executable, fixtures, version, "server5.crt", "server5.key", "test-ca2.crt")
    loopback(executable, fixtures, ssl.TLSVersion.TLSv1_3,
             "server5.crt", "server5.key", "test-ca2.crt", hostname="wrong.example", expected=-5, flags=4)
    loopback(executable, fixtures, ssl.TLSVersion.TLSv1_3,
             "server5.crt", "server5.key", "test-ca-sha256.crt", expected=-5, flags=8)
    loopback(executable, fixtures, ssl.TLSVersion.TLSv1_3,
             "server5-expired.crt", "server5.key", "test-ca2.crt", expected=-5, flags=1)
    loopback(executable, fixtures, ssl.TLSVersion.TLSv1_3,
             "server5-future.crt", "server5.key", "test-ca2.crt", expected=-5, flags=512)
    print("Host TLS PASS: readiness + TLS1.2/1.3 RSA/ECDSA + certificate rejection + fragmented BIO")


def mips(dependencies):
    compiler = next((ROOT / "toolchains/xburst2-windows").rglob("*gcc.exe"))
    prefix = str(compiler)[:-len("gcc.exe")]
    output = ROOT / "build/ai-tls-mips"
    run(["cmake", "-S", TLS, "-B", output, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=MinSizeRel",
         f"-DCMAKE_TOOLCHAIN_FILE={ROOT / 'cmake/xburst2-elf.cmake'}", f"-DCROSS_COMPILE={prefix}",
         f"-DAI_MBEDTLS_SOURCE={dependencies / 'mbedtls'}"])
    run(["cmake", "--build", output, "--target", "ai_tls", "-j", "8"])
    entrypoints = ["platform_init", "platform_shutdown", "random", "create", "handshake", "read",
                   "write", "close_notify", "destroy", "last_error", "verify_flags"]
    archive_paths = [output / "libai_tls.a", output / "mbedtls/library/libmbedtls.a",
                    output / "mbedtls/library/libmbedx509.a",
                    output / "mbedtls/tf-psa-crypto/core/libtfpsacrypto.a"]
    retained = output / "ai-tls-retained.o"
    command = [prefix + "ld.exe", "-r", "--gc-sections", "-EL"]
    for symbol in entrypoints:
        command += ["-u", "ai_tls_" + symbol]
    run([*command, "--start-group", *archive_paths, "--end-group", "-o", retained])
    report = subprocess.check_output([prefix + "size.exe", str(retained)], text=True)
    (output / "retained-size.txt").write_text(report, encoding="utf-8")
    print(report)
    print("MIPS compile/link PASS; retained TLS sections exclude libc/libgcc, CA bundle, and heap.")
    print("This relocatable-link measurement is not the final firmware BIN delta.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mips", action="store_true")
    parser.add_argument("--skip-host", action="store_true")
    parser.add_argument("--dependencies", type=Path, default=ROOT / "build/ai-network-deps")
    arguments = parser.parse_args()
    if not arguments.skip_host:
        host(arguments.dependencies.resolve())
    if arguments.mips:
        mips(arguments.dependencies.resolve())
