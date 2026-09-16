# linux内核子系统之 DRM显示子系统
参考博客： [https://blog.csdn.net/hexiaolong2009/article/details/83720940](https://blog.csdn.net/hexiaolong2009/article/details/83720940 "https://blog.csdn.net/hexiaolong2009/article/details/83720940")

---
```c
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

struct buffer_object {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t size;
    uint8_t *vaddr;
    uint32_t fb_id;
};

struct buffer_object buf;

static int modeset_create_fb(int fd, struct buffer_object *bo)
{
    struct drm_mode_create_dumb create = {};
     struct drm_mode_map_dumb map = {};
    /* create a dumb-buffer, the pixel format is XRGB888 */
    create.width = bo->width;
    create.height = bo->height;
    create.bpp = 32;          
    drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    /* bind the dumb-buffer to an FB object */
    bo->pitch = create.pitch;
    bo->size = create.size;
    bo->handle = create.handle;
    drmModeAddFB(fd, bo->width, bo->height, 24, 32, bo->pitch,  bo->handle, &bo->fb_id);
    /* map the dumb-buffer to userspace */
    map.handle = create.handle;
    drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
    bo->vaddr = mmap(0, create.size, PROT_READ | PROT_WRITE,
            MAP_SHARED, fd, map.offset);
    /* initialize the dumb-buffer with white-color */
    memset(bo->vaddr, 0xff, bo->size);
    return 0;
}

static void modeset_destroy_fb(int fd, struct buffer_object *bo)
{
    struct drm_mode_destroy_dumb destroy = {};
    drmModeRmFB(fd, bo->fb_id);
    munmap(bo->vaddr, bo->size);
    destroy.handle = bo->handle;
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

int main(int argc, char **argv)
{
    int fd;
    drmModeConnector *conn;
    drmModeRes *res;
    uint32_t conn_id;
    uint32_t crtc_id;
    fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    res = drmModeGetResources(fd);
    crtc_id = res->crtcs[0];
    conn_id = res->connectors[0];
    conn = drmModeGetConnector(fd, conn_id);
    buf.width = conn->modes[0].hdisplay;
    buf.height = conn->modes[0].vdisplay;
    modeset_create_fb(fd, &buf);
    drmModeSetCrtc(fd, crtc_id, buf.fb_id,0, 0, &conn_id, 1, &conn->modes[0]);
    getchar();
    modeset_destroy_fb(fd, &buf);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(fd);
    return 0;
}
```


libdrm:  [https://dri.freedesktop.org/libdrm/libdrm-2.4.100.tar.bz2](https://dri.freedesktop.org/libdrm/libdrm-2.4.100.tar.bz2)
```shell
# ./configure
# Make -j4
./tests/modetest/modetest   -M vc4 -D 0 -a -s 32@140:1920x1080  -P 173@140:1920x1080 -Ftiles
```

DRM，英文全称 Direct Rendering Manager, 即 直接渲染管理器。
它是为了解决多个程序对 Video Card 资源的协同使用问题而产生的。它向用户空间提供了一组 API，用以访问操纵 GPU
DRM 的诞生就是用来处理多个程序对 Video Card 资源的协同使用问题。
比如图形执行管理器 GEM 或者内核模式设置 KMS，这些都是属于 DRM 子系统。
DRM 同时也负责处理 GPUs 切换的问题。

fbdev----->用于管理显卡的framebuffer，不能用于处理基于Video Card的GPU 3D加速的需求（很久以前出现的，有API）
在最初的用户空间的程序（比如 X Server）可以直接管理这些资源，但这些程序通常表现的就仿佛他们是唯一去获取这些资源的一样。当有多个程序试图去以自己的方式同时控制 Video Card 资源时，就会崩溃。

VSync最初是由 GPU 厂商开发的一种，用于防止屏幕撕裂的技术方案，全称 Vertical Synchronization，该方案很早就已经被广泛应用于 PC 上。
我们可以把它理解为一种时钟中断。想要画面流畅显示，刷新频率（Display）和帧率(GPU)需要保持同步。

![[Pasted image 20260610170230.png]]

1, libdrm 测试：modetest 查询信息

1.1，libdrm-2.4.100: modetest 编译   
```diff
libdrm-2.4.100:  https://dri.freedesktop.org/libdrm/libdrm-2.4.100.tar.bz2
                                  https://gitlab.freedesktop.org/mesa/drm.git

https://github.com/ChaojiangLuo/docs/blob/master/drm-howto/modeset.c

modetest:
tests/modetest/modetest.c
               cursor.c
               buffers.c

libutil.a : format.c  kms.c  pattern.c
            tests/util/kms.c

libdrm.so : xf86drm.c xf86drmHash.c  xf86drmRandom.c  xf86drmSL.c xf86drmMode.c

- 问题： 开源社区libdrm编译出来的modetest 找不到设备？
- 静态编译：  可以发现找不到设备 是因为drm_device设备存在drm_master
gcc format.c kms.c pattern.c buffers.c cursor.c modetest.c xf86drm.c xf86drmHash.c xf86drmRandom.c xf86drmSL.c xf86drmMode.c -I../include/drm/ -I../tests/ -pthread -lm  -DMAJOR_IN_SYSMACROS -g -o modetest   

cp -rf ../tests/util/format.* ../tests/util/kms.* ../tests/util/pattern.* ../tests/util/common.h  ../tests/modetest/cursor.* ../tests/modetest/buffers.* ../tests/modetest/modetest.c  ../libdrm_macros.h ../xf86drm* .

# git diff  xf86drm.c
diff --git a/xf86drm.c b/xf86drm.c
index 5933e4bc..73fe9a7a 100644
--- a/xf86drm.c
+++ b/xf86drm.c
@@ -1111,8 +1111,7 @@ static int drmOpenByName(const char *name, int type)
                     drmFreeVersion(version);
                     id = drmGetBusid(fd);
                     drmMsg("drmGetBusid returned '%s'\n", id ? id : "NULL");
-                    if (!id || !*id) {          
-                        if (id)
+                    if (id) {
                             drmFreeBusid(id);
                         return fd;
                     } else {
```
1.2 ， libdrm/tests/moduetest/modetest.c   查询测试，执行流程 
[[modetest梳理]]
![[Pasted image 20260610165235.png]]

modetest查询原理：

![[Pasted image 20260610165253.png]]

modetest查询：内核响应原理

![](img_009.png)

![](img_007.png)

![[Pasted image 20260610165328.png]]

1.3 libdrm/tests/moduetest/modetest.c   显示测试，执行流程

libdrm进行显示测试modetest:
```
modetest -s 81@56:800x1280@RG24

            -s connector_id@crtc_id:mode@format

            -P <plane_id>@<crtc_id>:<w>x<h>[+<x>+<y>][*<scale>][@<format>]  set a plane
```

 modetest -s 81@56:1024x600@RG24  显示测试

![](img_004.png)

drm内存分配原理：

![](img_005.png)

创建内存对象：

![[Pasted image 20260610165419.png]]

映射内存对象：

![[Pasted image 20260610165425.png]]

![](img_001.png)

![[Pasted image 20260610165539.png]]

FB对象创建：

![](img_008.png)

![[Pasted image 20260610165550.png]]

![[Pasted image 20260610165557.png]]

![[Pasted image 20260610165611.png]]

drmPrimeHandleToFD( ) 问题 ：DRM_IOCTL_PRIME_HANDLE_TO_FD

![[Pasted image 20260610165619.png]]

![](img_002.png)

![[Pasted image 20260610165628.png]]

modetest 显示测试时：内核响应原理

简单的截屏工具：基于kms/drm

[https://github.com/pcercuei/kmsgrab/blob/main/kmsgrab.c](https://github.com/pcercuei/kmsgrab/blob/main/kmsgrab.c)

USB 显示： Displaylink 驱动

Displaylink: 利用USB和wifi连接显示器的技术。内核驱动是udl.ko (基于原始udlfb.ko重写)。

![](img_006.png)

![[Pasted image 20260610165702.png]]

![](img_003.png)

![[Pasted image 20260610165745.png]]