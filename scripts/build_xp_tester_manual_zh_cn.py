from pathlib import Path

from docx import Document
from docx.oxml.ns import qn


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "docs" / "Ruisi-USB-DLP-XP-Tester-Manual.docx"
OUTPUT = ROOT / "docs" / "Ruisi-USB-DLP-XP-Tester-Manual-zh-CN.docx"
ZH_FONT = "Microsoft YaHei"


TRANSLATIONS = {
    "TEST HANDOFF  |  WINDOWS XP SP3 X86": "测试交接  |  WINDOWS XP SP3 X86",
    "Installation and Hardware Qualification Manual": "安装与硬件验证手册",
    "LAB USE ONLY: ": "仅限实验室使用：",
    "This package enables live default-deny USB enforcement. Use only in an isolated test environment, on disposable test media, and from a VM snapshot. It is unsigned and is not approved for production deployment.": "此软件包将启用实时的 USB 默认拒绝管控。只能在隔离的测试环境中，使用可废弃的测试介质并基于虚拟机快照进行测试。本软件包未签名，不得用于生产部署。",
    "Prepared 29 August 2026": "编制日期：2026年8月29日",
    "1. Purpose and supported scope": "1. 目的与支持范围",
    "This manual gives an external qualification team a repeatable procedure for installing the native XP server and workstation agent, proving default-deny behavior with physical USB storage, exercising request approval and revocation, and returning usable evidence to the development team.": "本手册为外部验证团队提供一套可重复执行的流程，用于安装原生 XP 服务端和工作站代理，使用实体 USB 存储设备验证默认拒绝行为，演练申请审批与撤销，并向开发团队提交可用的测试证据。",
    "Supported target: 32-bit Windows XP Professional SP3 installed on NTFS.": "支持的目标：安装在 NTFS 系统卷上的 32 位 Windows XP Professional SP3。",
    "Unsupported targets: XP x64, XP before SP3, and any newer Windows release using this XP-specific MSI.": "不支持的目标：XP x64、SP3 之前的 XP，以及使用此 XP 专用 MSI 的任何较新 Windows 版本。",
    "Server and agent are separate products. A small lab may install both on one XP VM; a representative test uses separate server and workstation VMs.": "服务端与代理是两个独立产品。小型实验室可将二者安装在同一台 XP 虚拟机中；具有代表性的测试应使用独立的服务端和工作站虚拟机。",
    "The agent installs with DryRun=0 and default deny. Unapproved USB storage can be disabled immediately after installation.": "代理安装时设置 DryRun=0 并启用默认拒绝。未经批准的 USB 存储设备可能在安装完成后立即被禁用。",
    "The legacy transport is HTTP with Basic authentication. It is permitted only on an isolated test network.": "旧版传输使用 HTTP 和 Basic 身份验证，仅允许在隔离的测试网络中使用。",
    "Qualification boundary: ": "验证边界：",
    "The build, MSI installation, service startup, enrollment, and policy download have been exercised on XP. Physical USB blocking, notifier behavior, approval, revocation, service-down behavior, and multi-volume media remain external hardware qualification items.": "构建、MSI 安装、服务启动、注册和策略下载已在 XP 上执行过。实体 USB 阻断、通知器行为、审批、撤销、服务停止时的行为以及多卷介质仍属于外部硬件验证项目。",
    "2. Package contents": "2. 软件包内容",
    "3. Lab preparation": "3. 实验室准备",
    "Create restore points. ": "创建还原点。",
    "Take powered-off VM snapshots of the XP server and workstation before installation.": "安装前，在虚拟机关机状态下分别为 XP 服务端和工作站创建快照。",
    "Isolate the network. ": "隔离网络。",
    "Use a host-only or otherwise isolated test segment. Do not bridge port 5080 to an untrusted network.": "使用仅主机网络或其他隔离的测试网段。不得将 5080 端口桥接到不受信任的网络。",
    "Prepare test media. ": "准备测试介质。",
    "Use a USB stick with no valuable data. Safely eject it and leave it disconnected during installation.": "使用不含重要数据的 USB 闪存盘。安全弹出设备，并在安装期间保持断开。",
    "Confirm prerequisites. ": "确认先决条件。",
    "Verify XP Professional SP3, 32-bit architecture, NTFS system volume, local Administrator access, and correct date/time.": "确认系统为 XP Professional SP3、32 位架构、NTFS 系统卷，并具备本地 Administrator 权限且日期和时间正确。",
    "Prepare passthrough. ": "准备 USB 直通。",
    "Verify the hypervisor can attach the physical USB device to XP before relying on any DLP result.": "在依据任何 DLP 结果作出判断前，先确认虚拟化平台能够将实体 USB 设备连接到 XP。",
    "Copy and verify. ": "复制并校验。",
    "Copy the entire kit to each VM. Verify SHA256SUMS on a modern trusted machine before execution.": "将完整软件包复制到每台虚拟机。执行前，在受信任的现代计算机上校验 SHA256SUMS。",
    "USB passthrough: ": "USB 直通：",
    "A hypervisor capture failure is not an agent result. First prove that XP receives a USB or USBSTOR Plug and Play node. VirtualBox on some Windows 11 xHCI hosts may claim a device without delivering it to the guest; a USB 2.0 hub or different host may be required.": "虚拟化平台捕获失败不属于代理测试结果。首先应证明 XP 收到了 USB 或 USBSTOR 即插即用节点。在某些 Windows 11 xHCI 主机上，VirtualBox 可能显示已占用设备，却未将设备交付给来宾系统；此时可能需要 USB 2.0 集线器或另一台主机。",
    "4. Configure the kit": "4. 配置软件包",
    "4.1 Replace both placeholders": "4.1 替换两个占位值",
    "Replace config\\enrollment-token.txt with one random ASCII line of at least 32 characters. The server and every workstation in this test deployment must use the identical file.": "将 config\\enrollment-token.txt 的内容替换为一行至少 32 个字符的随机 ASCII 字符串。此测试部署中的服务端和每台工作站必须使用完全相同的文件。",
    "Replace config\\admin-password.txt with one unique ASCII line of at least 14 characters. Do not reuse a production password.": "将 config\\admin-password.txt 的内容替换为一行至少 14 个字符的唯一 ASCII 字符串。不得重复使用生产环境密码。",
    "Keep both files access-controlled. Do not paste their values into command lines, screenshots, tickets, or test logs.": "对这两个文件实施访问控制。不得将其内容粘贴到命令行、截图、工单或测试日志中。",
    "Important: ": "重要：",
    "The shipped values are deliberate placeholders and must not be used as credentials.": "随软件包提供的值是特意设置的占位符，不得作为凭据使用。",
    "4.2 Edit deployment.ini": "4.2 编辑 deployment.ini",
    "5. Install the server": "5. 安装服务端",
    "Open an elevated prompt. ": "打开提升权限的命令提示符。",
    "Sign in as a local Administrator and open Command Prompt.": "以本地 Administrator 身份登录并打开命令提示符。",
    "Open the server folder. ": "打开服务端文件夹。",
    "Change directory to the kit's server folder.": "将当前目录切换到软件包的 server 文件夹。",
    "Run the wrapper. ": "运行安装包装脚本。",
    "Execute install-server.cmd. Do not launch the MSI directly because the wrapper validates and protects the configuration files.": "执行 install-server.cmd。不要直接启动 MSI，因为包装脚本会验证并保护配置文件。",
    "Open the firewall only in the lab. ": "仅在实验室中开放防火墙。",
    "If client and server are separate machines, allow the configured TCP port for the isolated subnet.": "如果客户端与服务端位于不同计算机，请仅为隔离网段放行已配置的 TCP 端口。",
    "Verify the service. ": "验证服务。",
    "Confirm UsbDlp.LegacyServer reports STATE 4 RUNNING.": "确认 UsbDlp.LegacyServer 报告 STATE 4 RUNNING。",
    "Expected result: ": "预期结果：",
    "The wrapper prints that the server was installed on the configured HTTP port. The service is RUNNING and the data directory is protected under All Users Application Data\\UsbDlp.": "包装脚本显示服务端已安装并监听配置的 HTTP 端口。服务状态为 RUNNING，数据目录位于 All Users Application Data\\UsbDlp 下并受到保护。",
    "6. Install the workstation agent": "6. 安装工作站代理",
    "Keep USB storage disconnected. ": "保持 USB 存储设备断开。",
    "No removable test media should be attached while live enforcement is installed.": "安装实时管控功能时，不得连接任何可移动测试介质。",
    "Confirm server reachability. ": "确认服务端可达。",
    "From the workstation, ping the server address and confirm the isolated firewall path is correct.": "从工作站 ping 服务端地址，确认隔离网络中的防火墙路径正确。",
    "Run the wrapper as Administrator. ": "以 Administrator 身份运行包装脚本。",
    "Execute client\\install-client.cmd. The wrapper installs the exact XP MSI, protects agent data, stages the token, writes DryRun=0, and restarts the service.": "执行 client\\install-client.cmd。包装脚本将安装匹配的 XP MSI、保护代理数据、部署令牌、写入 DryRun=0，并重启服务。",
    "Verify service and enrollment. ": "验证服务与注册。",
    "Confirm the agent is RUNNING, then inspect the log for dry_run=false, enrollment_succeeded, and policy_applied version=1.": "确认代理状态为 RUNNING，然后检查日志中是否包含 dry_run=false、enrollment_succeeded 和 policy_applied version=1。",
    "Fail closed at startup: ": "启动时默认拒绝：",
    "The agent begins with default deny even before a policy is downloaded. A missing server does not turn the agent into dry-run mode.": "即使尚未下载策略，代理启动时也会执行默认拒绝。服务端不可用不会使代理切换到 dry-run 模式。",
    "7. Execute the core hardware workflow": "7. 执行核心硬件测试流程",
    "7.1 Block and request access": "7.1 阻断并申请访问权限",
    "Start evidence capture. ": "开始采集证据。",
    "Record time, VM snapshot name, USB manufacturer/model, VID/PID, serial number if available, and whether the device has multiple partitions.": "记录时间、虚拟机快照名称、USB 制造商/型号、VID/PID、序列号（如有），以及设备是否包含多个分区。",
    "Insert the unapproved USB stick. ": "插入未获批准的 USB 闪存盘。",
    "Attach it to the XP workstation through the hypervisor. Do not open or write files.": "通过虚拟化平台将其连接到 XP 工作站。不要打开文件或写入数据。",
    "Verify the block. ": "验证阻断。",
    "Within the expected interval, the notifier should appear with device details and no usable removable drive should remain.": "在预期时间内，通知器应显示设备详细信息，并且系统中不应保留任何可用的可移动磁盘。",
    "Submit a request. ": "提交申请。",
    "Select 1, 7, 30, or 90 days; enter a 10-2000 character business justification; select Submit request.": "选择 1、7、30 或 90 天；输入 10-2000 个字符的业务理由；单击 Submit request。",
    "Verify dismissal. ": "验证窗口关闭。",
    "The notifier should report submission and close automatically. A duplicate pending request should state that a recent request exists and also close.": "通知器应报告提交成功并自动关闭。如果存在重复的待处理申请，应提示近期申请已存在并同样自动关闭。",
    "7.2 Approve the request": "7.2 批准申请",
    "List requests on the server. ": "在服务端列出申请。",
    "Run server\\admin-server.cmd --requests from an elevated prompt.": "在提升权限的命令提示符中运行 server\\admin-server.cmd --requests。",
    "Copy the pending request ID. ": "复制待处理申请 ID。",
    "Match the workstation, device, and user; do not approve an unrelated request.": "核对工作站、设备和用户，不得批准无关申请。",
    "Approve. ": "批准。",
    "Run admin-server.cmd --approve REQUEST_ID 30, replacing REQUEST_ID with the exact value.": "运行 admin-server.cmd --approve REQUEST_ID 30，并将 REQUEST_ID 替换为准确值。",
    "Wait for convergence. ": "等待策略收敛。",
    "Allow up to one 30-second policy poll. If XP does not surface the device while attached, unplug and replug it once.": "等待最多一个 30 秒策略轮询周期。如果设备保持连接时 XP 未显示该设备，请拔出后重新插入一次。",
    "Verify use. ": "验证使用。",
    "The device should be enabled and its expected partition or partitions should become accessible.": "设备应被启用，其预期的一个或多个分区应可以访问。",
    "Why the wrapper restarts the server: ": "包装脚本为何重启服务端：",
    "The local compatibility CLI updates encrypted persisted state. admin-server.cmd briefly stops the service, applies the command, and restarts it so agents immediately receive the new policy version.": "本地兼容性 CLI 会更新加密的持久化状态。admin-server.cmd 会短暂停止服务、应用命令并重新启动服务，使代理立即收到新的策略版本。",
    "7.3 Revoke and deny": "7.3 撤销与拒绝",
    "Revoke the approved request. ": "撤销已批准的申请。",
    "Run admin-server.cmd --revoke REQUEST_ID.": "运行 admin-server.cmd --revoke REQUEST_ID。",
    "Wait for policy convergence. ": "等待策略收敛。",
    "Allow up to 30 seconds. If necessary, unplug and replug once.": "最多等待 30 秒。如有必要，拔出后重新插入一次。",
    "Verify renewed blocking. ": "验证重新阻断。",
    "No partition from the revoked device should remain usable. Multi-volume media must expose no usable partition.": "被撤销设备的任何分区均不得继续可用。多卷介质不得暴露任何可用分区。",
    "Exercise denial. ": "演练拒绝。",
    "Create a fresh pending request with another device or after test-state reset, then run admin-server.cmd --deny REQUEST_ID and confirm access remains blocked.": "使用另一台设备或在重置测试状态后创建新的待处理申请，然后运行 admin-server.cmd --deny REQUEST_ID，并确认访问仍被阻断。",
    "8. Acceptance checklist": "8. 验收检查表",
    "[ ] Both MSI files install only on XP SP3 x86 and reject the wrong OS profile.": "[ ] 两个 MSI 文件均只能安装在 XP SP3 x86 上，并会拒绝错误的操作系统配置。",
    "[ ] Server and agent services are RUNNING after fresh install and after rerunning each wrapper.": "[ ] 全新安装后以及重新运行各包装脚本后，服务端和代理服务均为 RUNNING。",
    "[ ] Agent log reports dry_run=false, default_deny=true, enrollment_succeeded, and policy_applied.": "[ ] 代理日志报告 dry_run=false、default_deny=true、enrollment_succeeded 和 policy_applied。",
    "[ ] An unapproved USB storage device is disabled and exposes no usable partition.": "[ ] 未获批准的 USB 存储设备被禁用，且不暴露任何可用分区。",
    "[ ] The notifier appears only for a newly blocked device and displays friendly name, instance ID, VID, and PID.": "[ ] 通知器仅在新设备被阻断时出现，并显示友好名称、实例 ID、VID 和 PID。",
    "[ ] Submit remains disabled for fewer than 10 justification characters and works at 10-2000 characters.": "[ ] 业务理由少于 10 个字符时提交按钮保持禁用，达到 10-2000 个字符时可以提交。",
    "[ ] The notifier closes after successful or duplicate-pending submission.": "[ ] 成功提交或检测到重复待处理申请后，通知器自动关闭。",
    "[ ] Approval is scoped to the exact workstation and device instance and converges within one policy poll or one replug.": "[ ] 批准仅适用于准确匹配的工作站和设备实例，并在一次策略轮询或一次重新插入后收敛。",
    "[ ] Revocation blocks the attached device or blocks it on the first replug after policy convergence.": "[ ] 撤销后，已连接设备立即被阻断，或在策略收敛后的第一次重新插入时被阻断。",
    "[ ] Multi-volume USB media exposes no usable partition while blocked.": "[ ] 多卷 USB 介质被阻断时不暴露任何可用分区。",
    "[ ] Keyboard, mouse, webcam, audio, internal disks, hubs, and controllers remain usable.": "[ ] 键盘、鼠标、摄像头、音频设备、内部磁盘、集线器和控制器保持可用。",
    "[ ] Stopping the XP agent is recorded as legacy_backstop_unavailable; no full service-down fail-closed claim is made.": "[ ] 停止 XP 代理时记录 legacy_backstop_unavailable；不宣称服务停止后仍能完全保持默认拒绝。",
    "[ ] No password or enrollment token appears in process listings, command output, or collected logs.": "[ ] 进程列表、命令输出和收集的日志中均不出现密码或注册令牌。",
    "9. Diagnostics and evidence collection": "9. 诊断与证据收集",
    "Secret handling: ": "机密信息处理：",
    "Do not collect admin-password.txt, enrollment-token.txt, bootstrap-token.txt, identity.dat, policy.dat, or server-state.dat in an ordinary ticket. If encrypted state is required, transfer it through an approved secure channel.": "不得在普通工单中收集 admin-password.txt、enrollment-token.txt、bootstrap-token.txt、identity.dat、policy.dat 或 server-state.dat。如需加密状态文件，必须通过经批准的安全通道传输。",
    "Record exact timestamps and time zone for each insertion, request, decision, and observed state change.": "记录每次插入、申请、审批决定和观察到的状态变化的准确时间戳与时区。",
    "Record whether the physical device reached XP. A missing USB or USBSTOR node is a hypervisor passthrough failure, not a DLP enforcement result.": "记录实体设备是否到达 XP。缺少 USB 或 USBSTOR 节点表示虚拟化平台直通失败，不属于 DLP 管控结果。",
    "For each failure, include the previous successful step and whether unplug/replug, service restart, or VM restart changed the outcome.": "对于每项失败，记录此前最后一个成功步骤，并说明拔出/重新插入、重启服务或重启虚拟机是否改变结果。",
    "Return the completed acceptance checklist, screenshots, redacted logs, package hash results, VM details, and device inventory.": "提交填写完整的验收检查表、截图、脱敏日志、软件包哈希校验结果、虚拟机详细信息和设备清单。",
    "10. Uninstall and rollback": "10. 卸载与回滚",
    "Disconnect test media. ": "断开测试介质。",
    "Safely eject or unplug all removable storage before uninstalling.": "卸载前，安全弹出或拔出所有可移动存储设备。",
    "Uninstall the agent. ": "卸载代理。",
    "Use Control Panel > Add or Remove Programs > Ruisi USB DLP Legacy Agent.": "依次打开 Control Panel > Add or Remove Programs > Ruisi USB DLP Legacy Agent。",
    "Uninstall the server. ": "卸载服务端。",
    "Use Control Panel > Add or Remove Programs > Ruisi USB DLP Legacy Server.": "依次打开 Control Panel > Add or Remove Programs > Ruisi USB DLP Legacy Server。",
    "Verify services are gone. ": "确认服务已移除。",
    "Run both sc query commands and confirm each named service no longer exists.": "运行两条 sc query 命令，确认对应的服务均已不存在。",
    "Preserve evidence. ": "保留证据。",
    "The protected Application Data directory may remain for audit and reinstall. Do not delete it until evidence has been secured.": "受保护的 Application Data 目录可能会保留，以便审计和重新安装。在证据得到妥善保存前，不要删除该目录。",
    "Restore the snapshot. ": "还原快照。",
    "For a clean test rerun, restore the powered-off pre-install VM snapshots.": "如需重新执行干净测试，请还原安装前在关机状态下创建的虚拟机快照。",
    "XP service-down limitation: ": "XP 服务停止限制：",
    "XP lacks the modern layered installation backstop. A newly attached device can become usable while the agent service is stopped. This limitation is expected and must be reported, not hidden or described as fully fail-closed.": "XP 缺少现代系统中的分层安装防护机制。代理服务停止时，新连接的设备可能变为可用。这是已知限制，必须如实报告，不得隐瞒或描述为服务停止后仍能完全保持默认拒绝。",
    "11. Test report summary": "11. 测试报告摘要",
    "End of manual": "手册结束",
    "Package": "软件包",
    "Target": "目标平台",
    "Status": "状态",
    "Windows XP Professional SP3, x86, NTFS": "Windows XP Professional SP3，x86，NTFS",
    "UNVERIFIED compatibility build": "未验证的兼容性构建",
    "Path": "路径",
    "Purpose": "用途",
    "Native XP agent and on-demand notifier": "原生 XP 代理和按需通知器",
    "Validated live-mode agent installation wrapper": "经过验证的实时模式代理安装包装脚本",
    "Native XP management/API server": "原生 XP 管理/API 服务端",
    "Validated server configuration and installation wrapper": "经过验证的服务端配置和安装包装脚本",
    "Safe local request/approval/revocation wrapper": "安全的本地申请/审批/撤销包装脚本",
    "Editable server URL, port, and profile": "可编辑的服务端 URL、端口和配置文件",
    "Shared bootstrap token; replace before installation": "共享引导令牌；安装前必须替换",
    "Administrator password; replace before installation": "管理员密码；安装前必须替换",
    "Integrity hashes for every file in the kit": "软件包中每个文件的完整性哈希",
    "Key": "键",
    "Required value": "必需值",
    "HTTP URL reachable from the XP workstation, for example http://192.168.77.20:5080": "XP 工作站可访问的 HTTP URL，例如 http://192.168.77.20:5080",
    "Server listener port; keep 5080 unless the isolated lab requires another port": "服务端监听端口；除非隔离实验室需要其他端口，否则保留 5080",
    "legacy-xp-x86; do not change": "legacy-xp-x86；不要更改",
    "Evidence": "证据",
    "Location or command": "位置或命令",
    "Agent log": "代理日志",
    "Agent configuration": "代理配置",
    "Services": "服务",
    "Device state": "设备状态",
    "Requests": "申请",
    "Workstations": "工作站",
    "Windows logs": "Windows 日志",
    "Package integrity": "软件包完整性",
    "...\\Application Data\\UsbDlp\\legacy-agent.ini; redact paths if needed": "...\\Application Data\\UsbDlp\\legacy-agent.ini；必要时对路径进行脱敏",
    "sc query UsbDlp.LegacyAgent and sc query UsbDlp.LegacyServer": "sc query UsbDlp.LegacyAgent 和 sc query UsbDlp.LegacyServer",
    "Device Manager screenshots before insertion, blocked, approved, and revoked": "插入前、阻断后、批准后和撤销后的设备管理器截图",
    "Event Viewer System and Application entries at the test timestamps": "测试时间戳对应的事件查看器 System 和 Application 记录",
    "SHA-256 verification output for SHA256SUMS": "SHA256SUMS 的 SHA-256 校验输出",
    "Field": "字段",
    "Tester entry": "测试人员填写",
    "Tester / organization": "测试人员/组织",
    "Date, time zone": "日期、时区",
    "XP edition / service pack": "XP 版本/Service Pack",
    "Hypervisor / version": "虚拟化平台/版本",
    "Server and workstation topology": "服务端与工作站拓扑",
    "USB device model / VID / PID / serial": "USB 设备型号/VID/PID/序列号",
    "Single- or multi-volume": "单卷或多卷",
    "Overall result": "总体结果",
    "PASS / FAIL / BLOCKED BY LAB": "通过 / 失败 / 受实验室条件阻碍",
    "Failed checklist item IDs": "失败的检查项 ID",
    "Evidence archive name / SHA-256": "证据归档名称/SHA-256",
    "RUISI USB DLP  |  Windows XP SP3 x86 test handoff": "RUISI USB DLP  |  Windows XP SP3 x86 测试交接",
    "  |  Windows XP SP3 x86 test handoff": "  |  Windows XP SP3 x86 测试交接",
    "Version 1.1.0  |  UNVERIFIED  |  Page ": "版本 1.1.0  |  未验证  |  第 ",
}


