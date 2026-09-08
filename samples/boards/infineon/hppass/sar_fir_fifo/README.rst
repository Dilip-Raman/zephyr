.. zephyr:code-sample:: hppass_sar_fir_fifo
   :name: HPPASS SAR ADC FIR and FIFO
   :relevant-api: adc_interface

   Filter HPPASS SAR ADC results in hardware and bulk-collect them through the FIFO.

Overview
********

This sample reads one HPPASS SAR ADC channel every 500 ms and shows the two
vendor extensions alongside the normal :c:func:`adc_read` path:

* The channel result is also pushed into a FIFO buffer, tagged with the channel
  that produced it.  Reaching the watermark fires a callback, which drains all
  three buffers in use.
* FIR filter 0 and FIR filter 1 both take that same channel as their input and
  run side by side, filter 0 over all 16 taps and filter 1 over 8.  Filter 0 is
  read as ADC channel 30 through :c:func:`adc_read`, filter 1 through the vendor
  call :c:func:`adc_ifx_hppass_sar_fir_read`, and both also land in their own
  FIFO buffers.

Requirements
************

A board with an HPPASS SAR ADC that exposes a FIFO block and FIR filters, such
as :zephyr:board:`kit_psc3m5_evk`.  No external wiring is needed - channel 12 is
the onboard potentiometer R6 on that board.  Turn the knob to see the filter
smooth the raw jitter next to it.  Jumper J9 must be fitted, since it is what
supplies the potentiometer.

The overlay must provide ``io-channels`` in ``zephyr,user``, a ``fifo`` node and
``fir-0``/``fir-1`` nodes under the ADC controller.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/boards/infineon/hppass/sar_fir_fifo
   :board: kit_psc3m5_evk/psc3m5fds2afq1
   :goals: build flash
   :compact:

Reading the output
==================

``status`` in the watermark line is a bitmask, one bit per FIFO buffer, so it is
printed in hex: ``0x1`` means only buffer 0 reached its watermark, ``0x7`` means
buffers 0, 1 and 2 all did.

The two filters are reported through different APIs on purpose.  ``FIR[0]`` is
read as ADC channel 30 through :c:func:`adc_read`, which keeps returning the
latest output and only reports ``-EAGAIN`` until the filter has produced
something.  ``FIR[1]`` uses the vendor call :c:func:`adc_ifx_hppass_sar_fir_read`,
which hands out each output once and reports ``-EAGAIN`` until the next one.

Both filters are declared with ``infineon,wait-tap-init``, so the hardware holds
their output until 16 samples have shifted in.  That wait is a fixed 16 in
hardware whatever the coefficients are, so it costs 16 conversions after every
reset and it is why the first readings below report no result while buffer 0 is
already carrying raw samples.  Coefficients survive a reset but the tap history
does not, so the wait happens on every boot.

Dropping ``infineon,wait-tap-init`` gives a result from the very first
conversion instead, at the cost of the filter reporting while its taps are still
part zero: a filter with N nonzero taps needs N conversions before the output is
meaningful.

Sample output
=============

Captured on ``kit_psc3m5_evk/psc3m5fds2afq1`` with the potentiometer steady:

.. code-block:: console

   *** Booting Zephyr OS build v4.4.0-16331-g89987752c4af ***
   ADC reading[0]:
   - adc@80000, channel 12: 852 = 686 mV
   FIR[0]: no result yet (channel 30)
   FIR[1]: no new result
   ADC reading[1]:
   - adc@80000, channel 12: 2533 = 2040 mV
   FIR[0]: no result yet (channel 30)
   FIR[1]: no new result
   FIR[0]: reload to 4-tap average -> 0
   FIR[0]: taps now 0x2000 0x2000 0x2000 0x2000
   ...
   ADC reading[3]:
   - adc@80000, channel 12: 2533 = 2040 mV
   FIR[0]: no result yet (channel 30)
   FIFO: watermark event (status=0x00000001)
   FIFO[0] RAW : 4 words - ch12=852 ch12=2533 ch12=2532 ch12=2533
   FIFO[1] FIR0: 0 words -
   FIFO[2] FIR1: 0 words -
   FIR[1]: no new result
   ...
   ADC reading[15]:
   - adc@80000, channel 12: 2533 = 2040 mV
   FIR[0]: 2533 (channel 30)
   FIFO: watermark event (status=0x00000001)
   FIFO[0] RAW : 4 words - ch12=2533 ch12=2533 ch12=2534 ch12=2533
   FIFO[1] FIR0: 1 words - ch30=2533
   FIFO[2] FIR1: 1 words - ch31=2532
   FIR[1]: 2532
   ...
   ADC reading[19]:
   - adc@80000, channel 12: 2529 = 2037 mV
   FIR[0]: 2528 (channel 30)
   FIFO: watermark event (status=0x00000007)
   FIFO[0] RAW : 4 words - ch12=2529 ch12=2525 ch12=2530 ch12=2529
   FIFO[1] FIR0: 4 words - ch30=2532 ch30=2530 ch30=2529 ch30=2528
   FIFO[2] FIR1: 4 words - ch31=2532 ch31=2531 ch31=2531 ch31=2530
   FIR[1]: 2530

The first conversion after boot reads low (852 here) because it is taken before
the input has settled; the reading that follows is already correct.  From
``reading[15]`` the filters produce output, buffers 1 and 2 start filling, and
the watermark status widens from ``0x1`` to ``0x7``.  Turning the knob from that
point shows the raw channel jumping while the filtered values trail it.
