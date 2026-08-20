
**During this exercise I found and fixed 6 bugs in the flight software stack, and implemented a fault injection on a separate branch to deliberately trigger a structural failure at a predicted tick. Here's what I found, where I found it, and how I fixed each one - followed by the fault injection reasoning at the end.**

# BUG-1 (causes the segfault at tick 1026)

**File:** src/drivers/telemetry_ingest.c

**Description:**
 telemetry_cursor is a raw pointer starting at &shared.queue[0] and increments every tick with zero bounds checking. At tick 1025 it walks off the end of queue[1024] and overwrites fault_check_fn inside SharedMemoryRegion. At tick 1026, Fault Check task (period=5, so it runs at ticks 1,6,11...1021,1026) calls the now-garbage function pointer-->SIGSEGV.

 **FIX :**
  I replaced the raw pointer increment with a ring buffer using modulo arithmetic. Instead of telemetry_cursor++ which just kept moving forward with no limit, I calculated the index as telemetry_frames % QUEUE_SIZE so it wraps back to 0 after hitting 1024. I also stopped using the cursor as the write target directly and instead used &state->shared.queue[idx] with the calculated index. This keeps all writes inside the array boundary no matter how long the sim runs.


# BUG-2 (ADC voltage reporting half the true value (VBAT stuck-looking))

**File:** src/drivers/adc_driver.c, adc_scale_voltage()

**Description:**
 adc_scale_voltage() in src/drivers/adc_driver.c had a mismatch between how the ADC value was encoded and decoded. On the encode side in adc_read_impl(), the raw voltage is multiplied by 16 and then left-shifted by ADC_SHIFT (which is 4), packing it into an int32. On the decode side, adc_scale_voltage() was right-shifting by ADC_SHIFT + 1 (which is 5) instead of 4, and then dividing by 16. That extra shift right by 1 bit is equivalent to dividing by 2, so every voltage reading coming out of the ADC was exactly half the true battery voltage. The sim starts with a true battery voltage of 6.35V but was reporting roughly 3.1V from tick 1. Because power_update uses battery_voltage_reported to check for safe mode entry, the satellite was tripping into safe mode around tick 1497 based on a completely wrong voltage reading - the battery was actually fine.

 **FIX :** 
 I removed the + 1 from the right shift in adc_scale_voltage. The encode side shifts left by ADC_SHIFT (4 bits) to pack the value, so the decode side needs to shift right by exactly the same amount to unpack it. Shifting right by 5 instead of 4 was losing one extra bit, which halved every voltage reading. Changing (ADC_SHIFT + 1) to ADC_SHIFT made encode and decode symmetric.

# BUG-3 (CRC computed over pre-scaled temperature)

**File:** src/middleware/telemetry_pipeline.c, packet_prepare()

**Description:** 
In packet_prepare() inside src/middleware/telemetry_pipeline.c, the CRC was being calculated before the temperature value was scaled. The function first populated the packet fields, then computed crc16_ccitt over the packet bytes, and then called packet_units_adjust() which multiplied temperature_raw by 10. This means the CRC was computed over a temperature value of say -14, but the packet that actually went out on the wire had a temperature of -140. Any ground station receiving this packet and recomputing the CRC over the actual bytes it received would get a completely different value and flag every single telemetry packet as corrupted. The data was physically transmitted correctly but the integrity check was always wrong because the checksum didn't match the final state of the data.

 **FIX :**
  I moved the packet_units_adjust(packet) call to happen before the CRC calculation instead of after. The CRC has to be computed over the final state of the packet data - the same bytes that will actually be sent on the wire. By scaling the temperature first and then computing the CRC, the checksum now covers the actual transmitted values and a ground station verifying it will get a match.


# BUG-4 (uint16_t overflow causes thermal readings to go permanently stale at tick 1700)

**File:** include/flight/state.h

**Description:** 
ThermalTaskState.runtime_ms is uint16_t (max 65535). It starts at 950 and increments 38 per tick. At tick 1700: 950 + 38*1700 = 65550 --> wraps to 14.
Meanwhile RefreshTaskState.runtime_ms is uint32_t so it never overflows and keeps growing. The comparison between that large value and the wrapped 14 becomes true, scheduler_bias_flipped permanently sets to true,and thermal_monitor_update forever logs previous_raw instead of current_raw - all temperature readings become one tick stale for the rest of the sim.

 **FIX :**
  I changed runtime_ms in ThermalTaskState from uint16_t to uint32_t. The value accumulates 38 every tick starting from 950, so by tick 1700 it exceeds 65535 which is the maximum a uint16_t can hold. It silently wraps around to a tiny number, making the scheduler think the thermal task had barely run, which permanently flipped the bias flag. A uint32_t can hold values up to about 4 billion so it never overflows within the simulation.

