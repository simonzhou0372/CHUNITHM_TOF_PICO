#!/usr/bin/env python3
"""
Chuni245Tof Configuration and Monitoring Tool
Keyboard-only mode with real-time performance monitoring and visualization

Features:
- Multi-language support (Chinese/English)
- Auto language detection based on system locale
- Language switch without layout changes
"""

import tkinter as tk
from tkinter import ttk, messagebox
import serial
import serial.tools.list_ports
import time
import threading
import json
import re
import locale
import os

VID = 0x0F0D
PID = 0x0092

# ============================================================================
# Multi-language Support
# ============================================================================

class LanguageManager:
    """多语言管理器"""

    # 语言代码
    LANG_CN = 'zh_CN'  # 中文
    LANG_EN = 'en_US'  # 英文

    def __init__(self):
        self.current_lang = self.detect_system_language()
        self.translations = self._load_translations()

    def detect_system_language(self):
        """检测系统语言"""
        try:
            # Windows
            import ctypes
            windll = ctypes.windll.kernel32
            lang_code = windll.GetUserDefaultUILanguage()
            # 转换为标准格式
            if lang_code in [0x0804, 0x0004]:  # 简体中文
                return self.LANG_CN
            else:
                return self.LANG_EN
        except:
            # 非 Windows 或失败，使用 locale
            try:
                sys_lang = locale.getdefaultlocale()[0]
                if sys_lang and sys_lang.startswith('zh'):
                    return self.LANG_CN
                else:
                    return self.LANG_EN
            except:
                return self.LANG_EN

    def _load_translations(self):
        """加载翻译字典"""
        return {
            # 窗口标题
            'title': {
                self.LANG_CN: "Chuni245Tof 配置工具",
                self.LANG_EN: "Chuni245Tof Config Tool"
            },

            # 配置框架
            'config_frame': {
                self.LANG_CN: "配置",
                self.LANG_EN: "Configuration"
            },

            # 端口
            'port': {
                self.LANG_CN: "端口:",
                self.LANG_EN: "Port:"
            },
            'refresh': {
                self.LANG_CN: "刷新",
                self.LANG_EN: "Refresh"
            },
            'connect': {
                self.LANG_CN: "连接",
                self.LANG_EN: "Connect"
            },
            'monitor': {
                self.LANG_CN: "监控:",
                self.LANG_EN: "Monitor:"
            },
            'start': {
                self.LANG_CN: "开始",
                self.LANG_EN: "Start"
            },
            'stop': {
                self.LANG_CN: "停止",
                self.LANG_EN: "Stop"
            },
            'interval': {
                self.LANG_CN: "间隔:",
                self.LANG_EN: "Interval:"
            },

            # MPR121 框架
            'mpr121_frame': {
                self.LANG_CN: "MPR121 触摸",
                self.LANG_EN: "MPR121 Touch"
            },
            'touch': {
                self.LANG_CN: "触摸:",
                self.LANG_EN: "Touch:"
            },
            'release': {
                self.LANG_CN: "释放:",
                self.LANG_EN: "Release:"
            },
            'mpr_help': {
                self.LANG_CN: "⚠️ 触摸 > 释放, 推荐: T=20, R=18",
                self.LANG_EN: "⚠️ Touch > Release, Recommend: T=20, R=18"
            },

            # TOF 框架
            'tof_frame': {
                self.LANG_CN: "TOF 空中",
                self.LANG_EN: "TOF Air"
            },
            'offset': {
                self.LANG_CN: "偏移:",
                self.LANG_EN: "Offset:"
            },
            'pitch': {
                self.LANG_CN: "级高:",
                self.LANG_EN: "Pitch:"
            },
            'air6_range': {
                self.LANG_CN: "Air6范围:",
                self.LANG_EN: "Air6 Range:"
            },
            'min_hold': {
                self.LANG_CN: "最小保持:",
                self.LANG_EN: "Min Hold:"
            },
            'tof_help': {
                self.LANG_CN: "⚠️ 偏移≥100, 级高≥30",
                self.LANG_EN: "⚠️ Offset≥100, Pitch≥30"
            },

            # 操作按钮
            'actions_frame': {
                self.LANG_CN: "操作",
                self.LANG_EN: "Actions"
            },
            'read_config': {
                self.LANG_CN: "读取配置",
                self.LANG_EN: "Read Config"
            },
            'apply_config': {
                self.LANG_CN: "应用配置",
                self.LANG_EN: "Apply Config"
            },
            'save_to_flash': {
                self.LANG_CN: "保存到Flash",
                self.LANG_EN: "Save to Flash"
            },
            'restore_defaults': {
                self.LANG_CN: "恢复默认",
                self.LANG_EN: "Restore Defaults"
            },

            # 可视化
            'tof_distance': {
                self.LANG_CN: "TOF 距离",
                self.LANG_EN: "TOF Distance"
            },
            'air_keys': {
                self.LANG_CN: "AIR 按键",
                self.LANG_EN: "AIR Keys"
            },
            'max_dist': {
                self.LANG_CN: "最大:",
                self.LANG_EN: "Max:"
            },
            'performance': {
                self.LANG_CN: "性能",
                self.LANG_EN: "Performance"
            },
            'loop_avg': {
                self.LANG_CN: "循环平均:",
                self.LANG_EN: "Loop Avg:"
            },
            'loop_max': {
                self.LANG_CN: "循环最大:",
                self.LANG_EN: "Loop Max:"
            },
            'hid_sends': {
                self.LANG_CN: "HID发送:",
                self.LANG_EN: "HID Sends:"
            },
            'tof_new': {
                self.LANG_CN: "TOF新数据:",
                self.LANG_EN: "TOF New:"
            },
            'poll_avg': {
                self.LANG_CN: "轮询平均:",
                self.LANG_EN: "Poll Avg:"
            },
            'poll_max': {
                self.LANG_CN: "轮询最大:",
                self.LANG_EN: "Poll Max:"
            },

            # 触摸滑块
            'touch_slider': {
                self.LANG_CN: "触摸滑块",
                self.LANG_EN: "Touch Slider"
            },

            # 日志
            'log_frame': {
                self.LANG_CN: "日志",
                self.LANG_EN: "Log"
            },

            # 语言切换
            'language': {
                self.LANG_CN: "语言:",
                self.LANG_EN: "Language:"
            },
            'chinese': {
                self.LANG_CN: "中文",
                self.LANG_EN: "中文"
            },
            'english': {
                self.LANG_CN: "English",
                self.LANG_EN: "English"
            },

            # 消息框
            'msg_save_confirm': {
                self.LANG_CN: "保存到 Flash?",
                self.LANG_EN: "Save to flash?"
            },
            'msg_save_success': {
                self.LANG_CN: "配置已成功保存到 Flash！",
                self.LANG_EN: "Configuration saved to flash!"
            },
            'msg_save_failed': {
                self.LANG_CN: "保存配置到 Flash 失败！\n\n请查看设备日志获取详细信息。",
                self.LANG_EN: "Failed to save configuration to flash!\n\nCheck device logs for details."
            },
            'msg_restore_confirm_title': {
                self.LANG_CN: "恢复默认值",
                self.LANG_EN: "Restore Defaults"
            },
            'msg_restore_confirm': {
                self.LANG_CN: "恢复默认配置？\n\n这将重置 RAM 为默认值。\n要保存默认值到 Flash，请之后点击 '保存到 Flash'。",
                self.LANG_EN: "Restore default configuration?\n\nThis will reset RAM to defaults.\nTo save defaults to flash, click 'Save to Flash' afterwards."
            },
            'msg_restore_success': {
                self.LANG_CN: "默认值已恢复到 RAM",
                self.LANG_EN: "Defaults restored to RAM"
            },
            'msg_save_note': {
                self.LANG_CN: "注意：使用 '保存到 Flash' 持久化默认值",
                self.LANG_EN: "NOTE: Use 'Save to Flash' to persist defaults"
            },
            'msg_not_connected': {
                self.LANG_CN: "未连接",
                self.LANG_EN: "Not connected"
            },
            'msg_connect_first': {
                self.LANG_CN: "请先连接",
                self.LANG_EN: "Connect first"
            },
            'msg_found': {
                self.LANG_CN: "找到:",
                self.LANG_EN: "Found:"
            },
            'msg_connected': {
                self.LANG_CN: "已连接:",
                self.LANG_EN: "Connected:"
            },
            'msg_disconnected': {
                self.LANG_CN: "已断开",
                self.LANG_EN: "Disconnected"
            },
            'msg_config_loaded': {
                self.LANG_CN: "配置已从设备加载",
                self.LANG_EN: "Config loaded from device"
            },
            'msg_monitoring_started': {
                self.LANG_CN: "监控已开始",
                self.LANG_EN: "Monitoring started"
            },
            'msg_monitoring_stopped': {
                self.LANG_CN: "监控已停止",
                self.LANG_EN: "Monitoring stopped"
            },

            # 错误消息
            'err_touch_range': {
                self.LANG_CN: "触摸: 5-30",
                self.LANG_EN: "Touch: 5-30"
            },
            'err_release_range': {
                self.LANG_CN: "释放: 1-25",
                self.LANG_EN: "Release: 1-25"
            },
            'err_offset_range': {
                self.LANG_CN: "偏移: 40-200",
                self.LANG_EN: "Offset: 40-200"
            },
            'err_pitch_range': {
                self.LANG_CN: "级高: 4-100",
                self.LANG_EN: "Pitch: 4-100"
            },
            'err_air6_range': {
                self.LANG_CN: "Air6范围",
                self.LANG_EN: "Air6 Range"
            },
            'err_hold_range': {
                self.LANG_CN: "保持时间: 10-500",
                self.LANG_EN: "Hold: 10-500"
            },
            'err_invalid_number': {
                self.LANG_CN: "无效数字",
                self.LANG_EN: "Invalid number"
            },
            'err_invalid_interval': {
                self.LANG_CN: "无效间隔",
                self.LANG_EN: "Invalid interval"
            },
        }

    def get(self, key):
        """获取翻译字符串"""
        if key in self.translations:
            return self.translations[key].get(self.current_lang, self.translations[key].get(self.LANG_EN, key))
        return key

    def switch_language(self, lang_code=None):
        """切换语言"""
        if lang_code:
            self.current_lang = lang_code
        else:
            # 切换到另一种语言
            if self.current_lang == self.LANG_CN:
                self.current_lang = self.LANG_EN
            else:
                self.current_lang = self.LANG_CN
        return self.current_lang


