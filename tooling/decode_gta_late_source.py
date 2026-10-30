#!/usr/bin/env python3
import ctypes as c,json,argparse,pathlib,hashlib,sys
U64=c.c_uint64;U32=c.c_uint32
class Attribute(c.LittleEndianStructure):_fields_=[(n,U64)for n in['offset','stride','step_rate']]+[(n,U32)for n in['location','format','buffer_index','step_function']]
class Layout(c.LittleEndianStructure):_fields_=[('count',U32),('flags',U32),('attributes',Attribute*31)]
class Draw(c.LittleEndianStructure):_fields_=[(n,U64)for n in['first','count','instances','base_instance','index_address','index_length','indirect_address']]+[('base_vertex',c.c_int64)]+[(n,U32)for n in['kind','primitive','index_type','conversion','fan_vertices','fan_capacity']]
class Buffer(c.LittleEndianStructure):_fields_=[(n,U64)for n in['object','gpu_base','length','address','offset','declared_address','declared_length','pool_lo','pool_hi','pool_base','pool_key','pool_generation','registry_generation','heap','heap_offset','cpu_base']]+[(n,U32)for n in['slot','role','origin','storage','options','flags']]
class Binding(c.LittleEndianStructure):_fields_=[('value',U64),('reg',c.c_uint16),('aid',c.c_uint16),('off',c.c_uint16),('kind',c.c_uint8),('rw',c.c_uint8)]
class Arguments(c.LittleEndianStructure):_fields_=[(n,U64)for n in['address','provided_address','buffer','offset','available_length','native_table']]+[(n,U32)for n in['stage','index','count','flags']]+[('bindings',Binding*64)]
class Texture(c.LittleEndianStructure):_fields_=[(n,U64)for n in['rid','object','parent_rid','parent_object','heap','heap_offset','buffer','buffer_offset','buffer_bytes_per_row','iosurface','allocated_size','usage','resource_options']]+[(n,U32)for n in['width','height','depth','format','type','samples','mips','array_length','parent_level','parent_slice','storage','cpu_cache','hazard','shareable','framebuffer_only','sparse']]+[('swizzle',U32*4)]+[(n,U32)for n in['iosurface_plane','origin','flags']]
class Header(c.LittleEndianStructure):_fields_=[('magic',U64)]+[(n,U32)for n in['abi','endian','record_size','count','truncated','unavailable','sealed','startup_enabled']]+[(n,U64)for n in['frame','layout_bytes','layout_missed']]
class Record(c.LittleEndianStructure):_fields_=[(n,U64)for n in['frame','submission','pass','draw','sequence','pso']]+[(n,U32)for n in['kind','stage','slot','flags','bytes','reserved']]+[('vertex_key',c.c_char*80),('fragment_key',c.c_char*80),('payload',c.c_uint8*1280)]
assert [c.sizeof(x)for x in[Header,Record,Draw,Buffer,Arguments,Texture,Attribute,Binding]]==[64,1512,88,152,1088,200,40,16]
def value(v):
 if isinstance(v,c.Array):return[vv if isinstance(vv,int)else value(vv)for vv in v]
 if isinstance(v,c.Structure):return{n:value(getattr(v,n))for n,_ in v._fields_}
 if isinstance(v,bytes):return v.split(b'\0')[0].decode('ascii','strict')
 return v
def decode(data):
 if len(data)<64 or len(data)>64+1024*1512:raise ValueError('size')
 h=Header.from_buffer_copy(data)
 if(h.magic,h.abi,h.endian,h.record_size,h.sealed,h.startup_enabled)!=(0x315243534c415447,1,0x01020304,1512,1,1)or h.count>1024 or len(data)!=64+h.count*1512:raise ValueError('header/length')
 result={'header':value(h),'sha256':hashlib.sha256(data).hexdigest(),'metadataOnly':True,'proofAdmission':False,'records':[]}
 types={1:Draw,2:Layout,3:Buffer,4:Arguments,5:Texture}
 for i in range(h.count):
  r=Record.from_buffer_copy(data,64+i*1512)
  if r.frame!=h.frame or r.kind not in types or r.bytes>1280:raise ValueError('record')
  raw=bytes(r.payload);typ=types[r.kind]
  if r.kind==2:
   if r.bytes<8:raise ValueError('layout short')
   n=int.from_bytes(raw[:4],'little')
   if n>31 or r.bytes!=8+n*40:raise ValueError('layout count')
  elif r.kind==4:
   if r.bytes<64:raise ValueError('argument short')
   n=int.from_bytes(raw[56:60],'little')
   if n>64 or r.bytes!=64+n*16:raise ValueError('argument count')
  elif r.bytes!=c.sizeof(typ):raise ValueError('payload length')
  body=value(typ.from_buffer_copy(raw[:c.sizeof(typ)]))
  if r.kind==2:body['attributes']=body['attributes'][:body['count']]
  if r.kind==4:body['bindings']=body['bindings'][:body['count']]
  item={n:value(getattr(r,n))for n,_ in r._fields_ if n!='payload'};item['body']=body;result['records'].append(item)
 return result
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('file',type=pathlib.Path);p.add_argument('--output',type=pathlib.Path);a=p.parse_args()
 try:r=decode(a.file.read_bytes())
 except(ValueError,UnicodeError)as e:print('Invalid late-source metadata: '+str(e),file=sys.stderr);sys.exit(2)
 if a.output:
  with a.output.open('x')as f:json.dump(r,f,indent=2);f.write('\n')
 else:print(json.dumps(r,indent=2))
