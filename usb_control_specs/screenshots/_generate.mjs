import { chromium } from 'playwright';
import fs from 'fs';
import path from 'path';

const OUT = path.resolve('.');
const dirs = { en: path.join(OUT, 'en'), zh: path.join(OUT, 'zh') };
for (const d of Object.values(dirs)) fs.mkdirSync(d, { recursive: true });

// ---- inline SVG icons (24x24 stroke) so no web font is needed ----
const ic = {
  shield: '<path d="M12 3l7 3v5c0 4.5-3 7.5-7 9-4-1.5-7-4.5-7-9V6z"/>',
  dashboard: '<path d="M4 4h7v7H4zM13 4h7v4h-7zM13 10h7v10h-7zM4 13h7v7H4z"/>',
  list: '<path d="M9 6h11M9 12h11M9 18h11M4 6h.01M4 12h.01M4 18h.01"/>',
  desktop: '<path d="M3 5h18v11H3zM8 20h8M12 16v4"/>',
  inbox: '<path d="M3 12h5l2 3h4l2-3h5M3 12l3-7h12l3 7v6H3z"/>',
  history: '<path d="M12 8v4l3 2M3 12a9 9 0 1 0 3-6.7L3 8M3 4v4h4"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="M21 21l-4-4"/>',
  plus: '<path d="M12 5v14M5 12h14"/>',
  download: '<path d="M12 4v10M8 11l4 4 4-4M5 19h14"/>',
  usb: '<path d="M12 3l2 3h-4zM12 6v13M12 19a2 2 0 1 0 0 4 2 2 0 0 0 0-4M8 11l4 3 4-5"/>',
  sd: '<path d="M6 3h9l4 4v14H6zM10 6v3M13 6v3M16 7v2"/>',
};
const svg = (p, sz = 18, color = 'currentColor') =>
  `<svg width="${sz}" height="${sz}" viewBox="0 0 24 24" fill="none" stroke="${color}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${p}</svg>`;

const C = { // palette (light theme, from CDS ramps)
  page:'#F1EFE8', card:'#FFFFFF', border:'#D3D1C7', border2:'#B4B2A9',
  ink:'#2C2C2A', mut:'#5F5E5A', mut2:'#888780',
  blueBg:'#E6F1FB', blueTx:'#0C447C', blue:'#185FA5',
  grn:'#3B6D11', grnDot:'#639922', red:'#A32D2D',
  amBg:'#FAEEDA', amTx:'#854F0B', pkBg:'#FBEAF0', pkTx:'#993556',
};

const t = {
  en: {
    appName:'USB device control', sub:'admin console',
    nav:{dashboard:'Dashboard',policies:'Policies',workstations:'Workstations',requests:'Requests',events:'Events'},
    policyMeta:'policy v42 · published 2h ago',
    lang:'EN | 中文',
  },
  zh: {
    appName:'USB 设备管控', sub:'管理控制台',
    nav:{dashboard:'概览',policies:'策略',workstations:'工作站',requests:'申请',events:'事件'},
    policyMeta:'策略 v42 · 2 小时前发布',
    lang:'EN | 中文',
  },
};

const badge = (bg,tx,txt)=>`<span style="background:${bg};color:${tx};padding:2px 8px;border-radius:8px;font-size:11px;white-space:nowrap;">${txt}</span>`;
const mono = s=>`<span style="font-family:'Noto Sans Mono',monospace;font-size:11px;color:${C.mut};overflow:hidden;text-overflow:ellipsis;white-space:nowrap;display:block;">${s}</span>`;

