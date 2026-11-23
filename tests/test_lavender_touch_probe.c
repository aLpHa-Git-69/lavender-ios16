/* Run the real patched probe against the replacement panel's ACK sequence. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uintptr_t vm_offset_t;
#define WHYRED_TLMM_BASE 0x03000000u
#define WHYRED_TLMM_SOUTH 0x00100000u
#define WHYRED_TLMM_NORTH 0x00900000u
#define TLMM_REG_SIZE 0x1000u
#define TLMM_CTRL 0u
#define TLMM_IO 4u
#define WHYRED_SDA 2u
#define WHYRED_SCL 3u
#define WHYRED_RST 66u
#define WHYRED_IRQ 67u
#define VM_WIMG_IO 0u
#define DSX_ADDR 0x20u

struct touch_pin { vm_offset_t cfg, io; uint32_t idle; };
struct dsx_funcs { int found_f01, found_f12; uint16_t f12_data; };
static struct touch_pin touch_sda, touch_scl;
static int touch_probe_done, enabled, missing_arg, ack30, ack62, ack20, fw_ready;
static uint8_t touch_fw_addr;
static uint32_t touch_max_x, touch_max_y;
static vm_offset_t touch_north;
static unsigned bus_ops, maps, register_reads, resets, params, threads, dsx_calls;

static int PE_parse_boot_argn(const char *name, void *value, unsigned size)
{
    assert(size == sizeof(uint32_t));
    *(uint32_t *)value = 0;
    if (strcmp(name, "sweetouchi2c") == 0) {
        *(uint32_t *)value = (uint32_t)enabled;
        return !missing_arg;
    }
    return 1; /* wait/reset/halt/touch duration all disabled for this test */
}
static vm_offset_t ml_io_map_unmappable(vm_offset_t p, unsigned size, unsigned flags)
{
    (void)size; (void)flags; maps++; return p;
}
static uint32_t tlmm_rd(vm_offset_t p) { (void)p; return 1; }
static void pin_setup(struct touch_pin *p, vm_offset_t tile, unsigned pin)
{
    p->cfg = tile + pin * TLMM_REG_SIZE; p->io = p->cfg + 4; p->idle = 0;
}
static int pin_read(struct touch_pin *p) { (void)p; return 1; }
static void pin_out(struct touch_pin *p, int value) { (void)p; (void)value; }
static int i2c_clear_bus(void) { return 0; }
static int i2c_ack(uint8_t addr)
{
    bus_ops++;
    return (addr == 0x01 && fw_ready) || (addr == 0x30 && ack30) ||
           (addr == 0x62 && ack62) || (addr == DSX_ADDR && ack20);
}
static int nvt_read(uint8_t addr, uint8_t page, uint8_t reg, uint8_t *buf, int len)
{
    assert(addr == 0x62); /* A 0x30 write/read reproduces the old regression. */
    (void)page; (void)reg; register_reads++; memset(buf, 1, (unsigned)len); return 0;
}
static int nvt_write_cmd(uint8_t addr, uint8_t reg, uint8_t cmd)
{
    assert(addr == 0x62 && reg == 0 && cmd == 0x69);
    resets++; fw_ready = 1; return 0;
}
static int nvt_params(uint8_t addr, uint8_t p[17], unsigned *max,
                      uint32_t *width, uint32_t *height)
{
    assert(addr == 0x01 && fw_ready);
    params++; memset(p, 0, 17); *max = 10; *width = 1080; *height = 2340; return 0;
}
static void touch_irq_setup(vm_offset_t north) { (void)north; }
static void dsx_diag(void) { dsx_calls++; }
static void dsx_identify(struct dsx_funcs *f) { memset(f, 0, sizeof(*f)); }
static void dsx_touch_test(const struct dsx_funcs *f, uint32_t seconds)
{
    (void)f; (void)seconds;
}
static int panel_report(int count, uint8_t addr) { (void)count; (void)addr; return 0; }
static void nvt_poll(uint8_t addr, unsigned max, uint32_t w, uint32_t h, uint32_t seconds)
{
    (void)addr; (void)max; (void)w; (void)h; (void)seconds;
}
static void delay(int us) { (void)us; }
static void santoni_touch_start_thread(void) { threads++; }

#include "touch_probe_mock_impl.h"

static void reset_case(void)
{
    touch_probe_done = missing_arg = ack30 = ack62 = ack20 = fw_ready = 0;
    enabled = 1;
    bus_ops = maps = register_reads = resets = params = threads = dsx_calls = 0;
    touch_fw_addr = 0; touch_max_x = touch_max_y = 0; touch_north = 0;
}
int main(void)
{
    reset_case(); ack30 = ack62 = 1;
    assert(santoni_touch_probe() == 0);
    assert(resets == 1 && params == 1 && threads == 1 && register_reads == 1);
    assert(touch_fw_addr == 0x01 && touch_max_x == 1080 && touch_max_y == 2340);

    reset_case(); fw_ready = 1;
    assert(santoni_touch_probe() == 0);
    assert(resets == 0 && params == 1 && threads == 1 && register_reads == 0);

    reset_case(); ack30 = ack62 = fw_ready = 1;
    assert(santoni_touch_probe() == 0);
    assert(resets == 0 && params == 1 && threads == 1 && touch_fw_addr == 0x01);

    reset_case(); ack30 = 1;
    assert(santoni_touch_probe() == 0);
    assert(register_reads == 0 && resets == 0 && params == 0 && threads == 0);

    reset_case(); ack20 = 1;
    assert(santoni_touch_probe() == 0);
    assert(dsx_calls == 1 && register_reads == 0 && params == 0 && threads == 0);

    reset_case(); enabled = 0;
    assert(santoni_touch_probe() == 0 && maps == 0 && bus_ops == 0);
    reset_case(); missing_arg = 1;
    assert(santoni_touch_probe() == 0 && maps == 0 && bus_ops == 0);
    puts("test_lavender_touch_probe: observed 0x30/0x62 regression, firmware preference and bus guards passed");
    return 0;
}
