.. _zbus_hlp_bme280:

zbus Observer Types + HLP Demo with BME280
==========================================

Overview
--------

This sample demonstrates all four zbus observer types working with a real
`Bosch BME280`_ temperature/humidity/pressure sensor on the nRF52840DK.

It is designed to be a reference for:

* Understanding the **execution context** and **delivery guarantee** of each
  observer type
* Observing and understanding **HLP (Priority Boost)** — how zbus prevents
  higher-priority observer threads from preempting the publisher mid-dispatch
* Using a **channel validator** to reject physically impossible sensor values
* Attaching **channel user_data** (metadata) accessible to all observers
* Integrating the classic **Zephyr Sensor API** (``sensor_sample_fetch`` +
  ``sensor_channel_get``) with zbus.

Requirements
------------

Hardware
~~~~~~~~

* `nRF52840 DK`_
* Bosch BME280 breakout board (any common module)

Building and Running
--------------------

Default build (HLP enabled, LOG_LEVEL=INF):

.. code-block:: bash

   west build -b nrf52840dk/nrf52840 -p always samples/subsys/zbus/zbus_hlp_bme280

   west flash

.. code-block:: bash

    minicom -D /dev/ttyACM0


HLP Toggle
~~~~~~~~~~

To compare HLP-on vs HLP-off behaviour, edit ``prj.conf``:

.. code-block:: kconfig

   # HLP ON (default) — publisher boosted during VDED
   CONFIG_ZBUS_PRIORITY_BOOST=y

   # HLP OFF — comment out or set to n
   # CONFIG_ZBUS_PRIORITY_BOOST=n

Expected output comparison (watch ``prio_VDED`` in ``[LISTENER]`` line):

.. code-block:: none

   # HLP ON
   [LISTENER] seq=3 | temp=27.90°C | hum=39.92% | prio_VDED=2 | HLP:BOOSTED

   # HLP OFF
   [LISTENER] seq=3 | temp=27.90°C | hum=39.92% | prio_VDED=5 | HLP:DISABLED


Sample Output
-------------

Normal output  with HLP enabled:

.. code-block:: none

    *** Booting Zephyr OS build v4.4.0 ***

   [00:00:00.269] <inf> zbus_hlp:    <<< zbus + BME280 on nRF52840DK >>>

   [00:00:00.269] <inf> zbus_hlp: BME280 device 'BME280_I2C' ready!
   [00:00:00.269] <inf> zbus_hlp: Priority map:
   [00:00:00.269] <inf> zbus_hlp:   sys workqueue : -1  (cooperative, async_listener)
   [00:00:00.269] <inf> zbus_hlp:   msg_sub thread:  3  (higher than publisher — HLP needed)
   [00:00:00.269] <inf> zbus_hlp:   publisher     :  5  (HLP boosts to 2 during VDED)
   [00:00:00.269] <inf> zbus_hlp:   sub thread    :  6  (lower than publisher, safe)
   [00:00:00.269] <inf> zbus_hlp: HLP: ENABLED — observe prio_VDED in [LISTENER] output
   [00:00:00.269] <inf> zbus_hlp:   Expected: prio_VDED=2 (boosted above msg_sub=3)

   [00:00:00.340] <inf> zbus_hlp: [LISTENER] seq=1 | temp=27.78°C | hum=40.77% | prio_VDED=2 | HLP:BOOSTED | total_pub=0
   [00:00:00.340] <inf> zbus_hlp: [MSG_SUB]  seq=1 | temp=27.78°C | hum=40.77% | ph=96.34 hPa | ts=269 ms | full-copy, never lost
   [00:00:00.341] <inf> zbus_hlp: [SUB]      seq=1 | temp=27.78°C | hum=40.77% | latest-value semantics | prio=6

   [00:00:10.269] <inf> zbus_hlp: [MAIN] channel stats — published=5 sensor_errors=0 validator_rejects=0 last_pub=8347 ms

.. code-block:: none

*** Booting Zephyr OS build v4.4.0-1388-g96d892e878ba ***
[00:00:00.269,226] <inf> zbus_hlp:
<<< zbus + BME280 on nRF52840DK >>>

[00:00:00.269,256] <inf> zbus_hlp: BME280 device 'BME280_I2C' ready!
[00:00:00.269,256] <inf> zbus_hlp: Priority map:
[00:00:00.269,256] <inf> zbus_hlp:   sys workqueue : -1  (cooperative, runs async_listener)
[00:00:00.269,256] <inf> zbus_hlp:   msg_sub thread:  3  (higher than publisher — HLP needed)
[00:00:00.269,287] <inf> zbus_hlp:   publisher     :  5  (HLP boosts to 3 during VDED)
[00:00:00.269,287] <inf> zbus_hlp:   sub thread    :  6  (lower than publisher, safe)
[00:00:00.269,287] <inf> zbus_hlp: Channel: sensor_chan | validator: BME280 range check | user_data: &chan_meta
[00:00:00.269,348] <inf> zbus_hlp: [MSG_SUB] started | prio=3 (higher than publisher=5, HLP needed)
[00:00:00.269,439] <inf> zbus_hlp: [PUB] started | prio=5 | BME280 sensor: BME280_I2C
[00:00:00.269,500] <inf> zbus_hlp: [SUB] started | prio=6 (lower than publisher, no preemption risk)
[00:00:00.340,759] <inf> zbus_hlp: [PUB] seq=1 | temp=26.29°C | hum=43.99% | ph=96.39 hPa | valid=1
[00:00:00.340,759] <inf> zbus_hlp: [PUBLISHER] prio BEFORE pub = 5 (my natural priority)

[00:00:00.340,789] <inf> zbus_hlp: [LISTENER] seq=1 | temp=26.29°C | hum=43.99% | prio_VDED=5 | HLP:DISABLED
[00:00:00.340,850] <inf> zbus_hlp: [ASYNC] seq=1 temp=26.29°C hum=43.99% | workqueue ctx
[00:00:00.340,972] <inf> zbus_hlp: [MSG_SUB] seq=1 | temp=26.29°C | hum=43.99% | ph=96.39 hPa |ts=269 ms | full-copy, never lost
[00:00:00.341,033] <inf> zbus_hlp: [PUBLISHER] prio AFTER  pub = 5 (restored by HLP)

[00:00:00.341,094] <inf> zbus_hlp: [SUB] seq=1 | temp=26.29°C | hum=43.99% | latest-value semantics |prio=6
[00:00:02.342,895] <inf> zbus_hlp: [PUB] seq=2 | temp=26.32°C | hum=43.32% | ph=96.39 hPa | valid=1
[00:00:02.342,926] <inf> zbus_hlp: [PUBLISHER] prio BEFORE pub = 5 (my natural priority)

[00:00:02.342,956] <inf> zbus_hlp: [LISTENER] seq=2 | temp=26.32°C | hum=43.32% | prio_VDED=5 | HLP:DISABLED
[00:00:02.343,017] <inf> zbus_hlp: [ASYNC] seq=2 temp=26.32°C hum=43.32% | workqueue ctx
[00:00:02.343,139] <inf> zbus_hlp: [MSG_SUB] seq=2 | temp=26.32°C | hum=43.32% | ph=96.39 hPa |ts=2341 ms | full-copy, never lost
[00:00:02.343,200] <inf> zbus_hlp: [PUBLISHER] prio AFTER  pub = 5 (restored by HLP)

.. note::
