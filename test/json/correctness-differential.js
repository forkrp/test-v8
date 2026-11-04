// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license in LICENSE.
// Run through tools/json_correctness/run.py against an unmodified reference d8.
'use strict';

const api = arguments[0] || 'parse';
const seed = Number(arguments[1] || 0x4a534f4e) >>> 0;
const count = Number(arguments[2] || 4000);
const group = arguments[3] || 'all';
if (typeof JSON[api] !== 'function') throw Error('Missing API: ' + api);
const parsing = api === 'parse' || api === 'parseAsync';
let state = seed;
function random() {
  state ^= state << 13; state ^= state >>> 17; state ^= state << 5;
  return state >>> 0;
}
const bits = new DataView(new ArrayBuffer(8));
const hexUnits = new Array(65536);
function hex(text) {
  const chunks=[];
  for(let start=0;start<text.length;start+=1024){
    const units=[];
    for(let i=start;i<Math.min(start+1024,text.length);++i){
      const c=text.charCodeAt(i);
      units.push(hexUnits[c] || (hexUnits[c]=c.toString(16).padStart(4,'0')));
    }
    chunks.push(units.join(''));
  }
  return chunks.join('');
}
// Independent of JSON.stringify: preserve -0, every numeric bit, UTF-16 code
// units, property order, holes, prototypes and property attributes.
function wire(value, ancestors = new Set()) {
  if (value === null) return 'null';
  switch (typeof value) {
    case 'undefined': return 'undefined';
    case 'boolean': return value ? 'true' : 'false';
    case 'string': return 's' + value.length + ':' + hex(value);
    case 'number':
      bits.setFloat64(0, value, false);
      return 'n' + bits.getBigUint64(0, false).toString(16).padStart(16, '0');
    case 'bigint': return 'i' + value.toString();
    case 'symbol': return 'y' + hex(value.description || '');
    case 'function': return 'function';
  }
  if (ancestors.has(value)) return 'cycle';
  ancestors.add(value);
  const proto = Object.getPrototypeOf(value);
  const array = Array.isArray(value);
  const expectedPrototype = array ? Array.prototype : Object.prototype;
  let result = (array ? 'A' : 'O') +
      (proto === expectedPrototype ? '' : proto === null ? '0' : '!') + '{';
  for (const key of Reflect.ownKeys(value)) {
    const d = Object.getOwnPropertyDescriptor(value, key);
    result += wire(key) + '=' + (d.enumerable ? 'e' : '-') +
        (d.configurable ? 'c' : '-') + (d.writable ? 'w' : '-');
    result += 'value' in d ? wire(d.value, ancestors) : 'accessor';
    result += ';';
  }
  ancestors.delete(value);
  return result + '}';
}
// A separate, deliberately simple encoder generates parser input. It does not
// call either optimized JSON API, and encodes all non-ASCII UTF-16 units.
function quote(text, standardEscapes = false) {
  const chunks=[],units=[];
  for (let i = 0; i < text.length; ++i) {
    const c = text.charCodeAt(i);
    if(standardEscapes && (c===34 || c===92)){
      units.push('\\'+text[i]);
    }else{
      units.push(c < 32 || c > 126 || c === 34 || c === 92 ?
          '\\u' + c.toString(16).padStart(4, '0') : text[i]);
    }
    if(units.length===1024){chunks.push(units.join(''));units.length=0;}
  }
  if(units.length)chunks.push(units.join(''));
  return '"'+chunks.join('')+'"';
}
function source(value, cache, standardEscapes = false) {
  if (value === null) return 'null';
  if (typeof value === 'string') {
    if(cache && cache.has(value))return cache.get(value);
    const result=quote(value,standardEscapes);
    if(cache)cache.set(value,result);
    return result;
  }
  if (typeof value !== 'object') return Object.is(value, -0) ? '-0' : String(value);
  if (Array.isArray(value)) return '[' + value.map(v=>source(v,cache,standardEscapes)).join(',') + ']';
  return '{' + Object.keys(value).map(k => quote(k,standardEscapes) + ':' + source(value[k],cache,standardEscapes)).join(',') + '}';
}
let rows = 0;
async function record(id, body) {
  let result;
  try { result = 'ok:' + wire(await body()); }
  catch (error) {
    result = 'throw:' + wire(error instanceof Error ?
        [error.name, error.message] : error);
  }
  print(id + '\t' + result);
  if ((++rows & 255) === 0 && typeof gc === 'function') gc();
}
function call(value, replacer, gap) { return JSON[api](value, replacer, gap); }
const strings = ['', 'a', 'é', '漢', '😀', '\ud800', '\udfff', '\0',
                 '\n\r\t\b\f', '"\\/', '__proto__', 'constructor', 'Aa', 'BB'];
