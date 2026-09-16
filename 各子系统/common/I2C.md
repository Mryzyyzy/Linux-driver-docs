# I2C 子系统

以 cdns-i2c 控制器 + hym8563 RTC 为例，从下到上分层整理。

## 1. 硬件基础

### 物理层：两根线

```
    SCL ────────────────────────────┬───────────┬───────────┐
                                    │           │           │
    SDA ────────────────────┬──────┼──────┬────┼──────┬────┤
                           │      │      │    │      │    │
                         ┌─┴──┐ ┌─┴──┐ ┌─┴──┐ ┌─┴──┐ ┌─┴──┐
                         | MCU| |RTC | |EEP| |SENS| |IOEX|
                         └────┘ └────┘ └────┘ └────┘ └────┘
                         主机    从机   从机   从机   从机
```

- **SCL**：时钟线，主机产生时钟
- **SDA**：数据线，双向，主机/从机都能拉低
- 两根线开漏 + 上拉电阻，拉低 = 0，释放 = 1（线与逻辑）
- **7-bit 从机地址**：有效范围 0x08–0x77（其余保留），hym8563 是 `0x51`

### 协议层

| 信号   | 定义                                              |
| ------ | ------------------------------------------------- |
| START  | SCL 高时，SDA 由高变低                            |
| 数据位 | SCL 低时发送方切换 SDA，SCL 高时接收方采样        |
| ACK    | 第 9 个时钟，接收方拉低 SDA 表示收到；不拉 = NACK |
| STOP   | SCL 高时，SDA 由低变高                            |

### 传输序列

- **写寄存器**：START + 从地址+W + ACK + 寄存器地址 + ACK + 数据 + ACK + … + STOP
- **读寄存器**：START + 从地址+W + ACK + 寄存器地址 + ACK + **重复 START** + 从地址+R + ACK + 数据（最后一字节主机回 NACK）+ STOP

`i2c_smbus_read_byte_data(client, reg)` 硬件上就是"写寄存器地址 + 重复 START + 读一个字节"。

## 2. 三个核心结构

定义在 `include/linux/i2c.h`。

### `struct i2c_adapter` — I2C 控制器（主机端）

```c
struct i2c_adapter {
    const struct i2c_algorithm *algo;  // master_xfer 在这里
    void *algo_data;                   // 控制器驱动私有数据
    struct device dev;
    int nr;                            // 总线号：i2c-0, i2c-1...
};
```

一个 adapter 对应一个控制器硬件（如 `4020000.i2c` = i2c-1）。cdns-i2c 中 `algo_data` 存 `struct cdns_i2c`，`algo->master_xfer` 指向 `cdns_i2c_master_xfer`（`drivers/i2c/busses/i2c-cadence.c:859`）。

### `struct i2c_client` — I2C 从设备

```c
struct i2c_client {
    unsigned short addr;         // 7-bit 地址，如 0x51
    char name[I2C_NAME_SIZE];    // 如 "hym8563"
    struct i2c_adapter *adapter; // 挂在哪条总线
    struct device dev;
    int irq;
};
```

从设备驱动的所有操作都围绕 `struct i2c_client *client` 展开。

### `struct i2c_driver` — 从设备驱动

```c
struct i2c_driver {
    int (*probe)(struct i2c_client *);
    void (*remove)(struct i2c_client *);
    const struct i2c_device_id *id_table;
    struct device_driver driver;
};
```

通过 `id_table` 或设备树 `compatible` 匹配，成功则调 `probe`。

### 关系

```
i2c_adapter (I2C 控制器)
    │
    ├── i2c_client (hym8563@0x51) ←── i2c_driver (rtc-hym8563)
    ├── i2c_client (pca9535@0x20) ←── i2c_driver (pca953x)
    └── i2c_client (eeprom@0x50)  ←── i2c_driver (at24)
```

一个 adapter 挂多个 client，每个 client 绑定一个 driver。

## 3. 从设备驱动视角

从设备驱动不碰控制器寄存器，只调 I2C 核心 API（SMBus 是 I2C 的标准化子集）：

```c
data = i2c_smbus_read_byte_data(client, HYM8563_CTL2);
i2c_smbus_write_byte_data(client, HYM8563_CTL2, data);
```

### 调用链

```
从设备驱动
  i2c_smbus_read_byte_data(client, reg)      [i2c-core-smbus.c]
    -> i2c_smbus_xfer()
      -> __i2c_transfer(adap, msgs, num)     [i2c-core-base.c]
        -> adap->algo->master_xfer(adap, msgs, num)   // 函数指针
控制器驱动
  cdns_i2c_master_xfer()                     [i2c-cadence.c]
    -> 操作控制器硬件寄存器完成传输
```

从设备驱动完全不关心底层是哪个控制器——I2C 核心负责路由到对应 adapter 的 `master_xfer`，因此 hym8563 驱动可移植到任何平台。

### `struct i2c_msg` — 一次传输的描述

