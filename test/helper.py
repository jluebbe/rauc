# SPDX-License-Identifier: LGPL-2.1-only
# SPDX-FileCopyrightText: 2021 Enrico Jörns <e.joerns@pengutronix.de>, Pengutronix
# SPDX-FileCopyrightText: 2021-2022 Bastian Krause <bst@pengutronix.de>, Pengutronix

import os
import subprocess
import shlex
import logging


def logger_from_command(command):
    """
    Returns a logger named after the executable, or in case of a python executable, after the
    python module,
    """
    cmd_parts = command.split()
    base_cmd = os.path.basename(cmd_parts[0])
    try:
        if base_cmd.startswith("python") and cmd_parts[1] == "-m":
            base_cmd = command.split()[2]
    except IndexError:
        pass

    return logging.getLogger(base_cmd)


def run(command, *, timeout=30):
    """
    Runs given command as subprocess with DBUS_STARTER_BUS_TYPE=session and PATH+=./build. Blocks
    until command terminates. Logs command and its stdout/stderr/exit code.
    Returns tuple (stdout, stderr, exit code).
    """
    logger = logger_from_command(command)
    logger.info("running: %s", command)

    proc = subprocess.run(shlex.split(command), capture_output=True, text=True, check=False, timeout=timeout)

    for line in proc.stdout.splitlines():
        if line:
            logger.info("stdout: %s", line)
    for line in proc.stderr.splitlines():
        if line:
            logger.warning("stderr: %s", line)

    logger.info("exitcode: %d", proc.returncode)

    return proc.stdout, proc.stderr, proc.returncode
