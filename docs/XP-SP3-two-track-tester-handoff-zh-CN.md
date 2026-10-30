# Windows XP SP3 USB 控制双路线测试交接

这两个压缩包是互斥的实验版本，不是生产版。每条路线必须从**独立、干净的 Windows XP Professional SP3 32 位 NTFS 快照**开始。不要在同一系统中叠加安装，也不要在生产设备上安装。内核过滤驱动路线存在无法启动系统的风险，测试前务必准备可恢复快照或完整磁盘镜像。

## 路线一：SetupAPI

压缩包名称含 `SetupAPI-1.1.6-TEST`。修改 `config\deployment.ini` 中的服务器地址，以及测试用的管理员密码和注册令牌。用管理员身份运行 `server\install-server.cmd`（若已有同版本测试服务器可跳过），然后在客户端运行 `client\install-client.cmd`。查看代理日志中的 `enforcement_backend=setupapi_propertychange`。只有出现 `blocked_verified` 且实际文件读取、写入均失败，才可以记录为阻断成功。`enforce_unverified` 或仍可读写均视为失败。卸载前连接测试过的所有 U 盘，并以管理员身份运行 `client\rollback-client.cmd`；如果提示有设备无法恢复，不要直接卸载，先连接该设备再重试。

## 路线二：内核磁盘过滤驱动

`Filter-1.1.7-TEST` 的日志没有任何已验证阻断，测试人员也证实手动启用后可读取；不要继续使用该包。新包名称含 `Filter-1.1.8-DIAGNOSTIC-TEST`，仅增加诊断，**尚未证明阻断有效**。**仅在干净快照中**修改相同配置，然后运行 `client\install-client.cmd`。该脚本先注册过滤驱动，再安装代理。安装后必须重启 XP，重启前不要测试 USB。重启后先运行 `client\diagnose-filter.cmd` 并保存输出。若显示 `FAIL_DO_NOT_TEST_AS_PROTECTED`，停止安全验证，将输出和 Windows 系统事件日志反馈给开发团队；只有显示 `READY_FOR_BYPASS_TEST` 才继续实际读写和设备管理器手动启用测试。查看日志中的 `enforcement_backend=xp_disk_filter` 和 `stage=`。确认系统正常启动、内部磁盘和键盘鼠标正常工作，再插入测试 U 盘。回退时以管理员身份运行 `client\rollback-client.cmd` 并重启；若无法启动，请直接恢复快照。

## 两条路线都要记录

1. 使用至少两支 U 盘，其中一支有多个分区或多个磁盘单元；逐一测试所有盘符的文件读取和写入。过滤驱动路线插入后再次运行 `client\diagnose-filter.cmd`，确认 `attached_usb_disks` 不为 0，且代理日志对每个设备出现 `stage=attached`；否则视为未保护。
2. 同一支 U 盘拔插至少 20 次，包括关闭通知窗口后再次插入；每次记录是否弹窗、盘符是否出现、文件是否可读写。
3. 在服务端批准、撤销后分别测试已插入状态和重新插入状态；记录实际生效时间。
4. 重启代理服务、断开服务器网络、重启 XP 后重复默认拒绝测试。
5. 记录内部磁盘、USB 集线器、键盘和鼠标是否受影响。
6. 发送完整的 `C:\Documents and Settings\All Users\Application Data\UsbDlp\legacy-agent.log`、Windows 事件查看器“系统”日志、设备管理器状态、操作时间线和测试结果。发生蓝屏时保留转储文件。

**判定规则：弹窗或日志文字本身不是阻断证据。未批准设备只要有任一盘符仍可读或写，该次测试即失败。发生蓝屏、无法启动或影响内部磁盘时立即停止测试并恢复快照。**

已知失败：1.1.6 在设备管理器手动启用后、下一次检测前存在可访问窗口；1.1.7 的代理日志中所有过滤驱动请求均失败，且未确认任何阻断。两个版本均未通过安全测试。拥有本地管理员权限的用户不属于当前用户态防护的抗绕过保证；如必须抵御本地管理员，需要另行设计并验证更强的内核和设备管理控制。
