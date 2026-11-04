Ruisi USB DLP · XP 蓝屏（STOP 0x7B）修复包 · HF1
=====================================================

【问题】
1.1.8 的 XP 磁盘类过滤驱动装完后，下一次重启会蓝屏：
    STOP 0x7B INACCESSIBLE_BOOT_DEVICE (0xC0000034)
根因：安装器用 SERVICE_DEMAND_START(3) 创建驱动服务，且没有写 Group，
      但驱动已经被追加到 DiskDrive 类的 UpperFilters。启动时卷栈拼不起来。

【修复内容】
驱动的启动配置必须是：
    Start = 0  (SERVICE_BOOT_START)
    Group = "System Bus Extender"   （PartMgr 所在的加载组）
只改 Start=0 冷启动能过，但热重启会掉到 PXE，所以两个值必须同时正确。

【三种用法】

1) 机器还能进系统 —— 最常见
   把 fix-xp-boot.cmd 拷到机器上，右键“以管理员身份运行”（或管理员 cmd 里执行）。
   看到 REPAIR_OK 后重启，再运行 verify-xp-boot.cmd，必须输出 BOOT_SAFETY=SAFE。
   修复前先用 verify-xp-boot.cmd 看现状，输出 NOT_SAFE 的机器一旦重启就回不来了。

2) 机器已经蓝屏起不来 —— 离线救援
   拆盘（或挂虚拟盘/镜像）到一台正常的 Windows / WinPE 上，执行：
       recover-offline.cmd E:\WINDOWS
   它会把 SYSTEM 注册表挂载起来，把 UpperFilters 改回只有 PartMgr，
   并把过滤驱动服务置为 Start=4（禁用）。改完装回原机即可正常启动，
   进系统后再按正常流程卸载 Agent。

3) 新装机 —— 装完立刻跑 fix-xp-boot.cmd
   在重启之前执行，不要只依赖安装器的返回码。

【已验证】
VirtualBox + XP SP3：从 Start=3 的蓝屏态执行 fix-xp-boot.cmd，
冷启动与热重启均正常进桌面，diagnose 全绿。
物理机（Lenovo E40）尚未复测，上生产前必须按同一流程复测一次。

【注意】
- 脚本里的判断全部基于注册表回读，不看命令返回码，回读不符一律拒绝。
- 修复失败时不要重启，直接跑 xp-filter-setup.exe uninstall。
- 本包只含脚本，不含重新编译的 setup.exe / 驱动；安装器源码已同步修正
  （SERVICE_BOOT_START + Group + install/repair 顺序 + 回读校验）。
