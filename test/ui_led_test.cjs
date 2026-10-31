const {JSDOM}=require('jsdom');const fs=require('fs');
const html=fs.readFileSync(require('path').join(__dirname,'..','index.html'),'utf8');
let led={on:true,mode:0,color:3,level:2,modes:['Rainbow','Solid','Breathe','Status'],palette:['Red','Orange','Yellow','Green','Cyan','Blue','Purple','White'].map((n,i)=>({name:n,hex:'#'+(i*111111%1000000).toString().padStart(6,'0')}))};
const posts=[];const errors=[];
const dom=new JSDOM(html,{runScripts:'dangerously',pretendToBeVisual:true,url:'http://192.168.4.1/',beforeParse(w){
 w.fetch=async(u,o={})=>{if(u==='/api/led'&&o.method==='POST'){const b=JSON.parse(o.body);posts.push(b);Object.assign(led,b);}
  const body=u.startsWith('/api/led')?led:u.startsWith('/api/state')?{state:'LIVE',live:false}:{};
  return {ok:true,status:200,json:async()=>JSON.parse(JSON.stringify(body)),text:async()=>JSON.stringify(body)}};
 w.addEventListener('error',e=>errors.push(e.message));}});
const w=dom.window,$=id=>w.document.getElementById(id);
setTimeout(async()=>{
 const nav=[...w.document.querySelectorAll('nav button')].find(b=>b.dataset.page==='lighting');
 if(!nav) throw 'no nav'; nav.click();
 if($('lighting').classList.contains('hide')) throw 'section hidden';
 const sw=$('ledColors').children; if(sw.length!==8) throw 'swatches '+sw.length;
 if($('ledModes').children.length!==4) throw 'modes';
 if(!$('ledPreview').classList.contains('rainbow')) throw 'preview rainbow';
 sw[0].click(); await new Promise(r=>setTimeout(r,20));
 if(JSON.stringify(posts.at(-1))!==JSON.stringify({color:0,on:true,mode:1})) throw 'color post '+JSON.stringify(posts.at(-1));
 if(!$('ledColors').children[0].classList.contains('sel')) throw 'sel';
 $('ledPower').click(); await new Promise(r=>setTimeout(r,20)); if(posts.at(-1).on!==false) throw 'power';
 if($('ledPower').textContent!=='เปิดไฟ') throw 'power label';
 $('ledPlus').click(); await new Promise(r=>setTimeout(r,20)); if(posts.at(-1).level!==3||posts.at(-1).on!==true) throw 'plus';
 if([...$('ledBars').children].filter(e=>e.classList.contains('on')).length!==3) throw 'bars';
 $('ledModes').children[3].click(); await new Promise(r=>setTimeout(r,20)); if(posts.at(-1).mode!==3) throw 'mode';
 if(errors.length) throw 'page errors: '+errors.join(';');
 console.log('UI lighting test PASS', posts.length,'posts'); process.exit(0);
},300);
setTimeout(()=>{console.log('timeout');process.exit(1)},4000);
