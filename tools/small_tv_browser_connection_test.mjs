import fs from 'node:fs/promises';
import path from 'node:path';
import {createRequire} from 'node:module';
import readline from 'node:readline';
import assert from 'node:assert/strict';
const args=Object.fromEntries(process.argv.slice(2).reduce((a,v,i,all)=>i%2?a:[...a,[v.slice(2),all[i+1]]],[]));
const url=new URL(args.url);assert.equal(url.hostname,'127.0.0.1');
const {chromium}=createRequire(import.meta.url)(path.join(args.modules,'playwright'));
const input=readline.createInterface({input:process.stdin});
const messages=input[Symbol.asyncIterator]();
const sync=async data=>{process.stdout.write(JSON.stringify(data)+'\n');assert.equal((await messages.next()).value,'continue');};
const browser=await chromium.launch({executablePath:args.browser,headless:true});
const rows=[];
const expected=JSON.parse(await fs.readFile(path.join(args.output,'expected.json'),'utf8'));
const version=browser.version();
try {
 for(let round=0;round<Number(args.rounds);round++) {
  for(const mode of ['keep','none','close']) {
   const context=await browser.newContext({viewport:{width:390,height:844},serviceWorkers:'block'});
   await context.route('**/*',route=>new URL(route.request().url()).origin===url.origin?route.continue():route.abort());
   const row={round,mode,requests:[],errors:[],page_errors:[],passed:false};rows.push(row);
   const page=await context.newPage();page.setDefaultTimeout(30000);
   page.on('console',m=>{if(['error','warning'].includes(m.type()))row.errors.push(m.text());});
   page.on('pageerror',e=>row.page_errors.push(String(e)));
   page.on('requestfinished',async req=>{const response=await req.response();row.requests.push({path:new URL(req.url()).pathname,status:response?.status(),finished:true});});
   page.on('requestfailed',req=>row.requests.push({path:new URL(req.url()).pathname,error:req.failure()?.errorText,finished:false}));
   try {
    if(mode!=='none') {
     const response=await context.request.get(url.href,{headers:mode==='close'?{'Connection':'close'}:{},timeout:15000});
     assert.equal(response.status(),200);await response.body();
    }
    await sync({phase:'before-page',round,mode});
    await page.goto(url.href,{waitUntil:'domcontentloaded'});
    await page.waitForLoadState('networkidle');
    row.controls=await page.evaluate(()=>({date:document.querySelector('#timelist')?.value,
      gif:document.querySelector('#giflist')?.value,theme:document.querySelector('#themeselect')?.value,
      brightness:document.querySelector('#ledBrightness')?.value,
      colors:[0,1,2].map(i=>document.querySelector('#colorPicker'+i)?.value),
      flags:Object.fromEntries([['tempUnit','temp-unit-switch'],['hourFormat','hour-format-switch'],['mileDisplay','mile-display-switch'],['timeSync','time-sync-switch']].map(([key,id])=>[key,Number(document.getElementById(id).checked)])),
      flagsReady:['temp-unit-switch','hour-format-switch','mile-display-switch'].every(id=>document.getElementById(id)?.disabled===false)}));
    const c=row.controls;
    row.controls_match=c.flagsReady && c.date===String(expected['/GetTimeModeValue']) &&
      c.gif===String(expected['/getgifvalue']) && c.theme===String(expected['/getclocktype']) &&
      Number(c.brightness)===expected['/get_led_brightness'].brightness &&
      c.colors.every((color,i)=>color.toLowerCase()===expected['/get_clock_colors']['color'+i].toLowerCase()) &&
      Object.keys(c.flags).every(key=>c.flags[key]===expected['/getsettings'][key]);
    row.passed=row.controls_match && row.errors.length===0 && row.page_errors.length===0 &&
      Object.keys(expected).every(api=>row.requests.some(r=>r.path===api && r.status===200 && r.finished)) &&
      !row.requests.some(r=>!r.finished || r.status!==200);
   }catch(error){row.error=String(error);}
   await sync({phase:'after-page',...row});
   await context.close();
  }
 }
}finally {
 await browser.close();input.close();
 await fs.writeFile(path.join(args.output,'browser.json'),JSON.stringify({finished:true,version,rows},null,2)+'\n');
}