function shell(lang, active, title, contentHtml){
  const L=t[lang]; const nav=L.nav;
  const item=(key,icon)=>{
    const on=key===active;
    return `<div style="display:flex;align-items:center;gap:10px;padding:8px 11px;border-radius:8px;font-size:13px;margin-bottom:2px;${on?`background:${C.blueBg};color:${C.blueTx};font-weight:500;`:`color:${C.mut};`}">${svg(icon,17,on?C.blueTx:C.mut)}<span>${nav[key]}</span>${key==='requests'?` <span style="margin-left:auto;background:${C.amBg};color:${C.amTx};font-size:11px;padding:1px 7px;border-radius:8px;">3</span>`:''}</div>`;
  };
  return `<!doctype html><html lang="${lang==='zh'?'zh-CN':'en'}"><head><meta charset="utf-8">
  <style>
    *{box-sizing:border-box;}
    body{margin:0;background:${C.page};font-family:${lang==='zh'?"'Noto Sans CJK SC',":''}'Helvetica Neue',Arial,sans-serif;color:${C.ink};}
    .frame{width:1040px;margin:0;background:${C.card};border:1px solid ${C.border};border-radius:14px;overflow:hidden;}
    table{width:100%;border-collapse:collapse;font-size:12px;}
    th{font-weight:400;color:${C.mut2};text-align:left;padding:6px 8px;}
    td{padding:7px 8px;border-top:1px solid ${C.border};vertical-align:middle;}
    .metric{background:${C.page};border-radius:8px;padding:11px 13px;}
    .metric p{margin:0;} .ml{font-size:12px;color:${C.mut2};} .mv{font-size:21px;font-weight:500;margin-top:3px;}
    .btn{font-size:12px;padding:6px 13px;border-radius:8px;border:1px solid ${C.border2};color:${C.mut};display:inline-flex;align-items:center;gap:6px;}
    .btnp{background:${C.blue};color:#fff;border:none;}
    .pill{font-size:12px;padding:4px 11px;border-radius:8px;border:1px solid ${C.border};color:${C.mut};}
    .pill.on{background:${C.blueBg};color:${C.blueTx};border-color:${C.blueBg};}
    .card2{background:${C.page};border-radius:8px;padding:13px;}
    h1{font-size:15px;font-weight:500;margin:0;}
  </style></head><body>
  <div class="frame">
    <div style="display:flex;align-items:center;gap:11px;padding:12px 18px;border-bottom:1px solid ${C.border};">
      ${svg(ic.shield,20,C.blue)}
      <span style="font-size:14px;font-weight:500;">${L.appName}</span>
      <span style="font-size:12px;color:${C.mut2};">${L.sub}</span>
      <span style="margin-left:auto;font-size:12px;color:${C.mut};">${L.policyMeta} · ${L.lang}</span>
    </div>
    <div style="display:flex;">
      <div style="width:190px;border-right:1px solid ${C.border};padding:10px;flex-shrink:0;">
        ${item('dashboard',ic.dashboard)}${item('policies',ic.list)}${item('workstations',ic.desktop)}${item('requests',ic.inbox)}${item('events',ic.history)}
      </div>
      <div style="flex:1;padding:18px 20px;min-width:0;">${contentHtml}</div>
    </div>
  </div></body></html>`;
}