```c
struct i2c_msg {
    __u16 addr;   // 从设备地址
    __u16 flags;  // I2C_M_RD 等
    __u16 len;
    __u8 *buf;
};
```

`i2c_smbus_read_byte_data` 本质是构造 2 个 msg（写寄存器地址 1 字节 + 读数据 1 字节），一次 `i2c_transfer` 发出。

## 4. 控制器驱动视角

职责：初始化控制器（时钟/引脚/寄存器）、实现 `master_xfer`、处理中断、电源管理。

### `cdns_i2c_master_xfer` 流程

```
1. pm_runtime_resume_and_get()          // 确保控制器 active
2. 检查总线空闲（SR 寄存器 BA 位）
3. 写目标设备地址
4. 逐个 msg：写 → 填 FIFO 启动；读 → 设字节数启动
5. 等传输完成（中断）
6. 数据拷回 msg->buf
7. 检查错误（NACK、仲裁丢失）
8. pm_runtime_mark_last_busy + put_autosuspend
```

### runtime PM

控制器空闲时关时钟省电，有传输再开：

- `pm_runtime_resume_and_get`：唤醒（开时钟 + 恢复寄存器）
- `pm_runtime_put_autosuspend`：延迟自动休眠（防止频繁开关时钟）

实现见 `i2c-cadence.c:1354` 的 `cdns_i2c_runtime_suspend` / `:1392` 的 `cdns_i2c_runtime_resume`。

## 5. 设备枚举方式

I2C 无自枚举能力（不像 USB/PCIe 能扫描），从设备来源：

1. **设备树**（主要方式）：内核解析 DTS 子节点，为每个节点创建 `i2c_client` 再匹配驱动

   ```dts
   &i2c1 {
       hym8563: rtc@51 {
           compatible = "haoyu,hym8563";
           reg = <0x51>;
           interrupts = <9 IRQ_TYPE_LEVEL_LOW>;
       };
   };
   ```
2. **ACPI**：x86 平台的等价机制
3. **用户空间 instantiate**（调试常用）：

   ```bash
   echo hym8563 0x51 > /sys/bus/i2c/devices/i2c-1/new_device
   ```

## 6. 调试接口

```bash
# 总线与设备（1-0051 = 总线 1 上地址 0x51）
ls /sys/bus/i2c/devices/
ls -l /sys/bus/i2c/devices/1-0051/driver   # 看绑定的驱动
ls /sys/bus/i2c/drivers/

# i2c-tools
i2cdetect -y 1          # 扫描总线
i2cget 1 0x51 0x02      # 读寄存器
i2cset 1 0x51 0x02 0x00
i2cdump 1 0x51          # dump 全部寄存器

# tracepoint
echo 1 > /sys/kernel/tracing/events/i2c/enable
```

## 7. 实战案例：resume 时 I2C 报 -13

场景：RTC 闹钟唤醒系统，`hym8563_resume()` 后 worker 首次 I2C 传输失败，`dev_err("i2c runtime fail, ret = -13")`。

```
硬件：  RTC 闹钟到点 -> INT 拉低 -> GPIO 中断 -> 系统唤醒
固件：  BL31 resume -> S3 script 操作 IOEXP -> I2C 控制器未初始化 -> 超时
内核：  dpm_resume（父先子后）
          ├── cdns_i2c_resume()        // 父，先执行
          │     -> pm_runtime_force_resume()
          │     -> disable_depth 仍在过渡
          └── hym8563_resume()         // 子，后执行
                -> 清 suspended -> re-enable IRQ
                     -> RTC 中断已 pending -> worker 立刻跑
                       -> i2c_smbus_read_byte_data()
                         -> cdns_i2c_master_xfer()
                           -> pm_runtime_resume_and_get()
                             -> rpm_resume() 见 disable_depth > 0 且状态非 ACTIVE
                             -> 返回 -EACCES(-13)
```

根因：开中断早于 I2C 控制器 runtime resume 完成，worker 撞进时间窗口。
修复：`hym8563_resume` 里先 `pm_runtime_resume_and_get` 确认控制器唤醒，再 enable IRQ。

## 8. 知识点自查

- [ ] I2C 物理层与协议层（START/STOP/ACK/7-bit 地址）
- [ ] adapter / client / driver 三者关系
- [ ] `i2c_msg` 结构，一次 SMBus read 对应 2 个 msg
- [ ] 控制器驱动 `master_xfer` 大致流程
- [ ] runtime PM 与 system PM 在 I2C 驱动中的区别
- [ ] 设备枚举方式（设备树为主）
- [ ] 从设备驱动到硬件的完整调用链
- [ ] i2cdetect / i2cget / i2cset / i2cdump 调试
- [ ] `/sys/bus/i2c/` 目录结构

### 练习

1. 读通 `i2c-cadence.c` 的 probe：时钟、中断、寄存器、adapter 注册
2. `i2cdump 1 0x51` 对照 datasheet 看 hym8563 寄存器
3. 用 tracepoint 跟踪 `i2cget 1 0x51 0` 的一次完整传输
