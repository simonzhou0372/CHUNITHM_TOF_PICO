#!/usr/bin/env python3
"""
Chuni245Tof Configuration and Monitoring Tool
Keyboard-only mode with real-time performance monitoring and visualization

Optimized GUI with fixed layout - no horizontal jumping or flickering
"""

import tkinter as tk
from tkinter import ttk, messagebox
import serial
import serial.tools.list_ports
import time
import threading
import json
import re

VID = 0x0F0D
PID = 0x0092


class ConfigApp:
    def __init__(self, root):
        self.root = root
        self.root.title("Chuni245Tof Config & Monitor")
        # Compact window size - no excessive whitespace
        self.root.geometry("600x720")
        self.root.resizable(False, False)

        self.ser = None
        self.monitoring = False
        self.monitor_thread = None

        # Performance data
        self.perf_data = {
            'loop_avg_us': 0,
            'loop_max_us': 0,
            'hid_sends': 0,
            'tof_new_data': 0,
            'tof_poll_avg_us': 0,
            'tof_poll_max_us': 0
        }

        # AIR debug data
        self.air_data = {
            'max_distance': 0,
            'sensor_bitmap': 0,
            'hid_bitmap': 0,
            'sensors': [{'dist': 0, 'age': 0, 'valid': False} for _ in range(5)]
        }

        # Slider state (32 cells)
        self.slider_state = 0x00000000

        # TOF visualization parameters
        self.tof_offset = 120
        self.tof_pitch = 40
        self.air6_range = 150

        # Canvas drawing items - created once, updated with coords
        self.tof_zone_items = []      # Background zone rectangles
        self.tof_bar_items = []       # Current height bars
        self.tof_marker_items = []    # Scale markers
        self.tof_canvases = []        # Canvas references

        self.setup_ui()
        self.root.after(100, self.refresh_ports)

    def setup_ui(self):
        # ===== Top: Configuration =====
        config_frame = ttk.LabelFrame(self.root, text="Configuration", padding=5)
        config_frame.pack(fill='x', padx=5, pady=5)

        # Port selection - compact layout
        port_frame = ttk.Frame(config_frame)
        port_frame.pack(fill='x', pady=3)

        ttk.Label(port_frame, text="Port:").pack(side='left', padx=2)
        self.port_combo = ttk.Combobox(port_frame, width=20)
        self.port_combo.pack(side='left', padx=2)
        ttk.Button(port_frame, text="Refresh", command=self.refresh_ports, width=7).pack(side='left', padx=1)
        ttk.Button(port_frame, text="Connect", command=self.connect, width=7).pack(side='left', padx=1)

        ttk.Label(port_frame, text="  Monitor:").pack(side='left', padx=2)
        self.monitor_btn = ttk.Button(port_frame, text="Start", command=self.toggle_monitoring, width=6)
        self.monitor_btn.pack(side='left', padx=1)
        ttk.Label(port_frame, text="Interval:").pack(side='left', padx=2)
        self.monitor_interval_var = tk.StringVar(value="16")
        ttk.Entry(port_frame, textvariable=self.monitor_interval_var, width=5).pack(side='left', padx=1)
        ttk.Label(port_frame, text="ms").pack(side='left')

        # Config parameters - compact grid layout
        params_frame = ttk.Frame(config_frame)
        params_frame.pack(fill='x', pady=3)

        # MPR121 Settings - fixed width labels
        mpr_frame = ttk.LabelFrame(params_frame, text="MPR121 Touch", padding=3)
        mpr_frame.pack(side='left', fill='both', padx=3)

        ttk.Label(mpr_frame, text="Touch:").grid(row=0, column=0, sticky='w')
        self.touch_thr_var = tk.StringVar(value="20")
        ttk.Entry(mpr_frame, textvariable=self.touch_thr_var, width=5).grid(row=0, column=1, padx=2)
        ttk.Label(mpr_frame, text="(5-30)", width=6).grid(row=0, column=2, padx=2)

        ttk.Label(mpr_frame, text="Release:").grid(row=1, column=0, sticky='w')
        self.release_thr_var = tk.StringVar(value="18")
        ttk.Entry(mpr_frame, textvariable=self.release_thr_var, width=5).grid(row=1, column=1, padx=2)
        ttk.Label(mpr_frame, text="(1-25)", width=6).grid(row=1, column=2, padx=2)

        help_text = "⚠️ Touch > Release, 推荐: T=20, R=18"
        ttk.Label(mpr_frame, text=help_text, foreground='gray', font=('Arial', 7)).grid(row=2, column=0, columnspan=3, sticky='w')

        # TOF Settings - fixed width labels
        tof_frame = ttk.LabelFrame(params_frame, text="TOF Air", padding=3)
        tof_frame.pack(side='left', fill='both', padx=3)

        ttk.Label(tof_frame, text="Offset:").grid(row=0, column=0, sticky='w')
        self.tof_offset_var = tk.StringVar(value="120")
        ttk.Entry(tof_frame, textvariable=self.tof_offset_var, width=5).grid(row=0, column=1, padx=2)
        ttk.Label(tof_frame, text="mm", width=4).grid(row=0, column=2)

        ttk.Label(tof_frame, text="Pitch:").grid(row=1, column=0, sticky='w')
        self.tof_pitch_var = tk.StringVar(value="40")
        ttk.Entry(tof_frame, textvariable=self.tof_pitch_var, width=5).grid(row=1, column=1, padx=2)
        ttk.Label(tof_frame, text="mm", width=4).grid(row=1, column=2)

        ttk.Label(tof_frame, text="Air6 Range:").grid(row=2, column=0, sticky='w')
        self.air6_range_var = tk.StringVar(value="150")
        ttk.Entry(tof_frame, textvariable=self.air6_range_var, width=5).grid(row=2, column=1, padx=2)
        ttk.Label(tof_frame, text="mm", width=4).grid(row=2, column=2)

        ttk.Label(tof_frame, text="Min Hold:").grid(row=3, column=0, sticky='w')
        self.air_hold_var = tk.StringVar(value="50")
        ttk.Entry(tof_frame, textvariable=self.air_hold_var, width=5).grid(row=3, column=1, padx=2)
        ttk.Label(tof_frame, text="ms", width=4).grid(row=3, column=2)

        tof_help = "⚠️ Offset≥100, Pitch≥30"
        ttk.Label(tof_frame, text=tof_help, foreground='gray', font=('Arial', 7)).grid(row=4, column=0, columnspan=3, sticky='w')

        # Buttons - wider to prevent text truncation
        btn_frame = ttk.LabelFrame(params_frame, text="Actions", padding=3)
        btn_frame.pack(side='left', fill='both', padx=3)

        ttk.Button(btn_frame, text="Read Config", command=self.read_config, width=16).pack(pady=1)
        ttk.Button(btn_frame, text="Apply Config", command=self.apply_config, width=16).pack(pady=1)
        ttk.Button(btn_frame, text="Save to Flash", command=self.save_config, width=16).pack(pady=1)
        ttk.Button(btn_frame, text="Restore Defaults", command=self.restore_defaults, width=16).pack(pady=1)

        # ===== Visualization - COMPACT =====
        viz_frame = ttk.Frame(self.root)
        viz_frame.pack(fill='x', padx=5, pady=5)

        # Main layout: TOF (left) | AIR (right)
        main_grid = ttk.Frame(viz_frame)
        main_grid.pack(fill='x', expand=False)

        # Row 0: TOF + AIR (side by side)
        tof_air_row = ttk.Frame(main_grid)
        tof_air_row.grid(row=0, column=0, sticky='nsew', padx=2, pady=2)

        # ===== Left: TOF Distance - FIXED GRID LAYOUT =====
        tof_container = ttk.Frame(tof_air_row)
        tof_container.pack(side='left', fill='both', padx=2)

        ttk.Label(tof_container, text="TOF Distance", font=('Arial', 9, 'bold')).pack()

        # FIXED GRID for TOF bars - prevents horizontal jumping
        tof_bars_frame = ttk.Frame(tof_container)
        tof_bars_frame.pack(pady=2)

        self.tof_height_labels = []
        self.tof_age_labels = []

        for i in range(5):
            # Create frame with FIXED width
            tof_frame = ttk.Frame(tof_bars_frame, width=52)
            tof_frame.grid(row=0, column=i, padx=2, sticky='n')
            tof_frame.grid_propagate(False)  # Prevent resize from children

            # Label - FIXED width
            ttk.Label(tof_frame, text=f"TOF{i+1}", font=('Arial', 8), width=6, anchor='center').pack()

            # Canvas: FIXED SIZE 28x160 - never changes
            canvas = tk.Canvas(tof_frame, width=28, height=160, bg='#f0f0f0',
                             highlightthickness=1, highlightbackground='#999999')
            canvas.pack()
            self.tof_canvases.append(canvas)

            # Distance label - FIXED width (accommodate "2058 mm")
            dist_label = ttk.Label(tof_frame, text="0 mm", font=('Arial', 7), width=8, anchor='center')
            dist_label.pack()
            self.tof_height_labels.append(dist_label)

            # Age label - FIXED width (accommodate "age: 188ms")
            age_label = ttk.Label(tof_frame, text="age: -", font=('Arial', 7), foreground='gray', width=9, anchor='center')
            age_label.pack()
            self.tof_age_labels.append(age_label)

        # ===== Middle: AIR Keys - FIXED GRID LAYOUT =====
        air_container = ttk.Frame(tof_air_row)
        air_container.pack(side='left', fill='both', padx=5)

        ttk.Label(air_container, text="AIR Keys", font=('Arial', 9, 'bold')).pack()

        # Max distance label - FIXED width
        self.max_dist_label = ttk.Label(air_container, text="Max: - mm", font=('Arial', 8),
                                        foreground='blue', width=12, anchor='center')
        self.max_dist_label.pack(pady=2)

        self.air_indicators = []
        air_bars_frame = ttk.Frame(air_container)
        air_bars_frame.pack(pady=1)

        # Air6 on top, Air1 on bottom - FIXED GRID
        for i in range(5, -1, -1):  # 6, 5, 4, 3, 2, 1
            air_frame = ttk.Frame(air_bars_frame, width=60)
            air_frame.grid(row=5-i, column=0, pady=0, sticky='w')
            air_frame.grid_propagate(False)

            # Label - FIXED width
            ttk.Label(air_frame, text=f"Air{i+1}", font=('Arial', 8), width=5, anchor='e').pack(side='left', padx=2)

            # Canvas: FIXED SIZE 22x16
            canvas = tk.Canvas(air_frame, width=22, height=16, bg='#e0e0e0',
                             highlightthickness=1, highlightbackground='#999999')
            canvas.pack(side='left')
            self.air_indicators.insert(0, canvas)

        # ===== Right: Performance Statistics =====
        perf_container = ttk.Frame(tof_air_row)
        perf_container.pack(side='left', fill='both', padx=5)

        perf_frame = ttk.LabelFrame(perf_container, text="Performance", padding=3)
        perf_frame.pack()

        self.perf_labels = {}
        perf_items = [
            ("Loop Avg:", "loop_avg_us", "μs"),
            ("Loop Max:", "loop_max_us", "μs"),
            ("HID Sends:", "hid_sends", ""),
            ("TOF New:", "tof_new_data", ""),
            ("Poll Avg:", "tof_poll_avg_us", "μs"),
            ("Poll Max:", "tof_poll_max_us", "μs")
        ]

        for i, (label_text, key, unit) in enumerate(perf_items):
            ttk.Label(perf_frame, text=label_text).grid(row=i, column=0, sticky='w')
            self.perf_labels[key] = ttk.Label(perf_frame, text="0", foreground='blue', width=5, anchor='e')
            self.perf_labels[key].grid(row=i, column=1, sticky='e', padx=2)
            ttk.Label(perf_frame, text=unit, width=3).grid(row=i, column=2, sticky='w')

        # Row 1: Touch Slider - COMPACT
        touch_container = ttk.Frame(main_grid)
        touch_container.grid(row=1, column=0, sticky='nsew', padx=2, pady=2)

        ttk.Label(touch_container, text="Touch Slider", font=('Arial', 9, 'bold')).pack()

        self.touch_indicators = {}
        touch_bars_frame = ttk.Frame(touch_container)
        touch_bars_frame.pack(pady=2)

        # Top row: 31 29 27 ... 3 1
        top_row = ttk.Frame(touch_bars_frame)
        top_row.pack()
        for col in range(15, -1, -1):
            cell_num = col * 2 + 1
            cell_frame = ttk.Frame(top_row)
            cell_frame.pack(side='left', padx=0)
            canvas = tk.Canvas(cell_frame, width=16, height=16, bg='#e0e0e0',
                             highlightthickness=1, highlightbackground='#999999')
            canvas.pack()
            self.touch_indicators[cell_num] = canvas

        # Bottom row: 32 30 28 ... 4 2
        bottom_row = ttk.Frame(touch_bars_frame)
        bottom_row.pack()
        for col in range(15, -1, -1):
            cell_num = col * 2 + 2
            cell_frame = ttk.Frame(bottom_row)
            cell_frame.pack(side='left', padx=0)
            canvas = tk.Canvas(cell_frame, width=16, height=16, bg='#e0e0e0',
                             highlightthickness=1, highlightbackground='#999999')
            canvas.pack()
            self.touch_indicators[cell_num] = canvas

        # ===== Log - COMPACT =====
        log_frame = ttk.LabelFrame(self.root, text="Log", padding=5)
        log_frame.pack(fill='x', padx=5, pady=5)

        # Reduced width to match overall window width
        self.log_text = tk.Text(log_frame, height=6, width=85, state='disabled',
                                font=('Consolas', 8), wrap='word')
        self.log_text.pack(fill='x')

        log_scrollbar = ttk.Scrollbar(log_frame, orient='vertical', command=self.log_text.yview)
        log_scrollbar.pack(side='right', fill='y')
        self.log_text.configure(yscrollcommand=log_scrollbar.set)

        # Initialize visualization
        self.update_tof_ranges()

    def update_tof_ranges(self):
        """Update TOF visualization ranges based on config"""
        try:
            self.tof_offset = int(self.tof_offset_var.get())
            self.tof_pitch = int(self.tof_pitch_var.get())
            self.air6_range = int(self.air6_range_var.get())
            self.create_tof_visualization()
        except ValueError:
            pass

    def create_tof_visualization(self):
        """
        Create TOF visualization objects ONCE - never delete during updates
        Only update coords/positions when data changes
        """
        max_height = self.tof_offset + self.tof_pitch * 5 + self.air6_range

        # Clear previous items (only called when parameters change, not during monitoring)
        self.tof_zone_items = []
        self.tof_bar_items = []
        self.tof_marker_items = []

        width = 28
        height = 160

        if max_height <= 0:
            return

        scale = height / max_height

        for i, canvas in enumerate(self.tof_canvases):
            # Clear canvas ONCE during initialization
            canvas.delete("all")

            # Draw AIR zones (background - fixed for given parameters)
            # Air6 (topmost) - Light Coral
            air6_bottom = max_height
            air6_top = self.tof_offset + self.tof_pitch * 5
            y6_bottom = int(height - air6_bottom * scale)
            y6_top = int(height - air6_top * scale)
            y6_bottom = max(0, min(height, y6_bottom))
            y6_top = max(0, min(height, y6_top))
            zone_item = canvas.create_rectangle(0, y6_top, width, y6_bottom,
                                                fill='#FFB6C1', outline='', width=0)
            self.tof_zone_items.append([zone_item])

            # Air1-5 - Light Blue
            colors = ['#ADD8E6', '#ADD8E6', '#ADD8E6', '#ADD8E6', '#ADD8E6']
            for j in range(4, -1, -1):
                air_bottom = self.tof_offset + self.tof_pitch * j
                air_top = self.tof_offset + self.tof_pitch * (j + 1)
                y_bottom = int(height - air_bottom * scale)
                y_top = int(height - air_top * scale)
                y_bottom = max(0, min(height, y_bottom))
                y_top = max(0, min(height, y_top))
                zone_item = canvas.create_rectangle(0, y_top, width, y_bottom,
                                                    fill=colors[j], outline='', width=0)
                self.tof_zone_items[i].append(zone_item)

            # Create height bar - INITIALLY HIDDEN (at bottom)
            bar_item = canvas.create_rectangle(0, height, width, height,
                                               fill='#4169E1', outline='', width=0)
            self.tof_bar_items.append(bar_item)

            # Create scale markers
            markers = []
            for mm in range(0, max_height + 1, 100):
                y = int(height - mm * scale)
                if 0 <= y <= height:
                    marker = canvas.create_line(0, y, 2, y, fill='gray', width=1)
                    markers.append(marker)
            self.tof_marker_items.append(markers)

    def update_tof_visualization(self):
        """
        Update TOF height bars - ONLY update coords, never delete/create
        Called during monitoring - fast and flicker-free
        """
        max_height = self.tof_offset + self.tof_pitch * 5 + self.air6_range

        width = 28
        height = 160

        if max_height <= 0:
            return

        scale = height / max_height

        for i in range(5):
            canvas = self.tof_canvases[i]

            if i < len(self.air_data['sensors']):
                dist = self.air_data['sensors'][i]['dist']
                valid = self.air_data['sensors'][i]['valid']

                # Skip invalid data (8190mm) - HIDE bar
                if valid and dist > 0 and dist < 8190 and dist <= max_height:
                    y_height = int(height - dist * scale)
                    y_height = max(0, min(height, y_height))
                    # Update bar position - ONLY Y changes
                    canvas.coords(self.tof_bar_items[i], 0, y_height, width, height)
                    canvas.itemconfig(self.tof_bar_items[i], state='normal')
                else:
                    # Hide bar for invalid data
                    canvas.itemconfig(self.tof_bar_items[i], state='hidden')

    def update_air_visualization(self):
        """Update AIR indicators + max distance display"""
        hid_bitmap = self.air_data['hid_bitmap']

        for i in range(6):
            canvas = self.air_indicators[i]

            if hid_bitmap & (1 << i):
                canvas.configure(bg='#4169E1')  # Royal Blue
            else:
                canvas.configure(bg='#e0e0e0')  # Light Gray

        # Update max distance label
        max_dist = 0
        for sensor in self.air_data['sensors']:
            if sensor['valid'] and sensor['dist'] < 8190:
                max_dist = max(max_dist, sensor['dist'])
        self.max_dist_label.config(text=f"Max: {max_dist} mm")

    def update_touch_visualization(self):
        """Update Touch indicators"""
        for cell_num in range(1, 33):
            canvas = self.touch_indicators[cell_num]

            if self.slider_state & (1 << (cell_num - 1)):
                canvas.configure(bg='#20B2AA')  # Light Sea Green
            else:
                canvas.configure(bg='#e0e0e0')  # Light Gray

    def log(self, msg):
        self.log_text.config(state='normal')
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.insert('end', f"[{timestamp}] {msg}\n")
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
            self.log("Disconnected")
            self.monitor_btn.config(text="Start")
            self.monitoring = False

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
            time.sleep(0.3)

            response = ""
            start_time = time.time()
            while (time.time() - start_time) < 2.0:
                if self.ser.in_waiting:
                    data = self.ser.read(self.ser.in_waiting)
                    response += data.decode(errors='ignore')
                    if '\n' in response:
                        break
                time.sleep(0.05)

            self.log(f">> {cmd}")
            if response.strip():
                # Truncate long responses in log
                log_response = response.strip()
                if len(log_response) > 150:
                    log_response = log_response[:150] + "..."
                self.log(f"<< {log_response}")
            return response.strip()
        except Exception as e:
            self.log(f"Error: {e}")
            return ""

    def read_config(self):
        response = self.send_cmd("CONFIG?")
        if response:
            try:
                parts = response.split()
                if len(parts) >= 7 and parts[0] == "CONFIG":
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
                    self.update_tof_ranges()
            except Exception as e:
                self.log(f"Parse error: {e}")

    def apply_config(self):
        try:
            touch = int(self.touch_thr_var.get())
            release = int(self.release_thr_var.get())
            offset = int(self.tof_offset_var.get())
            pitch = int(self.tof_pitch_var.get())
            air6_range = int(self.air6_range_var.get())
            air_hold = int(self.air_hold_var.get())

            # Validate
            if not (5 <= touch <= 30):
                messagebox.showerror("Error", "Touch: 5-30")
                return
            if not (1 <= release <= 25):
                messagebox.showerror("Error", "Release: 1-25")
                return
            if not (40 <= offset <= 200):
                messagebox.showerror("Error", "Offset: 40-200")
                return
            if not (4 <= pitch <= 100):
                messagebox.showerror("Error", "Pitch: 4-100")
                return
            if not (pitch <= air6_range <= 200):
                messagebox.showerror("Error", f"Air6: {pitch}-200")
                return
            if not (10 <= air_hold <= 500):
                messagebox.showerror("Error", "Hold: 10-500")
                return

            cmd = f"CONFIG {touch} {release} {offset} {pitch} {air6_range} {air_hold}"
            self.send_cmd(cmd)
            self.update_tof_ranges()

        except ValueError:
            messagebox.showerror("Error", "Invalid number")

    def save_config(self):
        if messagebox.askyesno("Confirm", "Save to flash?"):
            self.send_cmd("SAVE")
            self.update_tof_ranges()

    def restore_defaults(self):
        if messagebox.askyesno("Confirm", "Restore defaults?"):
            self.send_cmd("DEFAULT")
            self.touch_thr_var.set("20")
            self.release_thr_var.set("18")
            self.tof_offset_var.set("120")
            self.tof_pitch_var.set("40")
            self.air6_range_var.set("150")
            self.air_hold_var.set("50")
            self.update_tof_ranges()

    def toggle_monitoring(self):
        if self.monitoring:
            self.monitoring = False
            self.send_cmd("STOP_MONITOR")
            self.monitor_btn.config(text="Start")
            self.log("Monitoring stopped")
        else:
            if not self.ser:
                messagebox.showwarning("Warning", "Connect first")
                return

            try:
                interval = int(self.monitor_interval_var.get())
                interval = max(50, min(5000, interval))

                self.send_cmd(f"START_MONITOR {interval}")
                self.monitoring = True
                self.monitor_btn.config(text="Stop")
                self.log(f"Monitoring started ({interval}ms)")

                self.monitor_thread = threading.Thread(target=self.monitor_receive_loop, daemon=True)
                self.monitor_thread.start()
            except ValueError:
                messagebox.showerror("Error", "Invalid interval")

    def monitor_receive_loop(self):
        while self.monitoring:
            try:
                if self.ser and self.ser.is_open:
                    if self.ser.in_waiting:
                        # Read all available data
                        response = ""
                        while self.ser.in_waiting:
                            data = self.ser.read(self.ser.in_waiting)
                            response += data.decode(errors='ignore')
                            time.sleep(0.01)

                        # Split by '---' and process each JSON separately
                        if '---' in response:
                            json_blocks = response.split('---')
                            for json_block in json_blocks:
                                json_block = json_block.strip()
                                if json_block:
                                    self.root.after(0, lambda jb=json_block: self.parse_monitor_data(jb))
                    else:
                        time.sleep(0.01)
                else:
                    break
            except Exception as e:
                self.root.after(0, lambda: self.log(f"Monitor error: {e}"))
                break

    def parse_monitor_data(self, json_str):
        """Parse a single JSON data block"""
        try:
            # Clean JSON string
            json_str = json_str.strip()
            if not json_str:
                return

            # Find JSON object boundaries
            start_idx = json_str.find('{')
            end_idx = json_str.rfind('}')

            if start_idx == -1 or end_idx == -1:
                return

            json_str = json_str[start_idx:end_idx+1]

            data = json.loads(json_str)

            # Update Slider state
            if 'slider' in data:
                self.slider_state = data['slider']
                self.update_touch_visualization()

            # Update AIR state
            if 'air' in data:
                air = data['air']
                self.air_data['hid_bitmap'] = air.get('hid', 0)
                self.update_air_visualization()

            # Update TOF data (skip 8190mm)
            if 'tof' in data:
                tof_list = data['tof']
                for i, tof_data in enumerate(tof_list):
                    if i < 5:
                        dist = tof_data.get('d', 0)
                        age = tof_data.get('a', 0)
                        valid = tof_data.get('v', 0) == 1

                        # Skip invalid data (8190mm)
                        if dist >= 8190:
                            # Show stale age
                            if hasattr(self, 'tof_age_labels'):
                                self.tof_age_labels[i].config(text="age: --", foreground='red')
                            continue

                        self.air_data['sensors'][i]['dist'] = dist
                        self.air_data['sensors'][i]['age'] = age
                        self.air_data['sensors'][i]['valid'] = valid

                        self.tof_height_labels[i].config(text=f"{dist} mm")

                        # Update age label
                        if hasattr(self, 'tof_age_labels'):
                            age_text = f"age: {age}ms"
                            if not valid:
                                age_text = "age: N/A"
                                self.tof_age_labels[i].config(foreground='red')
                            elif age > 200:
                                self.tof_age_labels[i].config(foreground='red')
                            else:
                                self.tof_age_labels[i].config(foreground='gray')
                            self.tof_age_labels[i].config(text=age_text)

                # FAST update - only coords change, no delete/create
                self.update_tof_visualization()

            # Update performance
            if 'perf' in data:
                perf = data['perf']
                for key in self.perf_labels.keys():
                    if key in perf:
                        self.perf_data[key] = perf[key]
                        self.perf_labels[key].config(text=str(perf[key]))

        except json.JSONDecodeError as e:
            # Only log first error, not all of them
            pass
        except Exception as e:
            pass


def main():
    root = tk.Tk()
    app = ConfigApp(root)
    root.mainloop()


if __name__ == '__main__':
    main()