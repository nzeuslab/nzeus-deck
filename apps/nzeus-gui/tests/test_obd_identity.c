/* Exercise the real OBD state machine with adapter replies but no vehicle.
 * The serial transport and clock are simulated; this is not a hardware test. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "obd.h"
#include "serial.h"

static uint32_t now;
static char command[64], reply[256];
static int length, mode, car_present, sti_count, stdi_count, car_queries;
static int reject_restore, on_module, resets;
static int failures, checks;

uint32_t nzeus_tick_ms(void) { return now; }
int serial_open(const char *dev, int baud) { (void)dev; (void)baud; return 42; }
void serial_close(int fd) { (void)fd; }

static void answer(void)
{
    const char *text = "OK\r>";
    command[length] = 0;
    if (!strcmp(command, "ATZ")) { resets++; on_module = 0; text = "ELM327 v1.4b\r>"; }
    else if (!strcmp(command, "STI")) {
        sti_count++;
        text = mode == 1 ? "?\r>" : mode == 2 ? "" : "STN2232 v5.10.3\r>";
    } else if (!strcmp(command, "STDI")) {
        stdi_count++;
        text = mode == 3 ? "?\r>" : "OBDLink EX r2.7.1\r>";
    } else if (!strcmp(command, "ATDPN")) text = "A6\r>";
    else if (!strcmp(command, "0100")) {
        car_queries++;
        text = car_present ? "410000100000\r>" : "NO DATA\r>";
    } else if (!strcmp(command, "ATSH7F7")) on_module = 1;
    else if (!strcmp(command, "ATSH7DF")) {
        if (reject_restore) { text = reject_restore == 1 ? "?\r>" : ""; reject_restore = 0; }
        else on_module = 0;
    } else if (!strcmp(command, "010C")) {
        text = car_present && !on_module ? "410C1AF8\r>" : "NO DATA\r>";
    } else if (command[0] != 'A') text = "NO DATA\r>";
    snprintf(reply, sizeof reply, "%s", text);
    length = 0;
}

int serial_write(int fd, const char *data, int count)
{
    (void)fd;
    for (int i = 0; i < count; i++) {
        if (data[i] == '\r') answer();
        else if (length < (int)sizeof command - 1) command[length++] = data[i];
    }
    return count;
}

int serial_read(int fd, char *data, int size)
{
    (void)fd;
    int count = (int)strlen(reply);
    if (count > size) count = size;
    memcpy(data, reply, (size_t)count);
    memmove(reply, reply + count, strlen(reply + count) + 1);
    return count;
}

static void check(int condition, const char *name)
{
    checks++;
    printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) failures++;
}

static void poll_for(int milliseconds)
{
    for (int i = 0; i < milliseconds; i += 10) { now += 10; obd_poll(); }
}

int main(void)
{
    for (mode = 0; mode < 4; mode++) {
        obd_stop();
        now = 0; length = car_present = sti_count = stdi_count = car_queries = 0;
        reply[0] = 0;
        obd_start("simulated-adapter");
        poll_for(12000);  /* includes a car retry and a silent STI timeout */
        check(obd_state() == OBD_NO_CAR, "Adapter identification never implies a vehicle answered");
        check(sti_count == 1, "Identify once while disconnected, without repeated identity queries");
        check(car_queries >= 2, "Vehicle retries continue after identity or timeout");
        int readings = 0;
        for (int i = 0; i < PID_COUNT; i++) readings += obd_live()->have[i] != 0;
        check(readings == 0, "No vehicle values are invented");
        if (mode == 0) {
            check(obd_is_stn() && !strcmp(obd_adapter(), "OBDLink EX r2.7.1"), "Recognize EX without a car");
            check(strstr(obd_describe(), "OBDLink EX") != NULL, "No-car status shows the identified adapter");
        } else {
            check(!strcmp(obd_adapter(), "ELM327 v1.4b"), "Unsupported, silent or invalid identity keeps reset banner");
            if (mode != 3) check(!obd_is_stn() && stdi_count == 0, "Non-STN adapter never receives STDI");
        }
        car_present = 1;
        poll_for(6000);
        check(obd_state() == OBD_READY, "Vehicle can connect later without reconnecting the adapter");
    }
    for (int failure = 1; failure <= 2; failure++) {
        obd_stop(); mode = 0; car_present = 1; resets = reject_restore = on_module = 0;
        reply[0] = 0; length = 0;
        obd_start("simulated-adapter"); poll_for(1000);
        check(obd_live()->have[PID_RPM], "Restore recovery: initial live data works");
        obd_session(1);
        check(obd_ask("ATSH7F7", 1500) == 0, "Restore recovery: scan selects a module");
        poll_for(100);
        reject_restore = failure;
        obd_session(0);
        poll_for(6000);
        check(resets == 2, "Restore recovery: rejected or silent restore reinitializes once");
        check(obd_state() == OBD_READY && obd_live()->have[PID_RPM] && !on_module,
              "Restore recovery: live readings return after a failed scan restore");
    }
    obd_stop();
    printf("OBD identity: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
