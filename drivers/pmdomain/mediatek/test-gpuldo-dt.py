#!/usr/bin/env python3
"""Run the actual kernel DT admission function using a read-only libfdt OF shim."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent
source = (root / "mtk-scpsys.c").read_text()
start = source.index("static int scpsys_gpuldo_validate_dt(")
end = source.index("\nstatic struct scp *init_scp", start)
pre = r'''
#include <libfdt.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
static void *blob;
struct device_node { int off; } nodes[4096];
struct device { struct device_node *of_node; };
struct property { const char *name; int off; } props[16384];
struct resource { uint64_t start,size; };
static bool mt6797_gpuldo_owner_enable;
static struct device_node *make_node(int off) {
 if(off<0) return NULL;
 for(unsigned i=0;i<4096;i++) {
  if(nodes[i].off==off+1) return &nodes[i];
  if(!nodes[i].off) { nodes[i].off=off+1; return &nodes[i]; }
 } abort();
}
static int offset(const struct device_node *n) { return n->off-1; }
static const void *property(struct device_node *n,const char *p,int *len) {
 return fdt_getprop(blob,offset(n),p,len);
}
static void of_node_put(struct device_node *n) { (void)n; }
static struct device_node *of_node_get(struct device_node *n) { return n; }
static bool of_device_is_compatible(struct device_node *n,const char *s) {
 return !fdt_node_check_compatible(blob,offset(n),s);
}
static bool of_machine_is_compatible(const char *s) { return of_device_is_compatible(make_node(0),s); }
static bool of_property_read_bool(struct device_node *n,const char *s) { return property(n,s,NULL)!=NULL; }
static const void *of_find_property(struct device_node *n,const char *s,void *unused) {
 (void)unused; return property(n,s,NULL);
}
static bool of_device_is_available(struct device_node *n) {
 const char *s=property(n,"status",NULL); return !s||!strcmp(s,"okay")||!strcmp(s,"ok");
}
static struct device_node *of_parse_phandle(struct device_node *n,const char *p,int i) {
 int len; const fdt32_t *v=property(n,p,&len);
 return v && len>=4*(i+1) ? make_node(fdt_node_offset_by_phandle(blob,fdt32_to_cpu(v[i]))) : NULL;
}
static struct device_node *of_get_parent(struct device_node *n) { return make_node(fdt_parent_offset(blob,offset(n))); }
static int of_address_to_resource(struct device_node *n,int index,struct resource *r) {
 int len; const fdt32_t *v=property(n,"reg",&len);
 if(!v||len<16*(index+1)) return -EINVAL; v+=4*index;
 r->start=((uint64_t)fdt32_to_cpu(v[0])<<32)|fdt32_to_cpu(v[1]);
 r->size=((uint64_t)fdt32_to_cpu(v[2])<<32)|fdt32_to_cpu(v[3]); return 0;
}
static uint64_t resource_size(struct resource *r) { return r->size; }
static struct device_node *of_find_compatible_node(struct device_node *n,void *unused,const char *s) {
 (void)unused; return make_node(fdt_node_offset_by_compatible(blob,n?offset(n):-1,s));
}
static bool strends(const char *s,const char *end) {
 size_t a=strlen(s),b=strlen(end); return a>=b&&!strcmp(s+a-b,end);
}
static int of_property_count_u32_elems(struct device_node *n,const char *s) {
 int len; return property(n,s,&len)&&!(len%4)?len/4:-EINVAL;
}
static int of_property_count_u64_elems(struct device_node *n,const char *s) {
 int len; return property(n,s,&len)&&!(len%8)?len/8:-EINVAL;
}
static int of_property_read_u32_array(struct device_node *n,const char *s,u32 *out,int count) {
 int len; const fdt32_t *v=property(n,s,&len); if(!v||len<count*4) return -EINVAL;
 for(int i=0;i<count;i++) out[i]=fdt32_to_cpu(v[i]); return 0;
}
static int of_property_read_u32(struct device_node *n,const char *s,u32 *out) {
 return of_property_read_u32_array(n,s,out,1);
}
static int of_property_read_u64(struct device_node *n,const char *s,u64 *out) {
 u32 v[2]; int ret=of_property_read_u32_array(n,s,v,2); if(!ret)*out=((u64)v[0]<<32)|v[1]; return ret;
}
static int of_property_read_string(struct device_node *n,const char *s,const char **out) {
 int len; *out=property(n,s,&len); return *out && len>0 && !(*out)[len-1]?0:-EINVAL;
}
static struct property *prop_at(int off) {
 int len; const struct fdt_property *p=fdt_get_property_by_offset(blob,off,&len);
 if(!p) return NULL;
 for(unsigned i=0;i<16384;i++) if(!props[i].name||props[i].off==off) {
  props[i].off=off; props[i].name=fdt_string(blob,fdt32_to_cpu(p->nameoff)); return &props[i];
 } abort();
}
static struct device_node *child_next(struct device_node *p,struct device_node *n) {
 int off=n?fdt_next_subnode(blob,offset(n)):fdt_first_subnode(blob,offset(p));
 while(off>=0 && !of_device_is_available(make_node(off))) off=fdt_next_subnode(blob,off);
 return make_node(off);
}
#define for_each_property_of_node(n,p) for(p=prop_at(fdt_first_property_offset(blob,offset(n)));p;p=prop_at(fdt_next_property_offset(blob,p->off)))
#define for_each_of_allnodes(n) for(n=make_node(fdt_next_node(blob,-1,NULL));n;n=make_node(fdt_next_node(blob,offset(n),NULL)))
#define for_each_available_child_of_node(p,n) for(n=child_next(p,NULL);n;n=child_next(p,n))
'''
main = r'''
int main(int argc,char **argv) {
 if(argc!=4) return 2;
 FILE *f=fopen(argv[1],"rb"); if(!f)return 2;
 fseek(f,0,SEEK_END); long len=ftell(f); rewind(f); blob=malloc(len);
 if(!blob||fread(blob,1,len,f)!=(size_t)len)return 2; fclose(f);
 if(fdt_check_header(blob))return 2;
 struct device d={make_node(fdt_path_offset(blob,"/power-controller@10006000"))};
 mt6797_gpuldo_owner_enable=atoi(argv[2]);
 int ret=scpsys_gpuldo_validate_dt(&d); printf("%d\n",ret);
 return (ret==atoi(argv[3]))?0:1;
}
'''

def main_test():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("carrier", type=Path)
    ap.add_argument("derivative", type=Path)
    args = ap.parse_args()
    cc = os.environ.get("HOSTCC", "/nix/store/061ybr50r1m2jnvih4vvpgmgwb37df3r-gcc-wrapper-12.2.0/bin/gcc")
    libfdt = root.parents[2] / "scripts/dtc/libfdt"
    put = os.environ.get("FDTPUT", "fdtput")
    cases = [
      ("/", "compatible", "s", ["wrong,board"]),
      ("/power-controller@10006000", "mfg-supply", "x", ["c"]),
      ("/gpu@13040000", "mali-supply", "x", ["c"]),
      ("/i2c@11010000/regulator@1c", "reg", "x", ["1d"]),
      ("/i2c@11010000/regulator@1c", "regulator-name", "s", ["other"]),
      ("/i2c@11010000/regulator@1c", "regulator-boot-on", None, []),
      ("/i2c@11010000/regulator@1c", "regulator-always-on", None, []),
      ("/i2c@11010000", "reg", "x", ["0","11011000","0","1000","0","11000580","0","80"]),
      ("/i2c@11010000", "reg", "x", ["0","11010000","0","1000","0","11000590","0","80"]),
      ("/gpu-g3-opp-table/opp-238000000", "opp-microvolt", "x", ["ceb6c","ceb6c","cf851"]),
      ("/gpu-g3-opp-table/opp-238000000", "opp-hz", "x", ["0","e2f9781"]),
      ("/syscon@10001000", "clocks", "x", ["1"]),
      ("/syscon@10001000", "resets", "x", ["1"]),
      ("/syscon@10001000", "reg", "x", ["0","10000000","0","1000"]),
      ("/gpu@13040000", "reg", "x", ["0","13050000","0","4000"]),
    ]
    with tempfile.TemporaryDirectory(prefix="gemini-gpuldo-dt-") as tmp:
        path = Path(tmp)
        (path/"test.c").write_text(pre+source[start:end]+main)
        subprocess.run([cc,"-std=gnu11","-Wall","-Wextra","-Werror",
          "-Wno-misleading-indentation","-I",str(libfdt),str(path/"test.c"),
          str(libfdt/"fdt.c"),str(libfdt/"fdt_ro.c"),
          "-o",str(path/"test")],check=True)
        def run(file, enabled, expected):
            subprocess.run([str(path/"test"),str(file),str(enabled),str(expected)],check=True,stdout=subprocess.DEVNULL)
        run(args.carrier,0,0)
        run(args.derivative,1,1)
        run(args.derivative,0,-1)
        for index,(node,prop,kind,values) in enumerate(cases):
            target=path/f"bad-{index}.dtb"
            target.write_bytes(args.derivative.read_bytes())
            cmd=[put]+(["-t",kind] if kind else [])+[str(target),node,prop]+values
            subprocess.run(cmd,check=True)
            run(target,1,-1 if index==0 else -22)
        print(f"actual DT admission via libfdt: valid/default/disabled + {len(cases)} corruptions PASS")

if __name__ == "__main__":
    main_test()
