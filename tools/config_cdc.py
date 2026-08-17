#!/usr/bin/env python3
"""
Chuni245Tof Configuration Tool
Keyboard-only mode
"""

import tkinter as tk
from tkinter import ttk, messagebox
import serial
import serial.tools.list_ports
import time

VID = 0x0F0D
PID = 0x0092


class ConfigApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Chuni245Tof Keyboard Config")
        self.root.geometry("600x800")
        self.root.resizable(False, False)

        self.ser = None
        self.setup_ui()
        self.root.after(100, self.refresh_ports)

    def setup_ui(self):
        # Port selection
        port_frame = ttk.LabelFrame(self.root, text="Serial Port", padding=10)
        port_frame.pack(fill='x', padx=10, pady=5)

        self.port_combo = ttk.Combobox(port_frame, width=30)
        self.port_combo.pack(side='left', padx=5)

        ttk.Button(port_frame, text="Refresh", command=self.refresh_ports).pack(side='left', padx=2)
        ttk.Button(port_frame, text="Connect", command=self.connect).pack(side='left', padx=2)

        # Keyboard mapping info
        map_frame = ttk.LabelFrame(self.root, text="Keyboard Mapping", padding=10)
        map_frame.pack(fill='x', padx=10, pady=5)

        mapping_text = """Slider Keys (32):
  Cell 1-8:   Q W E R T Y U I
  Cell 9-20:  O P [ ] A S D F G H J K
  Cell 21-32: L ; ' \\ Z X C V B N M ,

Air Keys (6):
  1-6:   4 5 6 7 8 9"""

        ttk.Label(map_frame, text=mapping_text, justify='left').pack(anchor='w')

        # MPR121 Touch Threshold Settings
        mpr_frame = ttk.LabelFrame(self.root, text="MPR121 Touch Threshold Settings", padding=10)
        mpr_frame.pack(fill='x', padx=10, pady=5)

        ttk.Label(mpr_frame, text="Touch Threshold:").grid(row=0, column=0, sticky='w')
        self.touch_thr_var = tk.StringVar(value="20")
        touch_entry = ttk.Entry(mpr_frame, textvariable=self.touch_thr_var, width=10)
        touch_entry.grid(row=0, column=1, padx=5)
        ttk.Label(mpr_frame, text="(默认: 20, 范围: 5-30)").grid(row=0, column=2, sticky='w')

        ttk.Label(mpr_frame, text="Release Threshold:").grid(row=1, column=0, sticky='w', pady=5)
        self.release_thr_var = tk.StringVar(value="18")
        release_entry = ttk.Entry(mpr_frame, textvariable=self.release_thr_var, width=10)
        release_entry.grid(row=1, column=1, padx=5)
        ttk.Label(mpr_frame, text="(默认: 18, 范围: 1-25)").grid(row=1, column=2, sticky='w')

        # Help text
        help_text = """⚠️ MPR121 阈值机制:
  • Touch Threshold: 越低 → 越容易触发触摸
  • Release Threshold: 越高 → 越容易释放!
  • 推荐值: Touch=20, Release=18 (差值2)
  • 高阈值 = 抗噪声能力强，更稳定"""
        ttk.Label(mpr_frame, text=help_text, justify='left', foreground='gray').grid(row=2, column=0, columnspan=3, sticky='w', pady=5)

        # TOF settings
        tof_frame = ttk.LabelFrame(self.root, text="TOF Air Settings (mm)", padding=10)
        tof_frame.pack(fill='x', padx=10, pady=5)

        ttk.Label(tof_frame, text="Start Height (offset):").grid(row=0, column=0, sticky='w')
        self.tof_offset_var = tk.StringVar(value="120")
        ttk.Entry(tof_frame, textvariable=self.tof_offset_var, width=10).grid(row=0, column=1, padx=5)
        ttk.Label(tof_frame, text="(默认: 120mm)").grid(row=0, column=2, sticky='w')

        ttk.Label(tof_frame, text="Step Height (pitch):").grid(row=1, column=0, sticky='w', pady=5)
        self.tof_pitch_var = tk.StringVar(value="40")
        ttk.Entry(tof_frame, textvariable=self.tof_pitch_var, width=10).grid(row=1, column=1, padx=5)
        ttk.Label(tof_frame, text="(默认: 40mm, Air1-5)").grid(row=1, column=2, sticky='w')

        ttk.Label(tof_frame, text="Air6 Range:").grid(row=2, column=0, sticky='w', pady=5)
        self.air6_range_var = tk.StringVar(value="150")
        ttk.Entry(tof_frame, textvariable=self.air6_range_var, width=10).grid(row=2, column=1, padx=5)
        ttk.Label(tof_frame, text="(默认: 150mm, >= pitch)").grid(row=2, column=2, sticky='w')

        ttk.Label(tof_frame, text="Min Hold Time:").grid(row=3, column=0, sticky='w', pady=5)
        self.air_hold_var = tk.StringVar(value="50")
        ttk.Entry(tof_frame, textvariable=self.air_hold_var, width=10).grid(row=3, column=1, padx=5)
        ttk.Label(tof_frame, text="(默认: 50ms, 10-500)").grid(row=3, column=2, sticky='w')

        # TOF help text
        tof_help_text = """⚠️ 注意：
• Offset 不建议低于 100mm，否则会导致 Air1 持续触发
• Pitch 不建议低于 30mm，否则抬手过猛可能直接穿过 Air 区域
  （TOF 轮询周期较长，可能漏检快速抬手）
• Offset 越大，触发起始高度越高
• Air6 Range 控制最高检测范围
• Min Hold Time 防止快速抬手时按键闪烁"""
        ttk.Label(tof_frame, text=tof_help_text, justify='left', foreground='gray').grid(row=4, column=0, columnspan=3, sticky='w', pady=5)

        # Buttons
        btn_frame = ttk.Frame(self.root, padding=10)
        btn_frame.pack(fill='x')

        ttk.Button(btn_frame, text="Read Config", command=self.read_config).pack(side='left', padx=5)
        ttk.Button(btn_frame, text="Apply Config", command=self.apply_config).pack(side='left', padx=5)
        ttk.Button(btn_frame, text="Save to Flash", command=self.save_config).pack(side='left', padx=5)
        ttk.Button(btn_frame, text="Restore Defaults", command=self.restore_defaults).pack(side='left', padx=5)
        ttk.Button(btn_frame, text="Test Status", command=self.test_status).pack(side='left', padx=5)
        ttk.Button(btn_frame, text="Reboot Bootloader", command=self.reboot_bootloader).pack(side='left', padx=5)

        # Log
        log_frame = ttk.LabelFrame(self.root, text="Log", padding=5)
        log_frame.pack(fill='both', expand=True, padx=10, pady=5)

        self.log_text = tk.Text(log_frame, height=12, width=65, state='disabled')
        self.log_text.pack(fill='both', expand=True)

    def log(self, msg):
        self.log_text.config(state='normal')
        self.log_text.insert('end', msg + '\n')
        self.log_text.see('end')
        self.log_text.config(state='disabled')
        self.root.update()

    def refresh_ports(self):
        ports = []
        for port in serial.tools.list_ports.comports():
            if port.vid == VID and port.pid == PID:
                ports.append(f"{port.device} (Chuni245Tof)")
                self.log(f"Found: {port.device}")
            else:
                ports.append(port.device)

        self.port_combo['values'] = ports
        if ports:
            for i, p in enumerate(ports):
                if "Chuni245Tof" in p:
                    self.port_combo.current(i)
                    break
            else:
                self.port_combo.current(0)

    def connect(self):
        if self.ser:
            self.ser.close()
            self.ser = None

        port = self.port_combo.get().split(' ')[0]

        try:
            self.ser = serial.Serial(port, 115200, timeout=1.0, write_timeout=1.0)
            self.log(f"Connected: {port}")
        except Exception as e:
            self.log(f"Failed: {e}")

    def send_cmd(self, cmd):
        if not self.ser:
            self.log("Not connected")
            return ""
        try:
            self.ser.reset_input_buffer()
            self.ser.write((cmd + '\r\n').encode())
            self.ser.flush()
            time.sleep(0.5)  # Wait for response

            # Read all available data
            response = ""
            start_time = time.time()
            while (time.time() - start_time) < 1.0:  # Max 1 second wait
                if self.ser.in_waiting:
                    data = self.ser.read(self.ser.in_waiting)
                    response += data.decode(errors='ignore')
                    if '\n' in response:  # Got a complete line
                        break
                time.sleep(0.05)

            self.log(f">> {cmd}")
            if response.strip():
                self.log(f"<< {response.strip()}")
            return response.strip()
        except Exception as e:
            self.log(f"Error: {e}")
            return ""

    def read_config(self):
        """Read current configuration from device"""
        response = self.send_cmd("CONFIG?")

        # Parse response: CONFIG touch=X release=Y offset=Z pitch=W air6=R hold=H
        if response:
            try:
                parts = response.split()
                if len(parts) >= 7 and parts[0] == "CONFIG":
                    # Parse key=value pairs
                    for part in parts[1:]:
                        if '=' in part:
                            key, value = part.split('=')
                            if key == "touch":
                                self.touch_thr_var.set(value)
                            elif key == "release":
                                self.release_thr_var.set(value)
                            elif key == "offset":
                                self.tof_offset_var.set(value)
                            elif key == "pitch":
                                self.tof_pitch_var.set(value)
                            elif key == "air6":
                                self.air6_range_var.set(value)
                            elif key == "hold":
                                self.air_hold_var.set(value)
                    self.log("Config loaded from device")
            except Exception as e:
                self.log(f"Parse error: {e}")

    def apply_config(self):
        """Apply threshold settings to device"""
        try:
            touch = int(self.touch_thr_var.get())
            release = int(self.release_thr_var.get())
            offset = int(self.tof_offset_var.get())
            pitch = int(self.tof_pitch_var.get())
            air6_range = int(self.air6_range_var.get())
            air_hold = int(self.air_hold_var.get())

            # Validate ranges
            if not (5 <= touch <= 30):
                messagebox.showerror("Error", "Touch threshold must be 5-30")
                return
            if not (1 <= release <= 25):
                messagebox.showerror("Error", "Release threshold must be 1-25")
                return
            # Touch 应该 > Release
            if touch <= release:
                messagebox.showwarning("Warning",
                    f"Touch ({touch}) should be > Release ({release})\n"
                    f"推荐值: Touch=20, Release=18")

            # Offset 不建议低于 100mm
            if offset < 100:
                messagebox.showwarning("Warning",
                    f"Offset ({offset}mm) 不建议低于 100mm\n"
                    f"这可能导致 Air1 持续触发")

            if not (40 <= offset <= 200):
                messagebox.showerror("Error", "TOF offset must be 40-200")
                return
            if not (4 <= pitch <= 100):
                messagebox.showerror("Error", "TOF pitch must be 4-100")
                return
            if not (pitch <= air6_range <= 200):
                messagebox.showerror("Error", f"Air6 range must be {pitch}-200 (>= pitch)")
                return
            if not (10 <= air_hold <= 500):
                messagebox.showerror("Error", "Min hold time must be 10-500ms")
                return

            cmd = f"CONFIG {touch} {release} {offset} {pitch} {air6_range} {air_hold}"
            self.send_cmd(cmd)

        except ValueError:
            messagebox.showerror("Error", "Invalid number format")

    def save_config(self):
        """Save current config to flash"""
        if messagebox.askyesno("Confirm", "Save current configuration to flash?\nThis will persist after reboot."):
            self.send_cmd("SAVE")

    def restore_defaults(self):
        """Restore default configuration"""
        if messagebox.askyesno("Confirm", "Restore default values?\n(touch=20, release=18, offset=120, pitch=30, air6=150, hold=50)"):
            self.send_cmd("DEFAULT")
            # Update UI with defaults
            self.touch_thr_var.set("20")
            self.release_thr_var.set("18")
            self.tof_offset_var.set("120")
            self.tof_pitch_var.set("30")
            self.air6_range_var.set("150")
            self.air_hold_var.set("50")

    def test_status(self):
        self.send_cmd("STATUS")

    def reboot_bootloader(self):
        if messagebox.askyesno("Confirm", "Reboot to bootloader?"):
            self.send_cmd("BOOTLOADER")
            self.log("Rebooting...")
            self.root.quit()


def main():
    root = tk.Tk()
    app = ConfigApp(root)
    root.mainloop()


if __name__ == '__main__':
    main()