# BUG-5 (temp_copy buffer overflow when temperature = -1)

**File:** src/drivers/temp_copy.c, temp_guarded_copy()

**Description:**
 temp_guarded_copy() in src/drivers/temp_copy.c calculates the number of bytes to copy as copy_len = (uint16_t)(32 - temp). When temperature is negative - which happens regularly since the thermal model ranges from about -18 to +34 - this produces a copy length larger than 32. At temp = -1 for example, copy_len becomes 33. The destination buffer frame.dest is only SENSOR_COPY_DEST = 16 bytes. There's a guard condition meant to catch underflow using size_t arithmetic (shifted = temp + gate where gate is 2), but when temp = -1, shifted becomes 1 which is less than 2, so the guard passes and the oversized memcpy goes ahead. This writes 33 bytes into a 16-byte buffer, overflowing directly into the canary array sitting right after it in the struct. The sim detected this at tick 16 when the canary bytes were found modified - you can see [WARN] Sensor copy canary modified at tick 16 in the raw output. In real flight software this kind of overflow could corrupt adjacent state silently with no warning at all.

 **FIX :** 
 I added a clamp before the memcpy that caps copy_len at SENSOR_COPY_DEST (16 bytes) if it would exceed that. The length was calculated as 32 - temp which produces values larger than the destination buffer when temperature goes negative. The clamp ensures we never try to copy more bytes than the buffer can hold, so the canary bytes sitting right after the buffer stay untouched.



# BUG-6 ((latent) -  gets() in legacy_diag.c)

**File:** src/utils/legacy_diag.c, legacy_diag_dump()

**Description:** 
legacy_diag_dump() in src/utils/legacy_diag.c uses gets(line) to read input into a 32-byte stack buffer. gets has no length parameter - it just reads until a newline regardless of how much space is available. This means any input longer than 31 characters silently overwrites adjacent stack memory. The function isn't called anywhere in the sim loop, but it's compiled into the binary and the linker even threw a warning about it, which is what flagged it during the build. It's a latent vulnerability - harmless in this sim but dangerous in any real deployment where this function could get invoked.

 **FIX :** 
 I replaced gets(line) with fgets(line, sizeof(line), stdin). The gets function reads characters until a newline with no concept of how big the destination buffer is, so any input longer than 31 characters would write past the end of the array and into whatever memory sits next to it on the stack. fgets takes the buffer size as a parameter and stops reading at sizeof(line) - 1 characters, leaving room for the null terminator, so it can never write outside the array.


 # THE FAULT INJECTION: 

**What I changed:** Added cmd_set_actuators(100.0, 5000.0) in main.c right after cmd_bind_state(&state). This sets the heater PWM to 100% and the reaction wheel target to 5000 RPM before the tick loop starts, so both actuators are running at maximum from tick 1 onwards.

**How the temperature builds up:** Inside physics_update_actuators, every tick adds (target_heater_pwm / 100.0) * 0.25 of heat and subtracts a fixed 0.05 heat loss. At 100% PWM that's 1.0 * 0.25 = 0.25 added and 0.05 lost, giving a net gain of +0.20 degree Celsius per tick. Starting temperature is 20.0 degree Celsius and TEMP_CRITICAL is 85.0 degree Celsius, so the gap to cross is 65 degree Celsius. At 0.20 degree Celsius per tick that takes exactly 65 / 0.20 = 325 ticks. So temperature crosses the threshold sometime during tick 326.

**How the vibration builds up:** The wheel RPM ramps toward 5000 at 50 RPM per tick (the delta is capped at 50). So it reaches 5000 RPM at tick 100 and stays there. Vibration is calculated as 0.5 + actual_wheel_rpm * 0.0025. At 5000 RPM that's 0.5 + 12.5 = 13.0. VIB_CRITICAL is 12.0, so vibration exceeds the threshold from tick 100 onwards.

**Why tick 326:** The vibration condition is satisfied from tick 100. The temperature condition is satisfied from tick 326. Both have to be true at the same time for the structural failure to trigger. The first tick where both are simultaneously true is tick 326, so that's when physics_update_actuators hits the fatal branch and calls exit(42).

fault_manifest.json: `{"expected_crash_tick": 326}`

# CONCLUSION:
**All 6 identified bugs were fixed and verified against a clean 2250-tick run.**