const lengths = [0,1,2,3,7,8,9,15,16,17,31,32,33,63,64,65,95,96,97,
                 127,128,129,255,256,257,511,512,513,4095,4096,4097];
function randomValue(depth) {
  switch (random() % (depth ? 8 : 5)) {
    case 0: return null;
    case 1: return !!(random() & 1);
    case 2: return (random() & 1) ? -0 : (random() | 0) / 8;
    case 3: return strings[random() % strings.length];
    case 4: {
      let text = '';
      for (let i = random() % 24; i > 0; --i)
        text += String.fromCharCode(random() & 0xffff);
      return text;
    }
    case 5: return Array.from({length:random() % 9}, () => randomValue(depth-1));
    default: {
      const value = {};
      for (let i = random() % 10; i > 0; --i) {
        const key = random() & 1 ? strings[random() % strings.length] : String(random()%30);
        Object.defineProperty(value, key, {value:randomValue(depth-1),
            enumerable:true, writable:true, configurable:true});
      }
      return value;
    }
  }
}

async function boundaries() {
  // Every UTF-16 code unit, with adjacent units crossing scanner lanes.
  for (let base = 0; base < 65536; base += 128) {
    const text = Array.from({length:128}, (_,i) => String.fromCharCode(base+i)).join('');
    await record('bmp:'+base, () => call(parsing ? quote(text) : text));
  }
  for (const length of [31,32,33,47,48,49,63,64,65,127,128,129,255]) {
    for (const escape of ['\0','\x1f','"','\\','\n','\ud800','\udfff','😀']) {
      const values=[];
      for(let at=0;at<Math.min(length,33);++at)
        values.push('a'.repeat(at)+escape+'b'.repeat(length-at));
      await record('lanes:'+length+':'+hex(escape),()=>call(parsing?source(values):values));
    }
  }
  for (const n of lengths) {
    for (let i = 0; i < strings.length; ++i) {
      const value = 'a'.repeat(n) + strings[i] + '漢'.repeat(n % 3);
      await record('string:'+n+':'+i, () => call(parsing ? quote(value) : value));
      if (parsing) {
        // Literal UTF-16 source, including narrowing after a wide suffix.
        const literal = '["'+'a'.repeat(n)+'\\n",'+quote(strings[i])+',"漢"]';
        await record('wide:'+n+':'+i, () => call(literal));
      }
    }
  }
  for (const width of [0,1,2,31,32,33,62,63,64,65,66,126,127,128,129,255,256]) {
    const object = {};
    for (let i = 0; i < width; ++i) object['k'+i] = i & 1 ? i/7 : i;
    for (const key of ['__proto__','', 'Aa','BB','0','01','4294967294','4294967295'])
      Object.defineProperty(object,key,{value:key,enumerable:true,writable:true,configurable:true});
    const value = [object,{...object},{...object,k0:'changed'}];
    await record('width:'+width, () => call(parsing ? source(value) : value));
    if (parsing) {
      const text = source(value).slice(0,-1)+',{"a":1,"a":2,"\\u0061":3,"__proto__":4}]';
      await record('duplicate:'+width, () => call(text));
    }
  }
  for (const depth of [1,15,16,31,32,63,64,127,128,255,256,512]) {
    let value = -0;
    for (let i = 0; i < depth; ++i) value = i & 1 ? [value] : {next:value};
    await record('depth:'+depth, () => call(parsing ? source(value) : value));
  }
  const numbers = ['-0','-0.0','-0e9999','1e400','-1e-400','5e-324',
    '2.2250738585072012e-308','1.7976931348623158e308',
    '1073741823','1073741824','2147483647','2147483648','4294967295',
    '9007199254740991','9007199254740992','9007199254740993',
    '1e-6','1e-7','1e20','1e21','18446744073709551615'];
  for (const n of [15,16,17,18,19,20,22,23,127,128,129,767,768,1024,4096]) {
    numbers.push('9'.repeat(n), '0.'+'0'.repeat(n)+'1',
                 '1.'+'0'.repeat(n)+'1', '9'.repeat(n)+'e-'+n);
  }
  for (let i = 0; i < numbers.length; ++i) {
    const text = numbers[i];
    await record('number:'+i, () => call(parsing ? text : Number(text)));
    await record('predict:'+i, () => call(parsing ? '[1.25,2.5,'+text+',0,'+text+']' :
        [1.25,2.5,Number(text),0,Number(text)]));
  }
  if (parsing) {
    const bad = ['', ' ', '+1','00','-01','-', '1.','1e','1e+','1e-',
      'NaN','Infinity','undefined','[1,]','{"a":1,}','[','{','"',
      '"\\"','"\\u"','"\\u000"','"\\u00g0"','true false','{}x'];
    for (let c = 0; c < 256; ++c) {
      bad.push('"\\'+String.fromCharCode(c)+'"');
      if (c < 32) bad.push('"a'+String.fromCharCode(c)+'b"');
    }
    for (let i = 0; i < bad.length; ++i) {
      for (const prefix of ['', ' \r\n\t', '["漢",']) {
        const text = prefix + bad[i] + (prefix[0] === '[' ? ']' : '');
        await record('grammar:'+i+':'+hex(prefix), () => call(text));
      }
    }
  }
}

