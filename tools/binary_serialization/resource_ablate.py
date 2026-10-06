#!/usr/bin/env python3
"""Independently generate shape-only and shape+string wire controls.

Uses the final tables, strips later encodings, and applies whole-file standard
fallback. Loading controls therefore use one unchanged production decoder.
"""
import argparse
import math
from pathlib import Path
import struct
from resource_peer import Extension,parse,decode,equivalent

def integer(n):
    if 0<=n<128:return bytes([n])
    if -32<=n<0:return bytes([n+256])
    kinds=[(0xcc,1,False),(0xcd,2,False),(0xce,4,False),(0xcf,8,False)] if n>=0 else [(0xd0,1,True),(0xd1,2,True),(0xd2,4,True),(0xd3,8,True)]
    for tag,width,signed in kinds:
        try:return bytes([tag])+n.to_bytes(width,'big',signed=signed)
        except OverflowError:pass
    raise ValueError('integer range')

def length(n,fix,limit,tags,widths):
    if n<limit:return bytes([fix+n])
    for tag,width in zip(tags,widths):
        if n<1<<(8*width):return bytes([tag])+n.to_bytes(width,'big')
    raise ValueError('length')

def pack(v):
    if v is None:return b'\xc0'
    if type(v)==bool:return b'\xc3' if v else b'\xc2'
    if type(v)==int:return integer(v)
    if type(v)==float:
        if math.isfinite(v) and not(v==0 and math.copysign(1,v)<0) and v.is_integer() and abs(v)<=9007199254740991:return integer(int(v))
        if math.isfinite(v):
            try:
                f=struct.pack('>f',v)
                if struct.pack('>d',struct.unpack('>f',f)[0])==struct.pack('>d',v):return b'\xca'+f
            except OverflowError:pass
        return b'\xcb'+struct.pack('>d',v)
    if isinstance(v,str):
        b=v.encode('utf-8');return length(len(b),0xa0,32,[0xd9,0xda,0xdb],[1,2,4])+b
    if isinstance(v,bytes):return length(len(v),0,0,[0xc4,0xc5,0xc6],[1,2,4])+v
    if isinstance(v,list):return length(len(v),0x90,16,[0xdc,0xdd],[2,4])+b''.join(pack(x) for x in v)
    if isinstance(v,dict):return length(len(v),0x80,16,[0xde,0xdf],[2,4])+b''.join(pack(k)+pack(x) for k,x in v.items())
    if isinstance(v,Extension):
        sizes={1:0xd4,2:0xd5,4:0xd6,8:0xd7,16:0xd8}
        header=bytes([sizes[len(v.data)]]) if len(v.data) in sizes else length(len(v.data),0,0,[0xc7,0xc8,0xc9],[1,2,4])
        return header+bytes([v.kind])+v.data
    raise TypeError(type(v))

def ablate(resource,standard,strings_enabled):
    if not resource.startswith(b'V8MR\x01'):return standard
    shapes,strings,root=parse(resource[5:])
    def rewrite(v):
        if isinstance(v,Extension):
            if v.kind==0x51:return v if strings_enabled else strings[parse(v.data)]
            if v.kind==0x52:return decode(b'V8MR\x01'+pack([[],[],v]))
            if v.kind==0x50:
                index,values=parse(v.data);return Extension(v.kind,pack([index,[rewrite(x) for x in values]]))
        if isinstance(v,list):return [rewrite(x) for x in v]
        if isinstance(v,dict):return {k:rewrite(x) for k,x in v.items()}
        return v
    result=b'V8MR\x01'+pack([shapes,strings if strings_enabled else [],rewrite(root)])
    if not equivalent(decode(standard),decode(result)):raise ValueError('ablation value mismatch')
    return result if len(result)<len(standard) else standard

def main():
    p=argparse.ArgumentParser();p.add_argument('--resource',type=Path,required=True);p.add_argument('--standard',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--strings',action='store_true');a=p.parse_args()
    a.output.write_bytes(ablate(a.resource.read_bytes(),a.standard.read_bytes(),a.strings))

if __name__=='__main__':main()