def set_east_asia_font(run, font_name=ZH_FONT):
    r_fonts = run._element.get_or_add_rPr().get_or_add_rFonts()
    r_fonts.set(qn("w:eastAsia"), font_name)
    if run.text in TRANSLATIONS.values():
        run.font.name = font_name
        r_fonts.set(qn("w:ascii"), font_name)
        r_fonts.set(qn("w:hAnsi"), font_name)


def translate_paragraph(paragraph):
    for run in paragraph.runs:
        if run.text in TRANSLATIONS:
            run.text = TRANSLATIONS[run.text]
        if any("\u4e00" <= char <= "\u9fff" for char in run.text):
            set_east_asia_font(run)


def iter_all_paragraphs(document):
    yield from document.paragraphs
    for table in document.tables:
        for row in table.rows:
            for cell in row.cells:
                yield from cell.paragraphs
    for section in document.sections:
        for part in (
            section.header,
            section.even_page_header,
            section.footer,
            section.even_page_footer,
        ):
            yield from part.paragraphs


def configure_chinese_styles(document):
    for style_name in ("Normal", "Heading 1", "Heading 2", "Heading 3"):
        style = document.styles[style_name]
        style.font.name = ZH_FONT
        style._element.get_or_add_rPr().get_or_add_rFonts().set(
            qn("w:eastAsia"), ZH_FONT
        )


