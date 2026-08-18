# Flash 持久化修复说明

## 问题诊断

### 原因分析

经过详细代码分析，发现配置无法掉电保存的**根本原因**是：

#### 1. Flash 地址计算错误（最严重）

**位置**: `src/save.cpp:14`

```cpp
// 错误的代码：
#define FLASH_TARGET_OFFSET (256 * 1024 - FLASH_SECTOR_SIZE)
```

**问题**：
- 硬编码假设 Flash 大小为 256KB
- RP2040 Pico 标准配置是 **2MB Flash**
- 252KB (256KB - 4KB) 的位置会落在**程序代码区域内部**！
- 可能导致：
  - 程序代码被覆盖（如果真的写入成功）
  - 写入失败（Flash 保护机制）
  - 配置无法正确读取

**正确做法**：
```cpp
// 修复后的代码：
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
```

使用 RP2040 SDK 提供的 `PICO_FLASH_SIZE_BYTES` 宏，自动适配不同 Flash 容量。

#### 2. 缺少数据完整性校验

原始实现只有 magic number，没有：
- CRC32 校验
- 版本号管理
- 数据验证

如果 Flash 数据部分损坏，可能读取到错误配置。

#### 3. 缺少写入验证

原始 `save_write()` 函数：
- 调用 `flash_range_program()` 后立即返回 `true`
- 没有读回数据验证
- 无法判断写入是否真正成功

#### 4. 固件返回结果不明确

原始 SAVE 命令处理：
```cpp
else if (strcmp(cmd, "SAVE") == 0) {
    config_save();  // void 函数，无返回值
    printf("OK Config saved\r\n");  // 总是打印 OK
}
```

无论保存成功还是失败，都返回相同的结果。

#### 5. Python GUI 没有验证保存结果

```python
def save_config(self):
    if messagebox.askyesno("Confirm", "Save to flash?"):
        self.send_cmd("SAVE")  # 发送后不检查返回值
        self.update_tof_ranges()
```

## 修复方案

### 1. 修正 Flash 地址 (src/save.cpp)

```cpp
// 使用 SDK 提供的 Flash 大小宏
#define CONFIG_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
#define CONFIG_FLASH_ADDR (XIP_BASE + CONFIG_FLASH_OFFSET)

// 启动时打印配置，方便调试
void save_init(uint32_t magic, mutex_t* mutex) {
    ...
    printf("CONFIG FLASH: Offset=0x%X, Addr=0x%X\r\n",
           CONFIG_FLASH_OFFSET, CONFIG_FLASH_ADDR);
}
```

### 2. 增加数据完整性保障

定义持久化配置结构：

```cpp
typedef struct {
    uint32_t magic;        // Magic number 验证
    uint16_t version;      // 配置版本号
    uint16_t reserved;     // 保留字段

    // 配置数据
    uint8_t hid_mode;
    uint8_t touch_threshold;
    uint8_t release_threshold;
    uint8_t tof_offset;
    uint8_t tof_pitch;
    uint8_t air6_range;
    uint16_t air_min_hold_ms;
    uint16_t air_threshold[5];

    uint32_t crc32;        // CRC32 校验
} PersistentConfig;
```

### 3. 增加写入验证

```cpp
bool save_write(const void* data, uint32_t len) {
    // 1. 准备数据
    // 2. 计算 CRC32
    // 3. 擦除 Flash
    // 4. 写入 Flash
    // 5. 读回验证

    // 验证 magic
    if (flash_cfg->magic != cfg_to_save.magic) {
        return false;
    }

    // 验证版本
    if (flash_cfg->version != cfg_to_save.version) {
        return false;
    }

    // 验证 CRC
    if (flash_cfg->crc32 != cfg_to_save.crc32) {
        return false;
    }

    // 验证数据内容
    if (memcmp(&flash_cfg->hid_mode, &cfg_to_save.hid_mode, len) != 0) {
        return false;
    }

    return true;
}
```

