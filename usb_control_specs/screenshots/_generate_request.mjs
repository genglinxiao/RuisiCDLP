import { chromium } from 'playwright';
import fs from 'fs';
import path from 'path';

const OUT = path.resolve('.');
const dirs = { en: path.join(OUT, 'en'), zh: path.join(OUT, 'zh') };

const C = { desk:'#DBD9D0', card:'#FFFFFF', border:'#D3D1C7', border2:'#B4B2A9',
  ink:'#2C2C2A', mut:'#5F5E5A', mut2:'#888780', blue:'#185FA5', blueTx:'#0C447C',
  amTx:'#854F0B', field:'#FBFAF6', grn:'#3B6D11' };

const svg=(p,sz,col)=>`<svg width="${sz}" height="${sz}" viewBox="0 0 24 24" fill="none" stroke="${col}" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${p}</svg>`;
const usbIcon='<path d="M12 3l2 3h-4zM12 6v13M12 19a2 2 0 1 0 0 4 2 2 0 0 0 0-4M8 11l4 3 4-5"/>';

function dialog(lang){
  const en = lang==='en';
  const T = en ? {
    title:'Request USB device access', close:'',
    devLabel:'Device', wsLabel:'Workstation', userLabel:'Requested by',
    durLabel:'Access needed for', dur:'30 days',
    just:'Reason for access', ph:'e.g. Flash firmware to the test board for tomorrow’s bring-up',
    note:'Your request goes to IT Security for approval. This device stays blocked until approved. The request and this device are logged.',
    cancel:'Cancel', submit:'Submit request',
  } : {
    title:'申请 USB 设备访问', close:'',
    devLabel:'设备', wsLabel:'工作站', userLabel:'申请人',
    durLabel:'需要使用时长', dur:'30 天',
    just:'申请理由', ph:'例如：给明天调试的测试板烧录固件',
    note:'申请将提交给 IT 安全部门审批。在获批前该设备保持禁用。申请和该设备均会被记录。',
    cancel:'取消', submit:'提交申请',
  };
  const row=(label,val,mono)=>`<div style="display:flex;gap:12px;padding:9px 0;border-bottom:1px solid ${C.border};font-size:13px;">
    <span style="color:${C.mut2};width:110px;flex-shrink:0;">${label}</span>
    <span style="color:${C.ink};${mono?"font-family:'Noto Sans Mono',monospace;font-size:12px;":''}overflow:hidden;text-overflow:ellipsis;">${val}</span></div>`;
  return `<!doctype html><html lang="${en?'en':'zh-CN'}"><head><meta charset="utf-8"><style>
    *{box-sizing:border-box;} body{margin:0;}
    .desk{width:1000px;height:640px;background:${C.desk};display:flex;align-items:center;justify-content:center;font-family:${en?'':"'Noto Sans CJK SC',"}'Helvetica Neue',Arial,sans-serif;color:${C.ink};}
    .dlg{width:460px;background:${C.card};border:1px solid ${C.border};border-radius:14px;box-shadow:0 18px 50px rgba(0,0,0,.18);overflow:hidden;}
    .hd{display:flex;align-items:center;gap:10px;padding:15px 18px;border-bottom:1px solid ${C.border};}
    .bd{padding:16px 18px;}
    label.fl{display:block;font-size:12px;color:${C.mut2};margin:14px 0 6px;}
    textarea{width:100%;height:76px;border:1px solid ${C.border2};border-radius:8px;padding:9px 11px;font:inherit;font-size:13px;background:${C.field};color:${C.ink};resize:none;}
    .sel{display:inline-flex;align-items:center;gap:8px;border:1px solid ${C.border2};border-radius:8px;padding:7px 12px;font-size:13px;color:${C.ink};background:${C.field};}
    .ft{display:flex;gap:10px;justify-content:flex-end;padding:14px 18px;border-top:1px solid ${C.border};}
    .btn{font-size:13px;padding:8px 16px;border-radius:8px;border:1px solid ${C.border2};color:${C.mut};background:#fff;}
    .btnp{background:${C.blue};color:#fff;border:none;}
    .note{font-size:11px;color:${C.mut};line-height:1.55;margin-top:14px;background:#F6F4EC;border-radius:8px;padding:10px 12px;}
  </style></head><body><div class="desk"><div class="dlg">
    <div class="hd">${svg(usbIcon,20,C.amTx)}<span style="font-size:15px;font-weight:500;">${T.title}</span>
      <span style="margin-left:auto;color:${C.mut2};">${svg('<path d="M6 6l12 12M18 6L6 18"/>',18,C.mut2)}</span></div>
    <div class="bd">
      ${row(T.devLabel,'SanDisk Ultra USB 3.0')}
      ${row('','VID_0781&PID_5583\\4C530001230904',true)}
      ${row(T.wsLabel,'WS-ENG-042')}
      ${row(T.userLabel,'l.wang')}
      <label class="fl">${T.durLabel}</label>
      <span class="sel">${T.dur} ${svg('<path d="M6 9l6 6 6-6"/>',14,C.mut)}</span>
      <label class="fl">${T.just} <span style="color:${C.amTx}">*</span></label>
      <textarea placeholder="${T.ph}"></textarea>
      <div class="note">${svg('<circle cx="12" cy="12" r="9"/><path d="M12 8h.01M11 12h1v4h1"/>',15,C.mut)} ${T.note}</div>
    </div>
    <div class="ft"><span class="btn">${T.cancel}</span><span class="btn btnp">${T.submit}</span></div>
  </div></div></body></html>`;
}

const browser = await chromium.launch();
const page = await browser.newPage({ deviceScaleFactor:2, viewport:{width:1000,height:640} });
for (const lang of ['en','zh']){
  const html = dialog(lang);
  const hp = path.join(dirs[lang],'6_request_submit.html');
  fs.writeFileSync(hp, html);
  await page.goto('file://'+hp);
  await page.waitForTimeout(150);
  const el = await page.$('.desk');
  await el.screenshot({ path: path.join(dirs[lang],'6_request_submit.png') });
  console.log('wrote', lang, '6_request_submit');
}
await browser.close();
console.log('DONE');
