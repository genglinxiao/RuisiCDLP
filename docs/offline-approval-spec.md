# 离线审批码协议 v1

> 适用范围：`legacy`（C++ 原生线，XP–Win11）与 `modern`（.NET 8 线，Win10 22H2+ / x64）**必须字节级一致**。
> 任何一侧改动此格式，另一侧必须同步，否则同一枚审批码无法互通。

## 0. 为什么需要两条通道

| 通道 | 机制 | 码长 | 安全边界 | 场景 |
|---|---|---|---|---|
| 标准码 | RSA-2048 签名 | ~442 字符 | 无私钥不可伪造 | 可发文本/文件 |
| 短码 | per-device HMAC | 24 字符 | 需窃取该机密钥 + 时效 + 防重放 | 完全断网、只能电话口报 |

标准码是主力。短码只在无法传输长文本时使用，**有效期上限 8 小时**。

## 1. 公共定义

### 1.1 时间基准

```
BASE_EPOCH_MS = 1735689600000   // 2025-01-01T00:00:00Z
minute(t) = floor((t_ms - BASE_EPOCH_MS) / 60000)
```

分钟数以**大端无符号整数**编码。短码用 3 字节（≈31.9 年），标准码用 4 字节。

### 1.2 设备标识哈希

```
deviceIdHash = SHA256( UTF8(instanceId) )   // 取前 10 字节
```

`instanceId` 是 Windows 设备实例 ID，例如
`USBSTOR\DISK&VEN_MASS&PROD_STORAGE_DEVICE&REV_1.00\121220160204&0`。

**必须对原始字符串做 UTF-8 编码后再哈希，不得先做大小写折叠或路径规范化。**

### 1.3 字符编码：Crockford Base32

字母表：`0123456789ABCDEFGHJKMNPQRSTVWXYZ`（32 个，去掉了 I、L、O、U）

- 编码：每 5 bit 一个字符，高位在前，末尾不足 5 bit 补零
- 解码：大小写不敏感；`I`/`L` → `1`，`O` → `0`；忽略 `-` 和空格
- 解码遇到字母表外字符 → 整码无效

## 2. 设备码（用户报给 IT）

```
deviceCode = Base32( deviceIdHash )            // 10 字节 → 16 字符
显示格式    = XXXX-XXXX-XXXX-XXXX
```

16 字符可以口头报。IT 侧按设备码查表，找到对应的短码密钥（若有）。

## 3. 标准审批码

### 3.1 载荷（20 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | version | 固定 `0x01` |
| 1 | 10 | deviceIdHash | 见 1.2 |
| 11 | 4 | notBefore | minute()，大端 |
| 15 | 4 | notAfter | minute()，大端 |
| 19 | 1 | durationHours | 1–168，仅用于界面显示 |

### 3.2 签名

- 算法：RSA-2048，PKCS#1 v1.5，SHA-256
- 被签内容：**恰好 20 字节载荷**（不含版本前缀、不含编码字符）
- 签名长度：256 字节

### 3.3 编码

```
code = Base32( payload(20) || signature(256) )   // 276 字节 → 442 字符
```

校验失败一律视为无效，不做任何"容错修正"（Base32 解码的字符合并除外）。

## 4. 短码

### 4.1 载荷（8 字节）

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 1 | version(高4) \| durationHours(低4) | version=1；durationHours=notAfter-notBefore（小时） |
| 1 | 3 | notBefore | minute()，大端 3 字节 |
| 4 | 3 | deviceIdHash[0..2] | 设备哈希前 3 字节 |
| 7 | 1 | reserved | 固定 `0x00` |

### 4.2 MAC

```
key   = 该设备专属的 256 位随机密钥（见 §6）
mac   = HMAC-SHA256(key, payload)[0..6]        // 截断 7 字节
code  = Base32( payload(8) || mac(7) )         // 15 字节 → 24 字符
显示   = XXXX-XXXX-XXXX-XXXX-XXXX-XXXX
```

### 4.3 时长限制

```
durationHours ∈ [1, 8]
```

超过 8 小时的请求必须走标准码。**这是硬约束，两条线都要拒绝。**

## 5. 验证流程

### 5.1 公共前置

1. 解码 Base32；长度不符（标准码 ≠ 276 字节，短码 ≠ 15 字节）→ 拒绝
2. 校验 version = 1 → 否则拒绝
3. 校验 `deviceIdHash` 与**本机当前设备**一致 → 否则拒绝（码绑定了别的设备）
4. 校验时间窗口：`notBefore ≤ now < notAfter`，`now` 使用**时间高水位**
5. 防重放：以 `code 原文的 SHA-256` 为键记录，已用过 → 拒绝

### 5.2 标准码额外步骤

RSA 验签通过 → 才进入 5.1 的 3、4、5 步。

### 5.3 短码额外步骤

用本机密钥重算 MAC，常数时间比对 → 才进入 5.1 的 3、4、5 步。

### 5.4 时间高水位

`now = max(系统时间, 已记录的最大时间)`，最大值**必须持久化**，否则重启后归零、改系统时间即可复活过期码。

## 6. 密钥管理

### 6.1 标准码密钥对

- 生成：IT 侧一次性生成 RSA-2048
- 私钥：**只存在于 IT 侧**，不得下发到任何终端
- 公钥：编译进 agent（legacy 为 `uint8_t[256]` 常量，modern 为嵌入资源）
- 轮换：更换密钥对需要重新签发所有在有效期内的码；建议年度轮换

### 6.2 短码密钥（per-device）

- 生成：agent 首次启动时用系统 CSPRNG 生成 32 字节
- 存储：DPAPI(Machine scope) 加密后存 `%ProgramData%\UsbDlp\short-key.bin`
- 登记：IT 通过设备码把密钥记入自己的台账
- 泄露影响：**仅限该设备**
- 开关：ini `[Agent] ShortCode=0` 可整体关闭短码通道（默认 `1`）

## 7. 审计要求

每次审批尝试（成功或失败）都要记录：

```
approval_result kind=signed|short instance_id=... code_hash=... result=ok|invalid_mac|expired|replay|wrong_device not_after=...
```

码本身**不得写入日志**（只记哈希），避免日志泄露可用凭证。
