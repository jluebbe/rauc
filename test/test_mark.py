from conftest import have_grub, no_service
from helper import run


@no_service
@have_grub
def test_status_mark_good_internally(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-good")

    assert exitcode == 0


@no_service
@have_grub
def test_status_mark_bad_internally(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-bad")

    assert exitcode == 0


@no_service
@have_grub
def test_status_mark_active_internally(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-active")

    assert exitcode == 0


@no_service
@have_grub
def test_status_mark_good_booted(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-good booted")

    assert exitcode == 0


@no_service
@have_grub
def test_status_mark_good_other(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-good other")

    assert exitcode == 0


@no_service
@have_grub
def test_status_mark_good_any_bootslot(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-good rescue.0")

    assert exitcode == 0


@no_service
@have_grub
def test_status_mark_good_non_bootslot(rauc_no_service):
    out, err, exitcode = run(f"{rauc_no_service} --override-boot-slot=system0 status mark-good bootloader.0")

    assert exitcode == 1


@have_grub
def test_status_mark_good_dbus(rauc_dbus_service_with_system):
    out, err, exitcode = run("rauc status mark-good")

    assert exitcode == 0
    assert "marked slot rootfs.0 as good" in out


@have_grub
def test_status_mark_bad_dbus(rauc_dbus_service_with_system):
    out, err, exitcode = run("rauc status mark-bad")

    assert exitcode == 0
    assert "marked slot rootfs.0 as bad" in out


@have_grub
def test_status_mark_active_dbus(rauc_dbus_service_with_system):
    out, err, exitcode = run("rauc status mark-active")

    assert exitcode == 0
    assert "activated slot rootfs.0" in out