### 4. 修改固件返回明确结果

```cpp
// main.cpp
else if (strcmp(cmd, "SAVE") == 0) {
    if (config_save()) {
        printf("SAVE OK\r\n");
    } else {
        printf("SAVE ERROR\r\n");
    }
}

// config.cpp
bool config_save() {
    ...
    bool success = save_write(cfg, sizeof(config_t));
    return success;
}
```

### 5. 修改 Python GUI 检查结果

```python
def save_config(self):
    if messagebox.askyesno("Confirm", "Save to flash?"):
        response = self.send_cmd("SAVE")
        if "SAVE OK" in response:
            self.log("✓ Config saved to flash successfully")
            messagebox.showinfo("Success", "Configuration saved to flash!")
        elif "SAVE ERROR" in response:
            self.log("✗ ERROR: Failed to save config to flash")
            messagebox.showerror("Error", "Failed to save configuration!")
```

## 测试验证步骤

### 1. 编译并烧录固件

```bash
cd build
cmake ..
make
# 烧录 UF2 文件到 RP2040
```

### 2. 启动设备，检查串口日志

连接串口 (115200 baud)，观察启动日志：

```
========================================
CONFIG INIT: Starting configuration load
========================================
CONFIG FLASH: Offset=0x1FF000, Addr=0x101FF000, SectorSize=4096, PageSize=256
CONFIG INIT: Flash load failed, using defaults
CONFIG LOAD DEFAULT: touch=20 release=18 offset=120 pitch=30 air6=150 hold=100
========================================
```

**关键点**：
- `Offset=0x1FF000` (对于 2MB Flash) 而不是错误的 `0x3F000`
- 如果是首次运行，会看到 "CONFIG LOAD DEFAULT"

### 3. 使用 GUI 测试保存功能

1. 连接设备
2. 点击 "Read Config" 确认当前配置
3. 修改参数，点击 "Apply Config"
4. 点击 "Save to Flash"
5. 观察日志输出：

```
>> SAVE
<< CONFIG SAVE: Starting save operation...
<< CONFIG SAVE: Magic=0xCA34CAFE, Version=1, CRC=0xXXXXXXXX
<< CONFIG SAVE: Erasing sector at offset 0x1FF000...
<< CONFIG SAVE: Programming 256 bytes at offset 0x1FF000...
<< CONFIG SAVE: Verifying write...
<< CONFIG SAVE: SUCCESS - Verified magic, version, CRC, and data
<< CONFIG SAVE: SUCCESS
<< SAVE OK
```

6. GUI 应该显示成功消息

### 4. 掉电测试

**关键测试**：

1. 点击 "Save to Flash"
2. 确认看到 "SAVE OK"
3. **断开 USB 连接**（设备断电）
4. 重新连接 USB
5. 打开串口，观察启动日志：

```
========================================
CONFIG INIT: Starting configuration load
========================================
CONFIG FLASH: Offset=0x1FF000, Addr=0x101FF000, ...
CONFIG INIT: Loaded from Flash, validating...
CONFIG LOAD FLASH: touch=20 release=18 offset=120 pitch=30 air6=150 hold=100
========================================
```

**成功标志**：
- 看到 "CONFIG LOAD FLASH" 而不是 "CONFIG LOAD DEFAULT"
- 参数值与保存前一致

6. 使用 GUI 的 "Read Config" 确认参数正确

### 5. DEFAULT 命令测试

1. 点击 "Restore Defaults"
2. 观察日志：

```
>> DEFAULT
<< DEFAULT OK touch=20 release=18 offset=120 pitch=30 air6=150 hold=100
<< NOTE: Defaults applied to RAM. Use SAVE to persist to Flash.
```

**重要**：DEFAULT 只修改 RAM，不会自动保存到 Flash

3. 断电重启
4. 应该看到仍然加载之前的保存配置

### 6. 边界情况测试

