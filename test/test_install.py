import os
import shutil

from helper import run
from conftest import root
from conftest import no_service
from conftest import have_casync
from conftest import have_http
from conftest import have_streaming


@root
def test_install(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0
    assert os.path.isdir("/run/rauc/slots/active")
    assert os.path.islink("/run/rauc/slots/active/rootfs")
    assert os.readlink("/run/rauc/slots/active/rootfs")

    # copy to tmp path for safe ownership check
    shutil.copy("good-bundle.raucb", tmp_path / "good-bundle.raucb")

    out, err, exitcode = run(f"rauc install {tmp_path}/good-bundle.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
def test_install_verity(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-verity-bundle.raucb", tmp_path / "good-verity-bundle.raucb")

    out, err, exitcode = run(f"rauc install {tmp_path}/good-verity-bundle.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
def test_install_crypt(rauc_service, rauc_dbus_service_with_system_crypt, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-crypt-bundle-encrypted.raucb", tmp_path / "good-crypt-bundle-encrypted.raucb")

    out, err, exitcode = run(f"rauc install {tmp_path}/good-crypt-bundle-encrypted.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@have_casync
def test_install_plain_casync_local(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-casync-bundle-1.5.1.raucb", tmp_path / "good-casync-bundle-1.5.1.raucb")
    shutil.copytree("good-casync-bundle-1.5.1.castr", tmp_path / "good-casync-bundle-1.5.1.castr")

    out, err, exitcode = run(f"rauc install {tmp_path}/good-casync-bundle-1.5.1.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@have_casync
@have_http
def test_install_verity_casync_http(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    out, err, exitcode = run("rauc install http://127.0.0.1/test/good-casync-bundle-verity.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@have_streaming
def test_install_streaming(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    out, err, exitcode = run("rauc install http://127.0.0.1/test/good-verity-bundle.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@have_streaming
def test_install_streaming_error(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    out, err, exitcode = run("rauc install http://127.0.0.1/test/missing-bundle.raucb")

    assert exitcode == 1
    assert not os.path.getsize("images/rootfs-1") > 0


@root
def test_install_progress(rauc_service, rauc_dbus_service_with_system, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-bundle.raucb", tmp_path / "good-bundle.raucb")

    out, err, exitcode = run(f"rauc install --progress {tmp_path}/good-bundle.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
def test_install_rauc_external(rauc_service, rauc_dbus_service_with_system_external, tmp_path):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-bundle.raucb", tmp_path / "good-bundle.raucb")

    out, err, exitcode = run(f"rauc install {tmp_path}/good-bundle.raucb")

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@no_service
def test_install_no_service(tmp_path, create_system_files):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-bundle.raucb", tmp_path / "good-bundle.raucb")

    out, err, exitcode = run(
        "rauc install " "--conf=minimal-test.conf " "--override-boot-slot=system0 " f"{tmp_path}/good-bundle.raucb"
    )

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@no_service
@have_streaming
def test_install_no_service_streaming(tmp_path, create_system_files):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    out, err, exitcode = run(
        "rauc "
        "--conf=minimal-test.conf "
        "--override-boot-slot=system0 "
        "install "
        "http://127.0.0.1/test/good-verity-bundle.raucb"
    )

    assert exitcode == 0
    assert os.path.getsize("images/rootfs-1") > 0


@root
@no_service
@have_streaming
def test_install_no_service_streaming_error(tmp_path, create_system_files):
    assert os.path.exists("images/rootfs-1")
    assert not os.path.getsize("images/rootfs-1") > 0

    # copy to tmp path for safe ownership check
    shutil.copy("good-bundle.raucb", tmp_path / "good-bundle.raucb")

    out, err, exitcode = run(
        "rauc "
        "--conf=minimal-test.conf "
        "--override-boot-slot=system0 "
        "install "
        "http://127.0.0.1/test/missing-bundle.raucb"
    )

    assert exitcode == 1
    assert not os.path.getsize("images/rootfs-1") > 0
