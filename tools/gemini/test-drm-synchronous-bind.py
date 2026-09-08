#!/usr/bin/env python3
"""Exercise the real MMSYS collection when the probing child is not linked yet."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
s = (root / 'drivers/gpu/drm/mediatek/mtk_drm_drv.c').read_text()
a = s.index('static bool mtk_drm_get_all_drm_priv(')
b = s.index('\nstatic bool ', a + 1)
function = s[a:b]
shim = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define MAX_CRTC 3
#define CRTC_MAIN 0
#define CRTC_EXT 1
#define CRTC_THIRD 2
struct device_node { struct device_node *parent; int id; };
struct device { struct device *parent; struct device_node *of_node; void *data; struct device *child; };
struct platform_device { struct device dev; };
struct of_device_id { int unused; };
struct data { int main_len, ext_len, third_len; unsigned mmsys_dev_num; };
struct mtk_drm_private { struct data *data; bool mtk_drm_bound; struct mtk_drm_private **all_drm_private; };
static struct device_node nodes[3], parent;
static struct platform_device pdevs[3];
static int node_count, puts, gets;
static struct of_device_id mtk_drm_of_ids[1];
static int mtk_drm_match;
#define for_each_child_of_node(p,n) for(int ix=0; ix<node_count && ((n)=&nodes[ix]); ix++)
static const struct of_device_id *of_match_node(const struct of_device_id *ids, struct device_node *n) { (void)n; return ids; }
static void *dev_get_drvdata(struct device *d) { return d->data; }
static struct platform_device *of_find_device_by_node(struct device_node *n) { gets++;return &pdevs[n->id]; }
static struct device *device_find_child(struct device *p, void *unused, int match) { (void)unused;(void)match;if(p->child)gets++;return p->child; }
static void put_device(struct device *d) { (void)d;puts++; }
static void of_node_put(struct device_node *n) { (void)n; }
'''
test = r'''
int main(void) {
 struct data cfg[3]={{1,0,0,1},{0,1,0,2},{0,0,1,3}};
 struct mtk_drm_private *all[3][3]={{0}};
 struct mtk_drm_private priv[3];struct device drm[3];
 for(int i=0;i<3;i++) {
  nodes[i]=(struct device_node){&parent,i};
  pdevs[i].dev.of_node=&nodes[i];
  priv[i]=(struct mtk_drm_private){&cfg[i],true,all[i]};
  drm[i]=(struct device){&pdevs[i].dev,NULL,&priv[i],NULL};
 }
 /* The current child is absent during device_add's synchronous probe. */
 node_count=1;assert(!pdevs[0].dev.child);
 assert(mtk_drm_get_all_drm_priv(&drm[0]) && all[0][0]==&priv[0]);
 /* Same result once linked; never count the current master twice. */
 pdevs[0].dev.child=&drm[0];assert(mtk_drm_get_all_drm_priv(&drm[0]));
 node_count=2;cfg[0].mmsys_dev_num=2;
 priv[0].mtk_drm_bound=false;
 assert(!mtk_drm_get_all_drm_priv(&drm[1]));
 priv[0].mtk_drm_bound=true;
 assert(mtk_drm_get_all_drm_priv(&drm[1]));
 assert(all[0][0]==&priv[0] && all[0][1]==&priv[1] && all[1][0]==&priv[0]);
 pdevs[1].dev.child=&drm[1];node_count=3;
 assert(mtk_drm_get_all_drm_priv(&drm[2]));
 for(int i=0;i<3;i++)for(int j=0;j<3;j++)assert(all[i][j]==&priv[j]);
 assert(gets==puts);
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.c').write_text(shim+function+test)
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-Wno-sign-compare',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS real DRM collection: synchronous single/dual/triple MMSYS, deferred peer, references balanced')