async function randomized() {
  for (let i = 0; i < count; ++i) {
    const value = randomValue(4);
    const text = source(value);
    await record('fuzz:'+i, () => call(parsing ? text : value));
    if (parsing && i % 3 === 0) {
      const at = random() % (text.length + 1);
      const mutation = [text.slice(0,at), text.slice(0,at)+'\\'+text.slice(at),
        text.slice(0,at)+'\0'+text.slice(at), text+' x'][random()%4];
      await record('mutate:'+i, () => call(mutation));
    }
  }
  // Arbitrary IEEE-754 bit patterns stress the independent formatter backend.
  if (!parsing) {
    for (let i = 0; i < count; ++i) {
      bits.setUint32(0, random(), false); bits.setUint32(4, random(), false);
      const value = bits.getFloat64(0, false);
      await record('bits:'+i, () => call([value,value,-value]));
    }
  }
}

async function effects() {
  if (parsing) {
    for (const mode of ['trace','delete','replace','mutate','throw','reenter']) {
      await record('reviver:'+mode, async () => {
        const trace = [], marker = {sentinel:'reviver'};
        const input = '{"0":-0,"a":[1,2,3],"a":[4,5],"__proto__":{"x":1},"b":"\\u0061"}';
        let result, failure;
        try {
          result = await call(input, function(key,value,context) {
            trace.push([key, typeof value, context && context.source,
                        Object.keys(this), Object.is(value,-0)]);
            if (typeof gc === 'function') gc();
            if (mode === 'delete' && key === '0') return undefined;
            if (mode === 'replace' && key === 'b') return {replacement:true};
            if (mode === 'mutate' && key === '0') this.injected = 7;
            if (mode === 'throw' && key === 'b') throw marker;
            if (mode === 'reenter' && key === 'b') return JSON.parse('[1,-0,"x"]');
            return value;
          });
        } catch (error) { failure = error === marker ? 'same-marker' : String(error); }
        return [result,failure,trace];
      });
    }
    for (const throws of [false,true]) {
      await record('coerce:'+throws, async () => {
        const trace=[], marker={sentinel:'coerce'};
        const input={toString(){trace.push('toString');if(throws)throw marker;return '[-0,"漢"]';}};
        try {return [await call(input),trace];}
        catch(e){return [e===marker,trace];}
      });
    }
    return;
  }
  for (const mode of ['getter','toJSON','replacer','proxy','throw','reenter','prototype','cycle']) {
    await record('effects:'+mode, async () => {
      const trace=[], marker={sentinel:'callback'};
      const value={a:1,b:2,c:[3,,undefined,NaN,-0]};
      let replacer, cleanup=()=>{};
      if (mode==='getter' || mode==='throw' || mode==='reenter') {
        Object.defineProperty(value,'b',{enumerable:true,get(){
          trace.push('get:b');delete value.a;value.d=4;
          if(typeof gc==='function')gc();
          if(mode==='throw')throw marker;
          return mode==='reenter' ? JSON.stringify({inner:7}) : '漢';
        }});
      }
      if(mode==='toJSON')value.b={toJSON(key){trace.push(['toJSON',key]);return -0;}};
      if(mode==='replacer')replacer=function(k,v){trace.push(['replace',k,typeof v]);if(typeof gc==='function')gc();return k==='a'?undefined:v;};
      if(mode==='proxy')value.b=new Proxy({x:1,y:2},{
        ownKeys(t){trace.push('ownKeys');return Reflect.ownKeys(t).reverse();},
        getOwnPropertyDescriptor(t,k){trace.push(['descriptor',k]);return Reflect.getOwnPropertyDescriptor(t,k);},
        get(t,k,r){trace.push(['get',String(k)]);return Reflect.get(t,k,r);}});
      if(mode==='prototype'){
        Object.defineProperty(Array.prototype,'1',{configurable:true,get(){return 99;},set(v){Object.defineProperty(this,'1',{value:v,writable:true,enumerable:true,configurable:true});}});
        cleanup=()=>delete Array.prototype[1];
      }
      if(mode==='cycle')value.b=value;
      try {return [await call(value,replacer),trace];}
      catch(e){return [e===marker?'same-marker':[e.name,e.message],trace];}
      finally{cleanup();}
    });
  }
  for (const length of [0,1,2,31,32,33,63,64,65,127,128,129,1024,4000]) {
    for (const kind of ['getter','toJSON','bigint','cycle']) {
      await record('late-fallback:'+length+':'+kind,async()=>{
        const trace=[];
        const values=Array.from({length},(_,i)=>({a:i,b:i/8,c:'plain'}));
        const last={a:1,b:2,c:3};
        if(kind==='getter')Object.defineProperty(last,'b',{enumerable:true,get(){
          trace.push('get');if(values.length)values[0].a=999;
          delete last.c;gc();return 'escaped\n漢';}});
        if(kind==='toJSON')last.toJSON=function(k){trace.push(['toJSON',k]);gc();return {x:7};};
        if(kind==='bigint')last.b=1n;
        if(kind==='cycle')last.b=last;
        values.push(last);
        try{return [await call(values),trace];}
        catch(e){return [e.name,e.message,trace];}
      });
    }
  }
  await record('array-length-snapshot',async()=>{
    const trace=[],value=[1,2,3,4];
    Object.defineProperty(value,'0',{get(){trace.push('get:0');value.length=2;return 9;}});
    return [await call(value),trace];
  });
  await record('replacer-gap-coercion-order',async()=>{
    const trace=[],key=new String('a'),gap=new Number(2);
    key.toString=function(){trace.push('key');return 'a';};
    gap.valueOf=function(){trace.push('gap');return 2;};
    const value={get a(){trace.push('get:a');return 1;}};
    return [await call(value,[key,key],gap),trace];
  });
  for (const gap of [undefined,0,1,10,11,Infinity,NaN,-1,'ab\0漢😀longspace',new Number(3),new String('  ')]) {
    await record('gap:'+wire(gap), () => call({x:[1,2],y:'漢'},undefined,gap));
  }
  for (const replacer of [[],['b','a','b',1,new String('c'),new Number(0)],
                          ['__proto__','constructor','4294967295']]) {
    await record('property-list:'+wire(replacer), () => call({a:1,b:2,c:3,0:4,1:5},replacer,2));
  }
  for (const value of [undefined,Symbol('x'),()=>1,NaN,Infinity,-Infinity,-0,
                       1n,Object(1n),new Number(NaN),new String('漢'),new Boolean(false),
                       Object(Symbol('x')),new Date(0),new Date(NaN)]) {
    await record('special:'+Object.prototype.toString.call(value)+':'+wire(value),()=>call(value));
  }
  await record('capture-before-mutation',async()=>{
    const value={a:'before',b:[1,2]}, result=call(value);
    value.a='after';value.b[0]=99;gc();
    return await result;
  });
  await record('bigint-toJSON',async()=>{
    BigInt.prototype.toJSON=function(){return this.toString();};
    try{return await call({x:1n,y:[2n]});}
    finally{delete BigInt.prototype.toJSON;}
  });
  if (typeof JSON.rawJSON === 'function') {
    for(const raw of ['-0','9007199254740993','1e400','"\\ud800"','true','null'])
      await record('raw:'+raw,()=>call({raw:JSON.rawJSON(raw)}));
  }
}