// ---------- view content per language ----------
function vDashboard(lang){
  const en=lang==='en';
  const m=[[en?'Agents online':'在线代理',`287<span style="font-size:12px;color:${C.mut2};font-weight:400;">/302</span>`,C.ink],
          [en?'Blocked today':'今日拦截','14',C.red],
          [en?'Pending requests':'待审申请','3',C.amTx],
          [en?'Stale policy >24h':'策略超期>24h','6',C.ink]];
  const rows=[
    [en?'Kingston IronKey D500S':'Kingston IronKey D500S','VID_0951&PID_1741\\002618A2',badge(C.blueBg,C.blueTx,en?'Global':'全局'),`<span style="color:${C.grn}">${en?'Allow':'允许'}</span>`,'j.chen'],
    [en?"Verbatim Store'n'Go":"Verbatim Store'n'Go",'VID_18A5&PID_0302\\*',badge(C.blueBg,C.blueTx,en?'Fab floor':'车间'),`<span style="color:${C.red}">${en?'Block':'拦截'}</span>`,'j.chen'],
    [en?'SanDisk Ultra (loaner #4)':'SanDisk Ultra（借用 #4）','VID_0781&PID_5583\\4C5300…',badge(C.amBg,C.amTx,'WS-ENG-042'),`<span style="color:${C.grn}">${en?'Allow→Sep 1':'允许→9/1'}</span>`,'admin'],
  ];
  return `<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:16px;">
    ${m.map(x=>`<div class="metric"><p class="ml">${x[0]}</p><p class="mv" style="color:${x[2]}">${x[1]}</p></div>`).join('')}
  </div>
  <div style="display:flex;align-items:center;gap:10px;margin-bottom:8px;">
    <span style="font-size:13px;font-weight:500;">${en?'Policy layers':'策略层级'}</span>
    <span style="font-size:12px;color:${C.mut2};">${en?'effective = global → group → workstation override':'生效 = 全局 → 分组 → 工作站覆盖'}</span>
    <span class="btn btnp" style="margin-left:auto;">${svg(ic.plus,13,'#fff')} ${en?'Add device':'添加设备'}</span>
  </div>
  <table><tr><th style="width:26%">${en?'Device':'设备'}</th><th style="width:30%">${en?'Instance ID':'实例 ID'}</th><th style="width:16%">${en?'Scope':'范围'}</th><th style="width:16%">${en?'Action':'动作'}</th><th style="width:12%">${en?'By':'操作人'}</th></tr>
  ${rows.map(r=>`<tr><td>${r[0]}</td><td>${mono(r[1])}</td><td>${r[2]}</td><td>${r[3]}</td><td style="color:${C.mut}">${r[4]}</td></tr>`).join('')}</table>
  <div class="card2" style="margin-top:16px;">
    <div style="display:flex;align-items:center;gap:9px;font-size:13px;">${svg(ic.desktop,16,C.mut)}<span style="font-weight:500;">WS-ENG-042</span><span style="color:${C.mut2};font-size:12px;">${en?'Design Engineering · default-deny':'设计工程组 · 默认拒绝'}</span><span style="margin-left:auto;display:inline-flex;align-items:center;gap:6px;font-size:12px;color:${C.grn};"><span style="width:7px;height:7px;border-radius:50%;background:${C.grnDot};"></span>${en?'online · v42':'在线 · v42'}</span></div>
    <div style="margin-top:9px;font-size:12px;color:${C.mut};display:grid;gap:5px;">
      <div><span style="color:${C.red}">●</span> 09:14 ${en?'Blocked':'已拦截'} — Samsung Galaxy S24 (MTP) · ${en?'user':'用户'} l.wang</div>
      <div><span style="color:${C.grn}">●</span> 08:52 ${en?'Allowed':'已允许'} — Kingston IronKey D500S · ${en?'user':'用户'} l.wang</div>
      <div><span style="color:${C.amTx}">●</span> 08:47 ${en?'Request':'申请'} — SanDisk Ultra ${en?'"flash test board"':'“烧录测试板”'} · ${en?'pending':'待审批'}</div>
    </div>
  </div>`;
}

