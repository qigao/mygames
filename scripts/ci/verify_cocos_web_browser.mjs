#!/usr/bin/env node
// REAL Microsoft Edge Web Desktop loading smoke for a REAL Creator build.
// This does not mock Cocos rendering, does not simulate a Game Owner, does not
// certify gameplay, and must never upload a screenshot, hand or bearer token.
// Run only after verify_cocos_creator_web.py has qualified the physical build.
import {createHash} from 'node:crypto';
import {createReadStream} from 'node:fs';
import {readFile, realpath, stat, writeFile} from 'node:fs/promises';
import {createServer} from 'node:http';
import {extname, join, resolve, sep} from 'node:path';
import {parseArgs} from 'node:util';
import {chromium} from 'playwright-core';

const PREFIX = 'mygames-m5i-web-desktop-';
const CONTENT_TYPE = Object.freeze({
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.mjs': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json',
  '.wasm': 'application/wasm',
  '.bin': 'application/octet-stream',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.webp': 'image/webp',
  '.ico': 'image/x-icon',
  '.svg': 'image/svg+xml',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.ttf': 'font/ttf',
  '.mp3': 'audio/mpeg',
  '.ogg': 'audio/ogg'
});

async function sha256(path) {
  const digest = createHash('sha256');
  for await (const chunk of createReadStream(path)) digest.update(chunk);
  return digest.digest('hex');
}
function assert(condition, message) {
  if (!condition) throw Error(message);
}
async function openTrustedBuild(project, manifestPath) {
  const manifest = JSON.parse(await readFile(manifestPath, 'utf8'));
  assert(manifest.gate === 'real-cocos-creator-cli-web-desktop',
    'Expected actual Creator build evidence');
  assert(manifest.creator_sha256_pin_verified === true &&
    manifest.expected_head_sha_matched === true,
    'Browser probe requires a trusted pinned Creator binary and exact Git HEAD');
  assert(manifest.documented_success_exit_code === 36 &&
    manifest.output_isolated_per_run === true,
    'Creator build was not actually accepted');
  assert(typeof manifest.source_head_sha === 'string' &&
    /^[0-9a-f]{40}$/.test(manifest.source_head_sha),
    'Missing exact source HEAD');
  assert(typeof manifest.output_name === 'string' &&
    new RegExp('^' + PREFIX + '[0-9a-f]{16}$').test(manifest.output_name),
    'Unsafe or nonisolated build output directory');
  const root = await realpath(join(project, 'build', manifest.output_name));
  const index = join(root, 'index.html');
  const application = join(root, 'src', 'application.js');
  const [a,b] = await Promise.all([sha256(index),sha256(application)]);
  assert(a === manifest.index_sha256 && b === manifest.bootstrap_sha256,
    'Creator build artifact changed after qualification');
  return {root,manifest};
}
function serveBuild(root) {
  const server = createServer(async (request,response) => {
    try {
      if (request.method !== 'GET' && request.method !== 'HEAD') {
        response.writeHead(405); response.end(); return;
      }
      const uri = new URL(request.url ?? '/', 'http://127.0.0.1');
      if (uri.pathname === '/favicon.ico') {
        response.writeHead(204); response.end(); return;
      }
      const decoded = decodeURIComponent(uri.pathname);
      assert(!decoded.includes('\0') && !decoded.includes('\\'),
        'Invalid resource path');
      let target = resolve(root, '.' + decoded);
      // '/' resolves to the root directory itself and must serve its
      // index.html; other paths must remain strictly inside this root.
      assert(target === root || target.startsWith(root + sep),
        'Resource escapes built output');
      let info = await stat(target);
      if (info.isDirectory()) {
        target = join(target,'index.html');
        info = await stat(target);
      }
      const physical = await realpath(target);
      assert(physical.startsWith(root + sep) && info.isFile(),
        'Resource is not a regular file within build');
      response.writeHead(200, {
        'Content-Type': CONTENT_TYPE[extname(target).toLowerCase()] ??
          'application/octet-stream',
        'Content-Length': info.size,
        'Cache-Control': 'no-store',
        'X-Content-Type-Options': 'nosniff'
      });
      if (request.method === 'HEAD') {response.end();return;}
      const stream = createReadStream(target);
      stream.on('error',()=>response.destroy());
      stream.pipe(response);
    } catch {
      response.writeHead(404);
      response.end();
    }
  });
  return server;
}
async function listen(server) {
  await new Promise((ok,fail)=>{
    server.once('error',fail);
    server.listen(0,'127.0.0.1',ok);
  });
  return 'http://127.0.0.1:' + server.address().port;
}