# 创建全局语言管理器
lang_manager = LanguageManager()


class ConfigApp:
    def __init__(self, root):
        self.root = root
        self.root.title(lang_manager.get('title'))

        # Compact window size - no excessive whitespace
        self.root.geometry("600x740")  # 增加高度以容纳语言切换
        self.root.resizable(False, False)

        # 多语言控件字典 - 保存需要更新的控件引用
        self.lang_widgets = {}

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
        self.tof_pitch = 30
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
        config_frame = ttk.LabelFrame(self.root, text=lang_manager.get('config_frame'), padding=5)
        config_frame.pack(fill='x', padx=5, pady=5)
        self.lang_widgets['config_frame'] = config_frame  # 保存引用

        # Port selection - compact layout
        port_frame = ttk.Frame(config_frame)
        port_frame.pack(fill='x', pady=3)

        port_label = ttk.Label(port_frame, text=lang_manager.get('port'))
        port_label.pack(side='left', padx=2)
        self.lang_widgets['port_label'] = port_label

        self.port_combo = ttk.Combobox(port_frame, width=20)
        self.port_combo.pack(side='left', padx=2)

        refresh_btn = ttk.Button(port_frame, text=lang_manager.get('refresh'), command=self.refresh_ports, width=7)
        refresh_btn.pack(side='left', padx=1)
        self.lang_widgets['refresh_btn'] = refresh_btn

        connect_btn = ttk.Button(port_frame, text=lang_manager.get('connect'), command=self.connect, width=7)
        connect_btn.pack(side='left', padx=1)
        self.lang_widgets['connect_btn'] = connect_btn

        monitor_label = ttk.Label(port_frame, text="  " + lang_manager.get('monitor'))
        monitor_label.pack(side='left', padx=2)
        self.lang_widgets['monitor_label'] = monitor_label

        self.monitor_btn = ttk.Button(port_frame, text=lang_manager.get('start'), command=self.toggle_monitoring, width=6)
        self.monitor_btn.pack(side='left', padx=1)
        self.lang_widgets['monitor_btn'] = self.monitor_btn

        interval_label = ttk.Label(port_frame, text=lang_manager.get('interval'))
        interval_label.pack(side='left', padx=2)
        self.lang_widgets['interval_label'] = interval_label

        self.monitor_interval_var = tk.StringVar(value="16")
        ttk.Entry(port_frame, textvariable=self.monitor_interval_var, width=5).pack(side='left', padx=1)
        ttk.Label(port_frame, text="ms").pack(side='left')

        # Language switch - 在同一行末尾添加
        lang_label = ttk.Label(port_frame, text="  " + lang_manager.get('language'))
        lang_label.pack(side='left', padx=2)
        self.lang_widgets['lang_label'] = lang_label

        self.lang_var = tk.StringVar(value=lang_manager.current_lang)
        lang_combo = ttk.Combobox(port_frame, textvariable=self.lang_var,
                                   values=[lang_manager.LANG_CN, lang_manager.LANG_EN],
                                   width=8, state='readonly')
        lang_combo.pack(side='left', padx=1)
        lang_combo.bind('<<ComboboxSelected>>', self.on_language_change)

        # Config parameters - compact grid layout
        params_frame = ttk.Frame(config_frame)
        params_frame.pack(fill='x', pady=3)

        # MPR121 Settings - fixed width labels
        mpr_frame = ttk.LabelFrame(params_frame, text=lang_manager.get('mpr121_frame'), padding=3)
        mpr_frame.pack(side='left', fill='both', padx=3)
        self.lang_widgets['mpr_frame'] = mpr_frame

        touch_label = ttk.Label(mpr_frame, text=lang_manager.get('touch'))
        touch_label.grid(row=0, column=0, sticky='w')
        self.lang_widgets['touch_label'] = touch_label

        self.touch_thr_var = tk.StringVar(value="20")
        ttk.Entry(mpr_frame, textvariable=self.touch_thr_var, width=5).grid(row=0, column=1, padx=2)
        ttk.Label(mpr_frame, text="(5-30)", width=6).grid(row=0, column=2, padx=2)

        release_label = ttk.Label(mpr_frame, text=lang_manager.get('release'))
        release_label.grid(row=1, column=0, sticky='w')
        self.lang_widgets['release_label'] = release_label

        self.release_thr_var = tk.StringVar(value="18")
        ttk.Entry(mpr_frame, textvariable=self.release_thr_var, width=5).grid(row=1, column=1, padx=2)
        ttk.Label(mpr_frame, text="(1-25)", width=6).grid(row=1, column=2, padx=2)

        help_text = lang_manager.get('mpr_help')
        self.mpr_help_label = ttk.Label(mpr_frame, text=help_text, foreground='gray', font=('Arial', 7))
        self.mpr_help_label.grid(row=2, column=0, columnspan=3, sticky='w')

        # TOF Settings - fixed width labels
        tof_frame = ttk.LabelFrame(params_frame, text=lang_manager.get('tof_frame'), padding=3)
        tof_frame.pack(side='left', fill='both', padx=3)
        self.lang_widgets['tof_frame'] = tof_frame

        offset_label = ttk.Label(tof_frame, text=lang_manager.get('offset'))
        offset_label.grid(row=0, column=0, sticky='w')
        self.lang_widgets['offset_label'] = offset_label

        self.tof_offset_var = tk.StringVar(value="120")
        ttk.Entry(tof_frame, textvariable=self.tof_offset_var, width=5).grid(row=0, column=1, padx=2)
        ttk.Label(tof_frame, text="mm", width=4).grid(row=0, column=2)

        pitch_label = ttk.Label(tof_frame, text=lang_manager.get('pitch'))
        pitch_label.grid(row=1, column=0, sticky='w')
        self.lang_widgets['pitch_label'] = pitch_label

        self.tof_pitch_var = tk.StringVar(value="30")
        ttk.Entry(tof_frame, textvariable=self.tof_pitch_var, width=5).grid(row=1, column=1, padx=2)
        ttk.Label(tof_frame, text="mm", width=4).grid(row=1, column=2)

        air6_label = ttk.Label(tof_frame, text=lang_manager.get('air6_range'))
        air6_label.grid(row=2, column=0, sticky='w')
        self.lang_widgets['air6_label'] = air6_label

        self.air6_range_var = tk.StringVar(value="150")
        ttk.Entry(tof_frame, textvariable=self.air6_range_var, width=5).grid(row=2, column=1, padx=2)
        ttk.Label(tof_frame, text="mm", width=4).grid(row=2, column=2)

        hold_label = ttk.Label(tof_frame, text=lang_manager.get('min_hold'))
        hold_label.grid(row=3, column=0, sticky='w')
        self.lang_widgets['hold_label'] = hold_label

        self.air_hold_var = tk.StringVar(value="100")
        ttk.Entry(tof_frame, textvariable=self.air_hold_var, width=5).grid(row=3, column=1, padx=2)
        ttk.Label(tof_frame, text="ms", width=4).grid(row=3, column=2)

        tof_help = lang_manager.get('tof_help')
        self.tof_help_label = ttk.Label(tof_frame, text=tof_help, foreground='gray', font=('Arial', 7))
        self.tof_help_label.grid(row=4, column=0, columnspan=3, sticky='w')

        # Buttons - wider to prevent text truncation
        btn_frame = ttk.LabelFrame(params_frame, text=lang_manager.get('actions_frame'), padding=3)
        btn_frame.pack(side='left', fill='both', padx=3)
        self.lang_widgets['btn_frame'] = btn_frame

        self.btn_read = ttk.Button(btn_frame, text=lang_manager.get('read_config'), command=self.read_config, width=16)
        self.btn_read.pack(pady=1)
        self.lang_widgets['btn_read'] = self.btn_read

        self.btn_apply = ttk.Button(btn_frame, text=lang_manager.get('apply_config'), command=self.apply_config, width=16)
        self.btn_apply.pack(pady=1)
        self.lang_widgets['btn_apply'] = self.btn_apply

        self.btn_save = ttk.Button(btn_frame, text=lang_manager.get('save_to_flash'), command=self.save_config, width=16)
        self.btn_save.pack(pady=1)
        self.lang_widgets['btn_save'] = self.btn_save

        self.btn_restore = ttk.Button(btn_frame, text=lang_manager.get('restore_defaults'), command=self.restore_defaults, width=16)
        self.btn_restore.pack(pady=1)
        self.lang_widgets['btn_restore'] = self.btn_restore

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

        self.tof_dist_label = ttk.Label(tof_container, text=lang_manager.get('tof_distance'), font=('Arial', 9, 'bold'))
        self.tof_dist_label.pack()
        self.lang_widgets['tof_dist_label'] = self.tof_dist_label

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

        self.air_keys_label = ttk.Label(air_container, text=lang_manager.get('air_keys'), font=('Arial', 9, 'bold'))
        self.air_keys_label.pack()
        self.lang_widgets['air_keys_label'] = self.air_keys_label

        # Max distance label - FIXED width
        self.max_dist_label = ttk.Label(air_container, text=lang_manager.get('max_dist') + " - mm", font=('Arial', 8),
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

        perf_frame = ttk.LabelFrame(perf_container, text=lang_manager.get('performance'), padding=3)
        perf_frame.pack()
        self.lang_widgets['perf_frame'] = perf_frame

        self.perf_labels = {}
        perf_items = [
            ("loop_avg", "loop_avg_us", "μs"),
            ("loop_max", "loop_max_us", "μs"),
            ("hid_sends", "hid_sends", ""),
            ("tof_new", "tof_new_data", ""),
            ("poll_avg", "tof_poll_avg_us", "μs"),
            ("poll_max", "tof_poll_max_us", "μs")
        ]

        for i, (label_key, data_key, unit) in enumerate(perf_items):
            perf_label = ttk.Label(perf_frame, text=lang_manager.get(label_key))
            perf_label.grid(row=i, column=0, sticky='w')
            self.lang_widgets[f'perf_{label_key}'] = perf_label

            self.perf_labels[data_key] = ttk.Label(perf_frame, text="0", foreground='blue', width=5, anchor='e')
            self.perf_labels[data_key].grid(row=i, column=1, sticky='e', padx=2)
            ttk.Label(perf_frame, text=unit, width=3).grid(row=i, column=2, sticky='w')

        # Row 1: Touch Slider - COMPACT
        touch_container = ttk.Frame(main_grid)
        touch_container.grid(row=1, column=0, sticky='nsew', padx=2, pady=2)

        self.touch_slider_label = ttk.Label(touch_container, text=lang_manager.get('touch_slider'), font=('Arial', 9, 'bold'))
        self.touch_slider_label.pack()
        self.lang_widgets['touch_slider_label'] = self.touch_slider_label

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
        log_frame = ttk.LabelFrame(self.root, text=lang_manager.get('log_frame'), padding=5)
        log_frame.pack(fill='x', padx=5, pady=5)
        self.lang_widgets['log_frame'] = log_frame

        # Reduced width to match overall window width
        self.log_text = tk.Text(log_frame, height=6, width=85, state='disabled',
                                font=('Consolas', 8), wrap='word')
        self.log_text.pack(fill='x')

        log_scrollbar = ttk.Scrollbar(log_frame, orient='vertical', command=self.log_text.yview)
        log_scrollbar.pack(side='right', fill='y')
        self.log_text.configure(yscrollcommand=log_scrollbar.set)

        # Initialize visualization
        self.update_tof_ranges()

    def on_language_change(self, event=None):
        """语言切换事件处理"""
        new_lang = self.lang_var.get()
        if new_lang != lang_manager.current_lang:
            lang_manager.switch_language(new_lang)
            self.update_ui_language()

    def update_ui_language(self):
        """更新 UI 语言"""
        # 更新窗口标题
        self.root.title(lang_manager.get('title'))

        # 更新 LabelFrame 标题
        if 'config_frame' in self.lang_widgets:
            self.lang_widgets['config_frame'].config(text=lang_manager.get('config_frame'))
        if 'mpr_frame' in self.lang_widgets:
            self.lang_widgets['mpr_frame'].config(text=lang_manager.get('mpr121_frame'))
        if 'tof_frame' in self.lang_widgets:
            self.lang_widgets['tof_frame'].config(text=lang_manager.get('tof_frame'))
        if 'btn_frame' in self.lang_widgets:
            self.lang_widgets['btn_frame'].config(text=lang_manager.get('actions_frame'))
        if 'perf_frame' in self.lang_widgets:
            self.lang_widgets['perf_frame'].config(text=lang_manager.get('performance'))
        if 'log_frame' in self.lang_widgets:
            self.lang_widgets['log_frame'].config(text=lang_manager.get('log_frame'))

        # 更新 Label 文本
        if 'port_label' in self.lang_widgets:
            self.lang_widgets['port_label'].config(text=lang_manager.get('port'))
        if 'monitor_label' in self.lang_widgets:
            self.lang_widgets['monitor_label'].config(text="  " + lang_manager.get('monitor'))
        if 'interval_label' in self.lang_widgets:
            self.lang_widgets['interval_label'].config(text=lang_manager.get('interval'))
        if 'lang_label' in self.lang_widgets:
            self.lang_widgets['lang_label'].config(text="  " + lang_manager.get('language'))

        # MPR121 标签
        if 'touch_label' in self.lang_widgets:
            self.lang_widgets['touch_label'].config(text=lang_manager.get('touch'))
        if 'release_label' in self.lang_widgets:
            self.lang_widgets['release_label'].config(text=lang_manager.get('release'))

        # TOF 标签
        if 'offset_label' in self.lang_widgets:
            self.lang_widgets['offset_label'].config(text=lang_manager.get('offset'))
        if 'pitch_label' in self.lang_widgets:
            self.lang_widgets['pitch_label'].config(text=lang_manager.get('pitch'))
        if 'air6_label' in self.lang_widgets:
            self.lang_widgets['air6_label'].config(text=lang_manager.get('air6_range'))
        if 'hold_label' in self.lang_widgets:
            self.lang_widgets['hold_label'].config(text=lang_manager.get('min_hold'))

        # Button 文本
        if 'refresh_btn' in self.lang_widgets:
            self.lang_widgets['refresh_btn'].config(text=lang_manager.get('refresh'))
        if 'connect_btn' in self.lang_widgets:
            self.lang_widgets['connect_btn'].config(text=lang_manager.get('connect'))
        if 'monitor_btn' in self.lang_widgets:
            self.lang_widgets['monitor_btn'].config(
                text=lang_manager.get('stop') if self.monitoring else lang_manager.get('start')
            )
        if 'btn_read' in self.lang_widgets:
            self.lang_widgets['btn_read'].config(text=lang_manager.get('read_config'))
        if 'btn_apply' in self.lang_widgets:
            self.lang_widgets['btn_apply'].config(text=lang_manager.get('apply_config'))
        if 'btn_save' in self.lang_widgets:
            self.lang_widgets['btn_save'].config(text=lang_manager.get('save_to_flash'))
        if 'btn_restore' in self.lang_widgets:
            self.lang_widgets['btn_restore'].config(text=lang_manager.get('restore_defaults'))

        # 可视化标签
        if 'tof_dist_label' in self.lang_widgets:
            self.lang_widgets['tof_dist_label'].config(text=lang_manager.get('tof_distance'))
        if 'air_keys_label' in self.lang_widgets:
            self.lang_widgets['air_keys_label'].config(text=lang_manager.get('air_keys'))
        if 'touch_slider_label' in self.lang_widgets:
            self.lang_widgets['touch_slider_label'].config(text=lang_manager.get('touch_slider'))

        # 性能标签
        perf_keys = ['loop_avg', 'loop_max', 'hid_sends', 'tof_new', 'poll_avg', 'poll_max']
        for key in perf_keys:
            widget_key = f'perf_{key}'
            if widget_key in self.lang_widgets:
                self.lang_widgets[widget_key].config(text=lang_manager.get(key))

        # 帮助文本
        if hasattr(self, 'mpr_help_label'):
            self.mpr_help_label.config(text=lang_manager.get('mpr_help'))
        if hasattr(self, 'tof_help_label'):
            self.tof_help_label.config(text=lang_manager.get('tof_help'))

        # 更新最大距离标签（如果已创建）
        if hasattr(self, 'max_dist_label'):
            current_text = self.max_dist_label.cget('text')
            # 保留数值部分，只更新前缀
            import re
            match = re.search(r'\d+', current_text)
            if match:
                dist = match.group()
                self.max_dist_label.config(text=f"{lang_manager.get('max_dist')} {dist} mm")

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
        self.max_dist_label.config(text=f"{lang_manager.get('max_dist')} {max_dist} mm")

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
                self.log(f"{lang_manager.get('msg_found')} {port.device}")
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
            self.log(lang_manager.get('msg_disconnected'))
            self.monitor_btn.config(text=lang_manager.get('start'))
            self.monitoring = False

        port = self.port_combo.get().split(' ')[0]

        try:
            self.ser = serial.Serial(port, 115200, timeout=1.0, write_timeout=1.0)
            self.log(f"{lang_manager.get('msg_connected')} {port}")
        except Exception as e:
            self.log(f"Failed: {e}")

    def send_cmd(self, cmd):
        if not self.ser:
            self.log(lang_manager.get('msg_not_connected'))
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
                    self.log(lang_manager.get('msg_config_loaded'))
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
                messagebox.showerror("Error", lang_manager.get('err_touch_range'))
                return
            if not (1 <= release <= 25):
                messagebox.showerror("Error", lang_manager.get('err_release_range'))
                return
            if not (40 <= offset <= 200):
                messagebox.showerror("Error", lang_manager.get('err_offset_range'))
                return
            if not (4 <= pitch <= 100):
                messagebox.showerror("Error", lang_manager.get('err_pitch_range'))
                return
            if not (pitch <= air6_range <= 200):
                messagebox.showerror("Error", f"{lang_manager.get('err_air6_range')}: {pitch}-200")
                return
            if not (10 <= air_hold <= 500):
                messagebox.showerror("Error", lang_manager.get('err_hold_range'))
                return

            cmd = f"CONFIG {touch} {release} {offset} {pitch} {air6_range} {air_hold}"
            self.send_cmd(cmd)
            self.update_tof_ranges()

        except ValueError:
            messagebox.showerror("Error", lang_manager.get('err_invalid_number'))

    def save_config(self):
        if messagebox.askyesno("Confirm", lang_manager.get('msg_save_confirm')):
            response = self.send_cmd("SAVE")
            # Check response
            if "SAVE OK" in response:
                self.log("✓ Config saved to flash successfully")
                messagebox.showinfo("Success", lang_manager.get('msg_save_success'))
            elif "SAVE ERROR" in response:
                self.log("✗ ERROR: Failed to save config to flash")
                messagebox.showerror("Error", lang_manager.get('msg_save_failed'))
            else:
                self.log(f"✗ Unexpected response: {response}")
                messagebox.showwarning("Warning", f"Unexpected response from device:\n{response}")
            self.update_tof_ranges()

    def restore_defaults(self):
        if messagebox.askyesno(lang_manager.get('msg_restore_confirm_title'),
                                lang_manager.get('msg_restore_confirm')):
            response = self.send_cmd("DEFAULT")
            if "DEFAULT OK" in response:
                self.touch_thr_var.set("20")
                self.release_thr_var.set("18")
                self.tof_offset_var.set("120")
                self.tof_pitch_var.set("30")
                self.air6_range_var.set("150")
                self.air_hold_var.set("100")
                self.update_tof_ranges()
                self.log(f"✓ {lang_manager.get('msg_restore_success')}")
                self.log(lang_manager.get('msg_save_note'))
            else:
                self.log(f"✗ ERROR: Failed to restore defaults: {response}")

    def toggle_monitoring(self):
        if self.monitoring:
            self.monitoring = False
            self.send_cmd("STOP_MONITOR")
            self.monitor_btn.config(text=lang_manager.get('start'))
            self.log(lang_manager.get('msg_monitoring_stopped'))
        else:
            if not self.ser:
                messagebox.showwarning("Warning", lang_manager.get('msg_connect_first'))
                return

            try:
                interval = int(self.monitor_interval_var.get())
                interval = max(50, min(5000, interval))

                self.send_cmd(f"START_MONITOR {interval}")
                self.monitoring = True
                self.monitor_btn.config(text=lang_manager.get('stop'))
                self.log(f"{lang_manager.get('msg_monitoring_started')} ({interval}ms)")

                self.monitor_thread = threading.Thread(target=self.monitor_receive_loop, daemon=True)
                self.monitor_thread.start()
            except ValueError:
                messagebox.showerror("Error", lang_manager.get('err_invalid_interval'))

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