function vPolicies(lang){
  const en=lang==='en';
  const rows=[
    [en?'Kingston IronKey D500S':'Kingston IronKey D500S',en?'vid+pid+serial':'vid+pid+序列号','0951/1741/002618A2',badge(C.blueBg,C.blueTx,en?'Global':'全局'),`<span style="color:${C.grn}">${en?'Allow':'允许'}</span>`,'—'],
    [en?'All mass storage':'所有大容量存储',en?'device class':'设备类','USBSTOR',badge(C.pkBg,C.pkTx,en?'Design Eng':'设计工程'),`<span style="color:${C.red}">${en?'Block':'拦截'}</span>`,'—'],
    [en?"Verbatim Store'n'Go":"Verbatim Store'n'Go",en?'vid+pid (any serial)':'vid+pid（任意序列号）','18A5/0302/*',badge(C.pkBg,C.pkTx,en?'Fab floor':'车间'),`<span style="color:${C.red}">${en?'Block':'拦截'}</span>`,'—'],
    [en?'SanDisk Ultra (loaner #4)':'SanDisk Ultra（借用 #4）',en?'vid+pid+serial':'vid+pid+序列号','0781/5583/4C5300…',badge(C.amBg,C.amTx,'WS-ENG-042'),`<span style="color:${C.grn}">${en?'Allow':'允许'}</span>`,`<span style="color:${C.amTx}">${en?'Sep 1':'9/1'}</span>`],
  ];
  return `<div style="display:flex;align-items:center;gap:9px;margin-bottom:14px;flex-wrap:wrap;">
    <h1>${en?'Device policies':'设备策略'}</h1>
    <span class="pill on">${en?'All scopes':'全部范围'}</span><span class="pill">${en?'Global':'全局'}</span><span class="pill">${en?'Group':'分组'}</span><span class="pill">${en?'Workstation':'工作站'}</span>
    <span class="btn btnp" style="margin-left:auto;">${svg(ic.plus,13,'#fff')} ${en?'Add device':'添加设备'}</span>
  </div>
  <table><tr><th style="width:22%">${en?'Device':'设备'}</th><th style="width:15%">${en?'Match type':'匹配方式'}</th><th style="width:22%">${en?'Value':'值'}</th><th style="width:14%">${en?'Scope':'范围'}</th><th style="width:13%">${en?'Action':'动作'}</th><th style="width:12%">${en?'Expires':'到期'}</th></tr>
  ${rows.map(r=>`<tr><td>${r[0]}</td><td style="color:${C.mut}">${r[1]}</td><td>${mono(r[2])}</td><td>${r[3]}</td><td>${r[4]}</td><td>${r[5]}</td></tr>`).join('')}</table>
  <p style="margin-top:12px;font-size:11px;color:${C.mut2};">${en?'Precedence: instance-id > vid+pid+serial > vid+pid > class · block beats allow within a tier · no match falls to group mode':'优先级：实例 ID > vid+pid+序列号 > vid+pid > 设备类 · 同级中拦截优先于允许 · 无匹配时按分组模式'}</p>`;
}

function vWorkstations(lang){
  const en=lang==='en';
  const rows=[
    ['WS-ENG-042',en?'Design Eng · deny':'设计工程 · 拒绝',en?'online':'在线',C.grn,'v42',C.ink,en?'just now':'刚刚','1.0.3'],
    ['WS-ENG-051',en?'Design Eng · deny':'设计工程 · 拒绝',en?'online':'在线',C.grn,'v42',C.ink,en?'1 min ago':'1 分钟前','1.0.3'],
    ['WS-FAB-118',en?'Fab floor · allow':'车间 · 允许',en?'online':'在线',C.grn,'v42',C.ink,en?'3 min ago':'3 分钟前','1.0.3'],
    ['WS-OFF-207',en?'Office · allow':'办公 · 允许',en?'offline':'离线',C.red,'v39',C.amTx,en?'2 days ago':'2 天前','1.0.2'],
  ];
  return `<div style="display:flex;align-items:center;gap:10px;margin-bottom:14px;">
    <h1>${en?'Workstations':'工作站'}</h1><span style="font-size:12px;color:${C.mut2};">${en?'302 total · 287 online':'共 302 · 在线 287'}</span>
    <span class="btn" style="margin-left:auto;">${svg(ic.search,13,C.mut)} ${en?'Search hostname':'搜索主机名'}</span>
  </div>
  <table><tr><th style="width:19%">${en?'Hostname':'主机名'}</th><th style="width:23%">${en?'Group':'分组'}</th><th style="width:13%">${en?'Status':'状态'}</th><th style="width:11%">${en?'Policy':'策略'}</th><th style="width:19%">${en?'Last seen':'最近在线'}</th><th style="width:12%">${en?'Agent':'代理'}</th></tr>
  ${rows.map(r=>`<tr><td>${r[0]}</td><td style="color:${C.mut}">${r[1]}</td><td style="color:${r[3]}">● ${r[2]}</td><td style="color:${r[5]}">${r[4]}</td><td style="color:${C.mut}">${r[6]}</td><td style="color:${C.mut}">${r[7]}</td></tr>`).join('')}</table>
  <div class="card2" style="margin-top:14px;">
    <div style="font-size:13px;font-weight:500;margin-bottom:8px;display:flex;">WS-ENG-042 <span style="font-weight:400;color:${C.mut2};font-size:12px;margin-left:6px;">— ${en?'detail':'详情'}</span><span style="margin-left:auto;font-size:12px;color:${C.blue};">${en?'Change group · Add override':'调整分组 · 添加覆盖'}</span></div>
    <div style="font-size:12px;color:${C.mut};display:grid;gap:4px;">
      <div><span style="color:${C.red}">●</span> 09:14 ${en?'Blocked':'已拦截'} — Samsung Galaxy S24 (MTP) · l.wang</div>
      <div><span style="color:${C.grn}">●</span> 08:52 ${en?'Allowed':'已允许'} — Kingston IronKey D500S · l.wang</div>
      <div><span style="color:${C.blue}">●</span> 08:30 ${en?'Policy applied':'策略已应用'} — v42</div>
    </div>
  </div>`;
}

