#!/usr/bin/env python3
"""Independent, dependency-free MessagePack/resource v1 decoder for evidence."""
import argparse
import json
import math
from pathlib import Path
import struct

class Extension:
    def __init__(self, kind, data): self.kind, self.data = kind, data

class Reader:
    def __init__(self, data): self.data, self.pos = data, 0
    def take(self, size):
        if size < 0 or size > len(self.data)-self.pos: raise ValueError('truncated')
        result = self.data[self.pos:self.pos+size]; self.pos += size; return result
    def uint(self, size): return int.from_bytes(self.take(size), 'big')
    def value(self, depth=0):
        if depth > 512: raise ValueError('nesting')
        tag = self.uint(1)
        if tag < 128: return tag
        if tag >= 224: return tag - 256
        if 160 <= tag <= 191: return self.take(tag & 31).decode('utf-8')
        if tag == 192: return None
        if tag in (194, 195): return tag == 195
        if tag in (202, 203): return struct.unpack('>f' if tag == 202 else '>d', self.take(4 if tag == 202 else 8))[0]
        if 204 <= tag <= 211:
            size = 1 << ((tag - 204) & 3)
            return int.from_bytes(self.take(size), 'big', signed=tag >= 208)
        if tag in (217,218,219): return self.take(self.uint(1 << (tag-217))).decode('utf-8')
        if tag in (196,197,198): return self.take(self.uint(1 << (tag-196)))
        if 144 <= tag <= 159 or tag in (220,221):
            size = tag & 15 if tag < 160 else self.uint(2 if tag == 220 else 4)
            if size > len(self.data)-self.pos: raise ValueError('array count')
            return [self.value(depth+1) for _ in range(size)]
        if 128 <= tag <= 143 or tag in (222,223):
            size = tag & 15 if tag < 144 else self.uint(2 if tag == 222 else 4)
            if size > (len(self.data)-self.pos)//2: raise ValueError('map count')
            result = {}
            for _ in range(size):
                key = self.value(depth+1)
                if not isinstance(key,str): raise ValueError('map key')
                result[key] = self.value(depth+1)
            return result
        if tag in (199,200,201) or 212 <= tag <= 216:
            size = 1 << (tag-212) if tag >= 212 else self.uint(1 << (tag-199))
            kind = self.uint(1)
            return Extension(kind, self.take(size))
        raise ValueError('unsupported tag')

def parse(data):
    r=Reader(data); value=r.value()
    if r.pos!=len(data): raise ValueError('trailing')
    return value

def decode(data):
    if not data.startswith(b'V8MR'): return parse(data)
    if data[:5] != b'V8MR\x01': raise ValueError('version')
    envelope=parse(data[5:])
    if not isinstance(envelope,list) or len(envelope)!=3: raise ValueError('envelope')
    shapes, strings, root=envelope
    if not isinstance(shapes,list) or not isinstance(strings,list): raise ValueError('tables')
    for shape in shapes:
        if not isinstance(shape,list) or any(not isinstance(k,str) for k in shape) or len(set(shape))!=len(shape): raise ValueError('shape')
    if any(not isinstance(s,str) for s in strings): raise ValueError('strings')
    def expand(node,depth=0):
        if depth>256: raise ValueError('semantic nesting')
        if isinstance(node,Extension):
            payload=parse(node.data)
            if node.kind==0x51:
                if type(payload)!=int or not 0<=payload<len(strings): raise ValueError('string reference')
                return strings[payload]
            if node.kind==0x50:
                if not isinstance(payload,list) or len(payload)!=2: raise ValueError('record')
                index,values=payload
                if type(index)!=int or not 0<=index<len(shapes) or not isinstance(values,list) or len(values)!=len(shapes[index]): raise ValueError('shape reference')
                return dict(zip(shapes[index],(expand(v,depth+1) for v in values)))
            if node.kind==0x52:
                if not isinstance(payload,list) or len(payload)!=2: raise ValueError('numbers')
                count,blocks=payload; result=[]
                if type(count)!=int or count<=0 or not isinstance(blocks,list) or not blocks or len(blocks)>count: raise ValueError('numeric count')
                formats=['B','b','H','h','I','i','Q','q','f','d']
                widths=[1,1,2,2,4,4,8,8,4,8]
                for kind,scale,body in blocks:
                    if type(kind)!=int or not 0<=kind<=9 or type(scale)!=int or not 0<=scale<=9 or (kind>=8 and scale): raise ValueError('kind')
                    if not isinstance(body,bytes) or not body or len(body)%widths[kind] or len(body)//widths[kind]>256: raise ValueError('block size')
                    for (value,) in struct.iter_unpack('>'+formats[kind],body):
                        if kind<8:
                            if abs(value)>9007199254740991: raise ValueError('unsafe integer')
                            value=float(value)/float(10**scale)
                        result.append(value)
                if not result or len(result)!=count: raise ValueError('numeric count')
                return result
            raise ValueError('extension')
        if isinstance(node,list): return [expand(v,depth+1) for v in node]
        if isinstance(node,dict): return {k:expand(v,depth+1) for k,v in node.items()}
        if isinstance(node,bytes): raise ValueError('binary value')
        return node
    return expand(root)

def equivalent(expected,actual):
    if isinstance(expected,dict):
        return isinstance(actual,dict) and expected.keys()==actual.keys() and all(equivalent(v,actual[k]) for k,v in expected.items())
    if isinstance(expected,list): return isinstance(actual,list) and len(expected)==len(actual) and all(equivalent(x,y) for x,y in zip(expected,actual))
    if type(expected)==bool or expected is None or isinstance(expected,str): return type(expected)==type(actual) and expected==actual
    if isinstance(expected,(int,float)) and isinstance(actual,(int,float)):
        return (math.isnan(expected) and math.isnan(actual)) or struct.pack('>d',float(expected))==struct.pack('>d',float(actual))
    return expected==actual

def main():
    p=argparse.ArgumentParser();p.add_argument('wire',type=Path);p.add_argument('--source',type=Path);a=p.parse_args()
    value=decode(a.wire.read_bytes())
    if a.source and not equivalent(json.loads(a.source.read_text()),value): raise ValueError('value mismatch')
    print(json.dumps({'wire':str(a.wire),'bytes':a.wire.stat().st_size,'validated':True}))

if __name__=='__main__': main()