async function run(project, buildReport, report, timeoutMs) {
  const {root,manifest} = await openTrustedBuild(project,buildReport);
  const server = serveBuild(root);
  let browser;
  const contexts=[];
  try {
    const origin = await listen(server);
    // This is a real installed Microsoft Edge GUI, not a synthetic
    // Cocos engine or bundled headless Chromium downloaded by Playwright.
    browser = await chromium.launch({
      channel:'msedge',headless:false,timeout:timeoutMs
    });
    const observations=[];
    for(let i=0;i<3;i++) {
      const context=await browser.newContext({
        viewport:{width:1280,height:720},
        serviceWorkers:'block'
      });
      contexts.push(context);
      let exceptions=0,resourceFailures=0,fatalConsole=0;
      let networkFailures=0,unexpectedOutbound=0,blockedWebSockets=0;
      // The unpacked Creator bundle must be self-contained. Intercept HTTP
      // before the very first navigation; abort and count all requests to
      // other origins, rather than leaking traffic from the GUI test runner.
      await context.route('**/*', async route=>{
        if(route.request().url().startsWith(origin+'/')) {
          await route.continue();
        } else {
          unexpectedOutbound++;
          await route.abort('blockedbyclient');
        }
      });
      // Intercept *every* WebSocket, not only the original local /ws URL.
      // Never impersonate the game server. The known loopback connection is
      // closed deliberately; an unexpected endpoint fails the qualification.
      await context.routeWebSocket(/.*/, async socket=>{
        blockedWebSockets++;
        if(socket.url() !== 'ws://127.0.0.1:1780/ws')
          unexpectedOutbound++;
        await socket.close();
      });
      const page=await context.newPage();
      page.on('pageerror',()=>{exceptions++;});
      page.on('response',response=>{
        if(response.url().startsWith(origin + '/') && response.status()>=400)
          resourceFailures++;
      });
      page.on('requestfailed',request=>{
        // HTTP 404 is counted above; DNS failures, aborted asset loads,
        // broken connections and other missing actual build resources do
        // not produce an HTTP response and must be rejected separately.
        if(request.url().startsWith(origin+'/')) networkFailures++;
      });
      page.on('console',message=>{
        // Expected loopback WebSocket failures do not prove a scene failure.
        // Never print raw console messages: they may include private data.
        if(message.type()==='error' &&
           /MissingScript|Cannot find module|Uncaught|SyntaxError|ReferenceError|WebGL.*not supported/i
             .test(message.text())) fatalConsole++;
      });
      // The local backend is deliberately not part of this smoke; all
      // WebSockets were already closed at the context boundary above.
      await page.goto(origin+'/',{waitUntil:'domcontentloaded',timeout:timeoutMs});
      await page.waitForFunction(()=>{
        return [...document.querySelectorAll('canvas')].some(canvas=>{
          const r=canvas.getBoundingClientRect();
          return canvas.width>=640 && canvas.height>=360 &&
                 r.width>=300 && r.height>=200;
        });
      },null,{timeout:timeoutMs});
      await page.waitForTimeout(3000);
      const surface=await page.evaluate(()=>{
        const canvas=[...document.querySelectorAll('canvas')].find(c=>c.width>=640);
        return canvas ? {
          buffer_width:canvas.width,buffer_height:canvas.height,
          displayed_width:Math.round(canvas.getBoundingClientRect().width),
          displayed_height:Math.round(canvas.getBoundingClientRect().height)
        } : null;
      });
      assert(surface !== null,'Actual Cocos Canvas not initialized');
      const image=await page.screenshot({animations:'disabled',timeout:timeoutMs});
      assert(image.length>256 && image.subarray(0,8).equals(
        Buffer.from([137,80,78,71,13,10,26,10])),
        'Real browser screenshot is not a valid PNG');
      assert(exceptions===0 && resourceFailures===0 && fatalConsole===0 &&
        networkFailures===0 && unexpectedOutbound===0,
        'Real Web Desktop scene failed, had missing resources, or attempted unexpected external traffic');
      observations.push({
        context:i+1,canvas:surface,
        screenshot_sha256:createHash('sha256').update(image).digest('hex'),
        uncaught_script_errors:exceptions,
        failed_static_resources:resourceFailures,
        failed_static_network_requests:networkFailures,
        unexpected_external_requests:unexpectedOutbound,
        deliberately_closed_websockets:blockedWebSockets,
        fatal_console_errors:fatalConsole
      });
      // We deliberately retain contexts simultaneously: three independent
      // browser profiles must have rendered the actual packaged app.
    }
    const evidence={
      gate:'real-cocos-edge-web-desktop-three-context-smoke',
      source_head_sha:manifest.source_head_sha,
      creator_build_manifest_verified:true,
      creator_executable_pin_verified:true,
      browser:'Microsoft Edge',
      browser_version:browser.version(),
      real_gui_browser:true,
      actual_isolated_browser_contexts:observations.length,
      contexts:observations,
      actual_gameplay_backend_connected:false,
      actual_three_player_gameplay_acceptance:false,
      editor_interactive_scene_import_acceptance:false,
      description:'Real Edge loaded the bundled Creator Web Desktop in three isolated contexts with all unrelated outbound traffic blocked. No synthetic Cocos renderer or fake gameplay server. Full three-client gameplay and Editor import remain separate.'
    };
    await writeFile(report,JSON.stringify(evidence,null,2)+'\n','utf8');
    console.log('Real Edge Web Desktop smoke: PASS (3 isolated browser contexts, Canvas present, static assets intact, no unexpected outbound network); full gameplay UNVERIFIED');
  } finally {
    await Promise.allSettled(contexts.map(x=>x.close()));
    if(browser) await browser.close();
    await new Promise(ok=>server.close(()=>ok()));
  }
}
try {
  const {values}=parseArgs({
    options:{
      project:{type:'string'},
      'build-report':{type:'string'},
      report:{type:'string'},
      timeout:{type:'string',default:'90000'}
    }
  });
  if(!values.project || !values['build-report'] || !values.report)
    throw Error('Usage: node verify_cocos_web_browser.mjs --project PATH --build-report JSON --report JSON');
  const ms=Number(values.timeout);
  assert(Number.isSafeInteger(ms) && ms>=30000 && ms<=180000,
    'Browser smoke timeout outside 30-180 seconds');
  await run(resolve(values.project),resolve(values['build-report']),
    resolve(values.report),ms);
} catch(error) {
  console.error('Actual Cocos Web Desktop browser smoke FAILED: ' +
    (error?.message ?? 'unknown error'));
  process.exitCode=1;
}