function vRequests(lang){
  const en=lang==='en';
  const c1=`<div style="border:1px solid ${C.border};border-radius:8px;padding:13px;margin-bottom:11px;">
    <div style="display:flex;gap:11px;align-items:flex-start;">${svg(ic.usb,18,C.amTx)}
      <div style="flex:1;min-width:0;">
        <div style="font-size:13px;font-weight:500;">SanDisk Ultra USB 3.0 <span style="font-weight:400;color:${C.mut2};">· l.wang · WS-ENG-042</span></div>
        ${mono('VID_0781&PID_5583\\4C530001230904')}
        <div style="font-size:12px;color:${C.mut};margin-top:6px;">${en?'"Need to flash a test board for the bring-up tomorrow." · 08:47':'“需要给明天调试的测试板烧录固件。” · 08:47'}</div>
      </div>
      <div style="display:flex;flex-direction:column;gap:7px;"><span class="btn btnp" style="justify-content:center;">${en?'Approve':'批准'}</span><span class="btn" style="justify-content:center;">${en?'Deny':'驳回'}</span></div>
    </div>
    <div style="margin-top:11px;padding-top:11px;border-top:1px solid ${C.border};font-size:12px;color:${C.mut};display:flex;gap:16px;flex-wrap:wrap;">
      <span style="color:${C.mut2};">${en?'On approve:':'批准后：'}</span>
      <span>${en?'Scope:':'范围：'} <span style="color:${C.blueTx}">${en?'This workstation ▾':'此工作站 ▾'}</span></span>
      <span>${en?'Action:':'动作：'} <span style="color:${C.grn}">${en?'Allow':'允许'}</span></span>
      <span>${en?'Expires:':'到期：'} <span style="color:${C.amTx}">${en?'30 days ▾':'30 天 ▾'}</span></span>
    </div></div>`;
  const c2=`<div style="border:1px solid ${C.border};border-radius:8px;padding:13px;">
    <div style="display:flex;gap:11px;align-items:flex-start;">${svg(ic.sd,18,C.amTx)}
      <div style="flex:1;min-width:0;">
        <div style="font-size:13px;font-weight:500;">${en?'Generic SD reader':'通用读卡器'} <span style="font-weight:400;color:${C.mut2};">· m.tan · WS-FAB-118</span></div>
        <div style="font-size:12px;color:${C.mut};margin-top:6px;">${en?'"Transfer inspection photos off the scope camera." · 08:20':'“从显微镜相机导出检测照片。” · 08:20'}</div>
      </div>
      <div style="display:flex;flex-direction:column;gap:7px;"><span class="btn btnp" style="justify-content:center;">${en?'Approve':'批准'}</span><span class="btn" style="justify-content:center;">${en?'Deny':'驳回'}</span></div>
    </div></div>`;
  return `<div style="display:flex;align-items:center;gap:9px;margin-bottom:14px;"><h1>${en?'Access requests':'访问申请'}</h1>${badge(C.amBg,C.amTx,en?'3 pending':'3 项待审')}</div>${c1}${c2}`;
}