#### 测试 CRC 校验

1. 保存配置
2. 使用调试工具手动修改 Flash 中的数据（破坏 CRC）
3. 重启设备
4. 应该看到 "CONFIG LOAD: CRC mismatch" 并使用默认配置

#### 测试参数范围验证

1. 手动修改 Flash 中的参数为非法值（如 touch_threshold = 0）
2. 重启设备
3. 应该看到参数被自动修正：

```
CONFIG VALIDATE: touch_threshold 0 -> 20 (out of range 5-30)
CONFIG INIT: Parameters corrected after validation
```

## 技术细节

### Flash 地址计算

RP2040 Flash 布局：
- Flash 起始地址：`XIP_BASE = 0x10000000`
- 程序代码：从 `0x10000000` 开始
- 配置存储：最后一个 sector

对于 2MB Flash (Pico 标准)：
- Flash 大小：`0x200000` (2,097,152 bytes)
- Sector 大小：`0x1000` (4,096 bytes)
- 配置偏移：`0x1FF000` (Flash 末尾 - 1 sector)
- 配置地址：`0x101FF000`

### CRC32 实现

使用 IEEE 802.3 标准多项式：
- 多项式：`0xEDB88320`（反向表示）
- 初始值：`0xFFFFFFFF`
- 最终异或：`0xFFFFFFFF`
- 与大多数 CRC32 工具兼容

### 启动流程

```
main()
  ↓
config_init()
  ↓
save_load() 尝试从 Flash 加载
  ↓
验证 magic → 验证 version → 验证 CRC
  ↓
如果全部通过：
  复制 Flash 数据到 config_data
  验证参数范围
  使用 Flash 配置
如果失败：
  使用 default_config
  ↓
设置 cfg = &config_data
  ↓
mpr121_init(), vl53l0x_init(), air_init()
  ↓
硬件初始化使用已加载的配置
```

## 常见问题

### Q: 为什么原来硬编码 256KB 是错误的？

A: RP2040 的 Flash 大小取决于具体板子：
- Raspberry Pi Pico: 2MB
- 其他 RP2040 板子：可能是 1MB、2MB、4MB、8MB、16MB

硬编码 256KB 会在大于 256KB 的 Flash 上落在程序代码区域，导致：
1. 程序被破坏（最严重）
2. Flash 写保护阻止写入
3. 配置无法正确读取

使用 SDK 的 `PICO_FLASH_SIZE_BYTES` 可以自动适配。

### Q: 为什么不把配置放在 Flash 开头？

A: 程序代码从 Flash 开头开始存放。放在末尾可以：
1. 避免与程序代码冲突
2. 程序更新时不会覆盖配置（UF2 通常从开头写入）
3. Flash 容量扩展时配置位置自动调整

### Q: DEFAULT 为什么不自动保存？

A: 用户可能想：
1. 临时测试默认值
2. 之后再调整其他参数
3. 最后一次性保存

如果 DEFAULT 自动保存，用户就失去了撤销的机会。

当前设计：
- DEFAULT：恢复默认值到 RAM
- SAVE：保存当前 RAM 配置到 Flash
- 用户可以选择 "DEFAULT + SAVE" 来实现恢复并保存

## 总结

**核心修复**：
1. ✅ Flash 地址使用 `PICO_FLASH_SIZE_BYTES` 而非硬编码
2. ✅ 增加 CRC32 校验
3. ✅ 增加版本号管理
4. ✅ 增加写入验证
5. ✅ 固件返回明确的成功/失败结果
6. ✅ Python GUI 检查返回值并显示结果
7. ✅ 详细的启动和调试日志

**测试要点**：
- 首次启动看到 "CONFIG LOAD DEFAULT"
- 保存后看到 "SAVE OK" 和 "CONFIG SAVE: SUCCESS"
- 掉电重启看到 "CONFIG LOAD FLASH"
- 参数值与保存前一致