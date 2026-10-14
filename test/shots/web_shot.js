// Renders the real web panel (web/index.html) with realistic API fixtures into PNGs.
// Usage: NODE_PATH=<node_modules> node test/shots/web_shot.js <outdir> <url>
const { chromium } = require('playwright');

const outdir = process.argv[2] || '.';
const url = process.argv[3] || 'http://localhost:8099/';

const info = {"parallel":4,"notify":50,"cookies":"example.com","ip":"192.168.1.8","conns":12,
  "cookie":true,"jb":true,"free":121830000000,"auto":1,"del":0,"replace":0,"ver":"1.00",
  "build":"20261014-181500","role":"daemon","net":true};

const G = 1000000;
const jobs = [
  {id:1,name:"Itemzflow_v1.8.9.pkg",state:"Downloading",size:22.5*G,done:12.4*G,speed:2.6*G,ipct:0,err:"",complete:false},
  {id:2,name:"Remote_Package_Installer_v1.02.pkg",state:"Downloading",size:4.2*G,done:0.32*G,speed:1.2*G,ipct:0,err:"",complete:false},
  {id:3,name:"Apollo_Save_Tool_v1.0.0.pkg",state:"Downloaded",size:18.6*G,done:18.6*G,speed:0,ipct:0,err:"",complete:true},
  {id:4,name:"FPKGi_v1.10.0-release.pkg",state:"Queued",size:85.5*G,done:0,speed:0,ipct:0,err:"",complete:false},
  {id:5,name:"PS4_Xplorer_v1.0.0.pkg",state:"Installing",size:12.3*G,done:12.3*G,speed:0,ipct:42,err:"",complete:true},
  {id:6,name:"RetroArch_v1.19.0.pkg",state:"Error",size:412*G,done:0,speed:0,ipct:0,
   err:"HTTP 401 - login needed? add a cookie for example.com in the web panel",complete:false},
];

const cat = [
  ["Apollo Save Tool","APOL00001",18600000,"USA","1.00"],
  ["FPKGi","PKGI00001",85458000,"USA","1.10"],
  ["GoldHEN Payload Loader","BREW00007",2100000,"EUR","1.00"],
  ["Itemzflow","ITEM00001",22500000,"USA","1.08"],
  ["Media Player","BREW00011",9400000,"EUR","1.02"],
  ["OpenOrbis Sample App","BREW00001",6100000,"USA","1.00"],
  ["PS4 Xplorer","PS4X00001",12300000,"USA","1.00"],
  ["Remote Package Installer","FLTZ00003",4200000,"USA","1.02"],
  ["RetroArch","RETR00001",412000000,"USA","1.19"],
  ["Save Manager","BREW00021",7300000,"EUR","1.01"],
  ["TurboDL","PKDL00001",6619136,"USA","1.00"],
  ["uLaunch Tester","BREW00031",1200000,"USA","1.00"],
];
const tsv = cat.map(c=>`${c[0]}\t${c[1]}\t${c[2]}\t${c[3]}\t${c[4]}\thttps://example.invalid/${c[1].toLowerCase()}.pkg`).join('\n')+'\n';

(async () => {
  const b = await chromium.launch();
  const ctx = await b.newContext({ viewport: { width: 1440, height: 940 }, deviceScaleFactor: 2 });
  const p = await ctx.newPage();
  await p.route('**/api/info', r => r.fulfill({ contentType: 'application/json', body: JSON.stringify(info) }));
  await p.route('**/api/jobs', r => r.fulfill({ contentType: 'application/json', body: JSON.stringify(jobs) }));
  await p.route('**/api/catalog.tsv*', r => r.fulfill({ contentType: 'text/plain', body: tsv }));
  await p.goto(url);
  await p.waitForTimeout(1200);
  await p.screenshot({ path: `${outdir}/web-desktop.png`, fullPage: false });
  // catalog section
  await p.evaluate(() => document.querySelectorAll('main .card')[2].scrollIntoView({ block: 'start' }));
  await p.waitForTimeout(400);
  await p.screenshot({ path: `${outdir}/web-catalog.png`, fullPage: false });
  // settings section
  await p.evaluate(() => document.querySelectorAll('main .card')[3].scrollIntoView({ block: 'start' }));
  await p.waitForTimeout(300);
  await p.screenshot({ path: `${outdir}/web-settings.png`, fullPage: false });

  // mobile
  const m = await b.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 3, isMobile: true, hasTouch: true });
  const mp = await m.newPage();
  await mp.route('**/api/info', r => r.fulfill({ contentType: 'application/json', body: JSON.stringify(info) }));
  await mp.route('**/api/jobs', r => r.fulfill({ contentType: 'application/json', body: JSON.stringify(jobs) }));
  await mp.route('**/api/catalog.tsv*', r => r.fulfill({ contentType: 'text/plain', body: tsv }));
  await mp.goto(url);
  await mp.waitForTimeout(1000);
  await mp.screenshot({ path: `${outdir}/web-mobile.png`, fullPage: true });
  await b.close();
  console.log('screenshots written to ' + outdir);
})();
