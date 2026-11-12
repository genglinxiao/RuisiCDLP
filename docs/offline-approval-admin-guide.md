# 离线审批 —— 管理员操作手册

> 面向：负责签发授权的 IT 管理员
> 适用：agent 无网络或无法连服务端的终端

## 1. 两条审批通道

| | 标准码 | 短码 |
|---|---|---|
| 长度 | 442 字符 | **24 字符** |
| 怎么传递 | 微信 / 邮件发文本，用户复制粘贴 | **电话念、手输** |
| 需要什么 | RSA 私钥 | 该终端专属的短码密钥 |
| 最长有效期 | 168 小时（7 天） | **8 小时** |
| 安全边界 | 无私钥无法伪造 | 需取得该机密钥 |

**默认走标准码**，只有"完全断网、只能打电话"时才用短码。

## 2. 标准流程（标准码）

1. 终端插入设备被拦，弹窗显示：
   ```
   设备码    5B0H-ZZMN-HDFP-7GTF
   ```
2. 用户把**设备码**报给 IT（16 字符，可口头念）
3. IT 执行：
   ```bash
   python tools/approve.py signed --device-code 5B0H-ZZMN-HDFP-7GTF --hours 4
   ```
4. 把输出的 442 字符码发给用户
5. 用户粘贴 → 点「提交审批码」→ 显示「已批准，设备现在可用」
6. **不需要重新插拔**，设备几秒内恢复

## 3. 电话流程（短码）

1. 用户报设备码，同上
2. IT 查台账拿到该终端的短码密钥，执行：
   ```bash
   python tools/approve.py short --device-code 5B0H-... --hours 4 --device-key <32字节hex>
   ```
3. 念给用户：`2G7E-G4HA-R4FG-0EQ7-36ZY-PTC8`
4. 用户在弹窗里输入 → 提交

短码有效期上限 **8 小时**，超过时必须改用标准码。

## 4. IT 台账要记什么

每台终端一行：

| 设备码 | 短码密钥 | 登记时间 | 备注 |
|---|---|---|---|
| `5B0H-ZZMN-HDFP-7GTF` | `852dbd37...63b3` | 2026-11-10 | Win11 / 门店 A |

**密钥从哪来**：agent 首次启动时自行生成，写入 agent 日志：

```
short_code_ready key=GMPVTDREN5W71T3Z5FN473CA27ZDBN92G5QA5SFGBJV11G3GCESG
```

这是 Base32（32 字节），转成 hex 才能给 `approve.py` 用。

## 5. ⚠️ 短码密钥重装问题（产品设计特点，务必知晓）

**短码密钥由每台终端的 agent 在首次启动时本地随机生成，不存在任何集中下发。**

这带来一个必须记住的后果：

| 操作 | 设备码 | 短码密钥 | 短码还能用吗 |
|---|---|---|---|
| 覆盖升级 agent | 不变 | 不变 | ✅ 能 |
| 卸载后重装 agent（保留数据目录） | 不变 | 不变 | ✅ 能 |
| 卸载后重装 agent（清空数据目录） | 不变 | **重新生成** | ❌ 台账失效 |
| **重装操作系统** | 不变 | **重新生成** | ❌ 台账失效 |

关键在于：**设备码基于硬件序列号计算，重装系统后不会变；但密钥一定会变。**

因此重装之后：

- IT 台账里「设备码 → 旧密钥」的映射失效
- 按旧密钥签出来的短码会报 `invalid_mac`
- **标准码不受影响**（它只依赖 IT 手里的私钥，与终端状态无关）

**处置办法**：重装完成后，从该终端的新日志里取出新的 `short_code_ready key=`，
更新台账即可。在此之前，该终端只能走标准码。

> 之所以这样设计：密钥完全由终端本地生成，IT 侧不持有任何可用于伪造其他终端授权的秘密。
> 一台机器的密钥被提取，影响范围仅限那一台。

## 6. 错误对照

| 弹窗提示 | 原因 | 处置 |
|---|---|---|
| `invalid_format` | 码没复制全，或含非法字符 | 重发/重输，确认首尾字符 |
| `invalid_signature` | 标准码被改动，或公钥不匹配 | 重新签发 |
| `wrong_device` | 码绑定的设备与当前弹窗不一致 | 核对设备码后重签 |
| `expired` | 超过有效期 | 重新签发 |
| `replay` | 该码已使用过 | 签发新码（同一分钟内生成的码相同，需等下一分钟） |
| `invalid_mac` | 短码密钥不匹配（常见于重装后） | 按第 5 节重新采集密钥 |
| `short_code_disabled` | 该终端配置关闭了短码通道 | 改用标准码 |

## 7. 关闭短码通道

在 `%ProgramData%\UsbDlp\legacy-agent.ini` 中：

```ini
[Agent]
ShortCode=0
```

重启服务后生效。敏感环境建议关闭，只允许标准码。

## 8. 审计日志

每次审批尝试都会记录，码本身**不写入日志**，只记哈希前 16 位：

```
approval_accepted instance_id=... kind=signed duration_h=4 code=WQ4MHB966T40046J
approval_rejected instance_id=... result=invalid_mac code=...
approval_entry_added instance_id=... expires_utc_ms=1794318540000
approval_devnode_enabled instance_id=...
approval_devnode_enable_failed instance_id=... result=...
```