async function largeCases() {
  // Cross both the four-way and eight-way parser split thresholds, preserving
  // strings that contain structural punctuation and odd/even backslash runs.
  for(const target of [4*1024*1024-32,4*1024*1024+32,16*1024*1024+32]){
    const unit='[]{}\\"\\\\tail,';
    const encode=v=>source(v,new Map(),true);
    const value=Array.from({length:8},(_,i)=>({id:i,pad:'',tail:[-0,1.25,true,null]}));
    const encodedUnitLength=encode(unit).length-2;
    const repetitions=Math.floor((target-encode(value).length)/8/encodedUnitLength);
    for(const item of value)item.pad=unit.repeat(repetitions);
    value[7].pad+='a'.repeat(target-encode(value).length);
    if(!parsing){
      await record('large:'+target,()=>call(value));
      continue;
    }
    const text=encode(value);
    if(text.length!==target)throw Error('Incorrect partition boundary fixture');
    // Record the exact byte-independent source length as part of the case id.
    await record('large:'+target+':'+text.length,()=>call(text));
    await record('large-space:'+target,()=>call(' \t\r\n'.repeat(33)+text+'\r\n '));
    if(target<8*1024*1024){
      const wide=text.replace('tail,','漢tail,');
      await record('large-wide:'+target,()=>call(wide));
    }
    for(const numerator of [1,2,3]){
      for(const offset of [-1,0,1,15,16]){
        const at=Math.floor(text.length*numerator/4)+offset;
        await record('large-bad:'+target+':'+numerator+':'+offset,
          ()=>call(text.slice(0,at)+'\0'+text.slice(at)));
      }
    }
  }
}

async function main() {
  if (group==='all' || group==='boundaries') await boundaries();
  if (group==='all' || group==='fuzz') await randomized();
  if (group==='all' || group==='effects') await effects();
  if (group==='large') await largeCases();
  print('DONE\t'+rows+'\t'+seed);
}
main().catch(error=>{print(error.stack||error);quit(1);});
