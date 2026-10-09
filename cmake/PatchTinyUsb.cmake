# Patch the build copy only. Keep the SDK installation and upstream license.
# Fail closed if a future TinyUSB version changes these exact insertion points.
set(_dcd_source "${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040/dcd_rp2040.c")
file(READ "${_dcd_source}" _dcd)
function(chuni_dcd_replace before after)
    string(FIND "${_dcd}" "${before}" _at)
    if (_at EQUAL -1)
        message(FATAL_ERROR "TinyUSB changed; review nonblocking EP0 patch: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" _dcd "${_dcd}")
    set(_dcd "${_dcd}" PARENT_SCOPE)
endfunction()
chuni_dcd_replace("TU_ATTR_ALWAYS_INLINE static inline void reset_ep0(void) {" [=[
static volatile bool chuni_setup_pending = false;
static uint32_t chuni_setup_started;
static uint32_t chuni_ep0_timeouts;
static bool chuni_timeout_noted;
TU_ATTR_ALWAYS_INLINE static inline bool reset_ep0(void) {
]=])
chuni_dcd_replace("while ((usb_hw->abort_done & abort_mask) != abort_mask) {}"
                 "if ((usb_hw->abort_done & abort_mask) != abort_mask) return false;")
chuni_dcd_replace("ep->next_pid = 1u;\n  }\n}" "ep->next_pid = 1u;\n  }\n  return true;\n}")
chuni_dcd_replace("    reset_ep0();" [=[
    if (!reset_ep0()) {
      // ABORT completion is deferred to Core0's loop, never polled in IRQ.
      usb_hw_clear->inte = USB_INTS_SETUP_REQ_BITS;
      chuni_setup_started = time_us_32();
      chuni_timeout_noted = false;
      chuni_setup_pending = true;
      return;
    }
]=])
chuni_dcd_replace("bool dcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {" [=[
// Deferred EP0 completion only. Never disconnect or disable the USB controller.
// A late ABORT completion may still arrive; check once per main-loop iteration.
bool chuni_dcd_task(void) {
  if (!chuni_setup_pending) return true;
  irq_set_enabled(USBCTRL_IRQ, false);
  if (reset_ep0()) {
    uint8_t const* setup = remove_volatile_cast(uint8_t const*, &usb_dpram->setup_packet);
    dcd_event_setup_received(0, setup, false);
    usb_hw_clear->sie_status = USB_SIE_STATUS_SETUP_REC_BITS;
    chuni_setup_pending = false;
    usb_hw_set->inte = USB_INTS_SETUP_REQ_BITS;
  } else if (!chuni_timeout_noted &&
             (uint32_t)(time_us_32() - chuni_setup_started) >= 1000) {
    chuni_ep0_timeouts++;
    chuni_timeout_noted = true;
    // Keep pending, pull-up and all non-SETUP IRQ sources intact.
  }
  irq_set_enabled(USBCTRL_IRQ, true);
  return true;
}
uint32_t chuni_dcd_timeout_count(void) { return chuni_ep0_timeouts; }
bool dcd_init(uint8_t rhport, const tusb_rhport_init_t* rh_init) {
  chuni_setup_pending = false;
]=])
chuni_dcd_replace("    usb_hw->dev_addr_ctrl = 0;" [=[
    chuni_setup_pending = false;
    usb_hw_clear->abort = USB_EP_ABORT_EP0_IN_BITS | USB_EP_ABORT_EP0_OUT_BITS;
    usb_hw_clear->abort_done = USB_EP_ABORT_EP0_IN_BITS | USB_EP_ABORT_EP0_OUT_BITS;
    usb_hw_set->inte = USB_INTS_SETUP_REQ_BITS;
    usb_hw->dev_addr_ctrl = 0;
]=])
set(_patched "${CMAKE_CURRENT_BINARY_DIR}/dcd_rp2040_nonblocking.c")
file(CONFIGURE OUTPUT "${_patched}" CONTENT "${_dcd}" @ONLY)
get_target_property(_sources tinyusb_device_base INTERFACE_SOURCES)
list(FIND _sources "${_dcd_source}" _source_index)
if (_source_index EQUAL -1)
    message(FATAL_ERROR "Could not replace TinyUSB RP2040 device source")
endif()
list(REMOVE_ITEM _sources "${_dcd_source}")
list(APPEND _sources "${_patched}")
set_property(TARGET tinyusb_device_base PROPERTY INTERFACE_SOURCES "${_sources}")
target_include_directories(tinyusb_device_base INTERFACE
    "${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040")