def remove_localization_page_break(document):
    paragraphs = list(document.paragraphs)
    for index, paragraph in enumerate(paragraphs[:-1]):
        next_text = paragraphs[index + 1].text
        page_breaks = paragraph._p.xpath('.//w:br[@w:type="page"]')
        if not paragraph.text and page_breaks and next_text == "4.2 Edit deployment.ini":
            paragraph._element.getparent().remove(paragraph._element)
            return
    raise RuntimeError("Expected page break before section 4.2 was not found")


def use_shared_page_furniture(document):
    document.settings.odd_and_even_pages_header_footer = False
    for section in document.sections:
        section.header.is_linked_to_previous = False
        section.footer.is_linked_to_previous = False
        section_properties = section._sectPr
        for reference in list(section_properties.xpath('./w:headerReference')):
            if reference.get(qn("w:type")) == "even":
                section_properties.remove(reference)
        for reference in list(section_properties.xpath('./w:footerReference')):
            if reference.get(qn("w:type")) == "even":
                section_properties.remove(reference)


def build_manual():
    if not SOURCE.exists():
        raise FileNotFoundError(f"English source manual not found: {SOURCE}")

    document = Document(SOURCE)
    remove_localization_page_break(document)
    use_shared_page_furniture(document)
    configure_chinese_styles(document)
    for paragraph in iter_all_paragraphs(document):
        translate_paragraph(paragraph)

    for section in document.sections:
        paragraph = section.footer.paragraphs[0]
        if not any(run.text == " 页" for run in paragraph.runs):
            suffix = paragraph.add_run(" 页")
            set_east_asia_font(suffix)

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    document.save(OUTPUT)
    print(OUTPUT)


if __name__ == "__main__":
    build_manual()