function vEvents(lang){
  const en=lang==='en';
  const rows=[
    ['09:14:02',en?'blocked':'已拦截',C.red,'WS-ENG-042','l.wang','Samsung Galaxy S24 (MTP)','v42'],
    ['08:52:41',en?'allowed':'已允许',C.grn,'WS-ENG-042','l.wang','Kingston IronKey D500S','v42'],
    ['08:47:10',en?'request':'申请',C.amTx,'WS-ENG-042','l.wang','SanDisk Ultra USB 3.0','v42'],
    ['08:30:55',en?'policy_applied':'策略已应用',C.blue,'WS-ENG-042','—','—','v39→v42'],
    ['07:15:33',en?'tamper_hint':'疑似篡改',C.pkTx,'WS-OFF-207','—',en?'service stop (non-SYSTEM)':'服务被非 SYSTEM 停止','v39'],
  ];
  return `<div style="display:flex;align-items:center;gap:9px;margin-bottom:14px;flex-wrap:wrap;">
    <h1>${en?'Events':'事件'}</h1>
    <span class="pill">${en?'Type: all ▾':'类型：全部 ▾'}</span><span class="pill">${en?'Last 24h ▾':'近 24 小时 ▾'}</span><span class="pill">${en?'Workstation ▾':'工作站 ▾'}</span>
    <span class="btn" style="margin-left:auto;">${svg(ic.download,13,C.mut)} ${en?'Export CSV':'导出 CSV'}</span>
  </div>
  <table><tr><th style="width:14%">${en?'Time':'时间'}</th><th style="width:14%">${en?'Type':'类型'}</th><th style="width:17%">${en?'Workstation':'工作站'}</th><th style="width:12%">${en?'User':'用户'}</th><th style="width:29%">${en?'Device':'设备'}</th><th style="width:14%">${en?'Policy':'策略'}</th></tr>
  ${rows.map(r=>`<tr><td style="color:${C.mut}">${r[0]}</td><td style="color:${r[2]}">${r[1]}</td><td>${r[3]}</td><td style="color:${C.mut}">${r[4]}</td><td style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${r[5]}</td><td style="color:${C.mut}">${r[6]}</td></tr>`).join('')}</table>`;
}

const views = {
  '1_dashboard':{active:'dashboard',fn:vDashboard},
  '2_policies':{active:'policies',fn:vPolicies},
  '3_workstations':{active:'workstations',fn:vWorkstations},
  '4_requests':{active:'requests',fn:vRequests},
  '5_events':{active:'events',fn:vEvents},
};

const browser = await chromium.launch();
const page = await browser.newPage({ deviceScaleFactor: 2, viewport:{width:1080,height:800} });
for (const lang of ['en','zh']){
  for (const [name,v] of Object.entries(views)){
    const html = shell(lang, v.active, name, v.fn(lang));
    const htmlPath = path.join(dirs[lang], name+'.html');
    fs.writeFileSync(htmlPath, html);
    await page.goto('file://'+htmlPath);
    await page.waitForTimeout(150);
    const el = await page.$('.frame');
    await el.screenshot({ path: path.join(dirs[lang], name+'.png') });
    console.log('wrote', lang, name);
  }
}
await browser.close();
console.log('DONE');
