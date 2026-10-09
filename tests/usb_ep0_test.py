"""Compile the ACTUAL generated EP0 routines against deterministic registers."""
from pathlib import Path
import argparse
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--compiler', default='g++')
args = parser.parse_args()
generated = Path('build/dcd_rp2040_nonblocking.c').read_text()

def function(signature):
    start = generated.index(signature)
    opening = generated.index('{', start)
    depth = 1
    pos = opening + 1
    while depth:
        if generated[pos] == '{': depth += 1
        if generated[pos] == '}': depth -= 1
        pos += 1
    return generated[start:pos]

prefix = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#define TU_ATTR_ALWAYS_INLINE
#define USBCTRL_IRQ 5
#define USB_EP_ABORT_EP0_IN_BITS 1u
#define USB_EP_ABORT_EP0_OUT_BITS 2u
#define USB_BUF_CTRL_DATA1_PID 0x2000u
#define USB_BUF_CTRL_SEL 0x1000u
#define USB_INTS_SETUP_REQ_BITS 1u
#define USB_SIE_STATUS_SETUP_REC_BITS 2u
#define USB_SIE_CTRL_PULLUP_EN_BITS 4u
#define remove_volatile_cast(type, ptr) reinterpret_cast<type>(ptr)
struct USB { uint32_t abort=0, abort_done=0, inte=1, sie_status=2, sie_ctrl=4; } hw, set_regs, clear_regs;
USB *usb_hw=&hw, *usb_hw_set=&set_regs, *usb_hw_clear=&clear_regs;
struct DPRAM { uint8_t setup_packet[8]{}; } dpram;
DPRAM* usb_dpram=&dpram;
struct hw_endpoint { bool active=true; unsigned next_pid=0, control=0; } endpoints[2];
hw_endpoint* hw_endpoint_get_by_num(int, int dir) { return &endpoints[dir]; }
void _hw_endpoint_buffer_control_set_value32(hw_endpoint* ep, uint32_t value) { ep->control=value; }
void hw_endpoint_reset_transfer(hw_endpoint* ep) { ep->active=false; }
unsigned chip_version=2, events=0, reads_of_time=0;
unsigned rp2040_chip_version() { return chip_version; }
bool irq_enabled=true;
void irq_set_enabled(unsigned, bool enabled) { irq_enabled=enabled; }
uint32_t now=0;
uint32_t time_us_32() { ++reads_of_time; return now; }
void dcd_event_setup_received(unsigned, const uint8_t*, bool in_isr) { assert(!in_isr); ++events; }
static volatile bool chuni_setup_pending=false;
static uint32_t chuni_setup_started=0, chuni_ep0_timeouts=0;
'''
tests = r'''
void reset_fixture() {
    hw=USB{}; set_regs=USB{}; clear_regs=USB{};
    endpoints[0]=hw_endpoint{}; endpoints[1]=hw_endpoint{};
    chip_version=2; events=0; reads_of_time=0; irq_enabled=true;
    now=0; chuni_setup_pending=false; chuni_setup_started=0; chuni_ep0_timeouts=0;
}
int main() {
    reset_fixture();
    assert(!reset_ep0()); // no ACK: must return immediately, with no time polling
    assert(reads_of_time==0 && endpoints[0].active);
    chuni_setup_pending=true;
    now=999;
    assert(chuni_dcd_task() && chuni_setup_pending && events==0 && irq_enabled);
    hw.abort_done=3;
    assert(chuni_dcd_task() && !chuni_setup_pending && events==1 && irq_enabled);
    assert(!endpoints[0].active && !endpoints[1].active);
    assert(endpoints[0].next_pid==1 && endpoints[1].next_pid==1);
    assert(chuni_dcd_task() && events==1); // setup is not queued twice

    reset_fixture();
    chuni_setup_pending=true;
    chuni_setup_started=UINT32_MAX-500;
    now=499; // exactly 1000us across wrap
    assert(!chuni_dcd_task());
    assert(chuni_ep0_timeouts==1 && !irq_enabled && hw.inte==0);
    assert(clear_regs.sie_ctrl==USB_SIE_CTRL_PULLUP_EN_BITS && !chuni_setup_pending);

    reset_fixture();
    chip_version=1; // B0/B1 must retain upstream no-ABORT behavior
    assert(reset_ep0());
    assert(!endpoints[0].active && !endpoints[1].active);
    std::cout << "PASS: actual EP0 routines defer without spinning, complete once, "
                 "time out across clock wrap, disable IRQ on fault, preserve B0/B1 path\n";
}
'''
output = Path('build/usb_ep0_test.cpp')
output.write_text(prefix + function('TU_ATTR_ALWAYS_INLINE static inline bool reset_ep0(void)') +
                  '\n' + function('bool chuni_dcd_task(void)') + tests)
subprocess.run([args.compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', str(output),
                '-o', 'build/usb_ep0_test.exe'], check=True, timeout=60)
subprocess.run([str(Path('build/usb_ep0_test.exe').resolve())], check=True, timeout=10)
