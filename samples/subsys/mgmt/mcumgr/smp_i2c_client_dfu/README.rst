.. _smp_i2c_client_dfu:

SMP I2C Client DFU Sample
#########################

Overview
********

This sample updates the firmware of a second board over I2C. The board running
the sample is the I2C controller. It talks to a target board that runs an
MCUmgr SMP server on the ``zephyr,smp-i2c`` transport, using the SMP client
APIs (``smp_client_object_init``, ``os_mgmt_client_echo``,
``img_mgmt_client_upload``, ``img_mgmt_client_state_write`` and
``os_mgmt_client_reset``).

The image to upload is compiled into the controller at build time from
``CONFIG_APP_DFU_IMAGE_FILE``. No firmware is hard-coded in the source.

Requirements
************

Two Zephyr boards, wired controller to target with three wires.

* SDA to SDA
* SCL to SCL
* GND to GND

The target board runs an application that enables the SMP I2C target transport
(``CONFIG_MCUMGR_TRANSPORT_I2C``) over MCUboot, for example
``samples/subsys/mgmt/mcumgr/smp_svr`` with a ``zephyr,smp-i2c`` node.

Building and Running
********************

The setup uses two boards wired together over I2C:

* Board A, the controller, runs this sample.
* Board B, the target, runs an SMP server on ``zephyr,smp-i2c``.

The examples use two FRDM-MCXN236 boards; change ``-b`` for other boards. In
all there are three builds and two flashes: Board B runs its own firmware, the
image Board A uploads is a separate build that is never flashed directly, and
Board A runs this sample with that image compiled in.

1. Board B, build and flash the firmware it runs. Any application that serves
   MCUmgr over MCUboot and enables ``CONFIG_MCUMGR_TRANSPORT_I2C`` with a
   ``zephyr,smp-i2c`` node works; ``smp_svr`` is a convenient base. Build it
   with sysbuild so MCUboot is included, passing an overlay that adds the
   ``zephyr,smp-i2c`` node on the board I2C bus and a conf that turns the
   transport on, then flash Board B:

   .. code-block:: console

      west build -p -b frdm_mcxn236 --sysbuild -d build_target \
          samples/subsys/mgmt/mcumgr/smp_svr \
          -- -DEXTRA_DTC_OVERLAY_FILE=smp_i2c.overlay \
             -DEXTRA_CONF_FILE=overlay-i2c.conf
      west flash -d build_target

2. Build the image Board A will upload to Board B. This is a normal signed
   application image: a new version of the server, or another app that still
   includes the SMP I2C server. Build it but do not flash it:

   .. code-block:: console

      west build -p -b frdm_mcxn236 --sysbuild -d build_image \
          samples/subsys/mgmt/mcumgr/smp_svr \
          -- -DEXTRA_DTC_OVERLAY_FILE=smp_i2c.overlay \
             -DEXTRA_CONF_FILE=overlay-i2c.conf

   The signed image is left at
   ``build_image/smp_svr/zephyr/zephyr.signed.bin``.

3. Board A, build this sample with the step 2 image compiled in, then flash
   Board A. Use an absolute path, since the file is read at build time:

   .. code-block:: console

      west build -p -b frdm_mcxn236 -d build_ctrl \
          samples/subsys/mgmt/mcumgr/smp_i2c_client_dfu \
          -- -DCONFIG_APP_DFU_IMAGE_FILE="$PWD/build_image/smp_svr/zephyr/zephyr.signed.bin"
      west flash -d build_ctrl

Open the Board A console (115200 8N1) with your serial terminal of choice:

.. code-block:: console

   minicom -D /dev/ttyACM0

On boot Board A pings Board B, reads its image state, uploads the step 2 image
to Board B second slot, marks it for test, resets Board B, and confirms the
image once Board B answers again over I2C.
