// SPDX-License-Identifier: MIT
/* Temporary read-only linear mappings of the device's existing super partition. */
#include "nolibc.h"
#include <stdbool.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/dm-ioctl.h>
static unsigned char data[65536] __attribute__((aligned(8)));
static struct dm_ioctl *header=(struct dm_ioctl *)data;
static void prepare(const char *name)
{
    memset(data,0,sizeof(data));
    /* Only base v4 commands and linear targets are used. */
    header->version[0]=4;
    header->data_size=sizeof(*header);
    header->data_start=(sizeof(*header)+7)&~7;
    strcpy(header->name,name);
}
static int number(const char *s, __u64 *value)
{
    __u64 v=0;
    if (!*s || strlen(s)>20) return -1;
    for (;*s;s++) {
        if (*s<'0' || *s>'9' || v>((~(__u64)0)-(*s-'0'))/10) return -1;
        v=v*10+(*s-'0');
    }
    *value=v; return 0;
}
int main(int argc,char **argv)
{
    if (argc<3 || strlen(argv[2])>=DM_NAME_LEN || strncmp(argv[2],"nord2-ro-",9)) {
        puts("Usage: dm_ro create nord2-ro-NAME LENGTH_SECTORS SOURCE_SECTOR [...] | remove nord2-ro-NAME"); return 1;
    }
    for (const char *s=argv[2];*s;s++) if (!((*s>='a' && *s<='z') || (*s>='0' && *s<='9') || *s=='-' || *s=='_')) return 1;
    char path[DM_NAME_LEN+16]; strcpy(path,"/dev/block/"); strcpy(path+strlen(path),argv[2]);
    bool removing=!strcmp(argv[1],"remove");
    if ((!removing && strcmp(argv[1],"create")) || (removing && argc!=3) ||
        (!removing && (argc<5 || argc>131 || !(argc&1)))) return 1;
    __u64 lengths[64],offsets[64],total=0,source_bytes=0;
    int targets=(argc-3)/2;
    if (!removing) {
        int super=open("/dev/block/by-name/super",O_RDONLY|O_CLOEXEC);
        if (super<0) { perror("super open"); return 1; }
        int result=ioctl(super,BLKGETSIZE64,&source_bytes); close(super);
        if (result || source_bytes%512) { puts("Invalid super block size"); return 1; }
        for (int i=0;i<targets;i++) {
            if (number(argv[3+2*i],&lengths[i]) || number(argv[4+2*i],&offsets[i]) || !lengths[i] ||
                offsets[i]>=source_bytes/512 || lengths[i]>source_bytes/512-offsets[i] ||
                total>source_bytes/512-lengths[i]) { puts("Invalid extent"); return 1; }
            total+=lengths[i];
        }
        struct stat existing;
        if (!stat(path,&existing)) { puts("Refusing an existing mapping node"); return 1; }
    }
    int control=open("/dev/device-mapper",O_RDWR|O_CLOEXEC);
    if (control<0) { perror("device-mapper open"); return 1; }
    prepare(argv[2]);
    if (removing) {
        int result=ioctl(control,DM_DEV_REMOVE,header);
        if (result) perror("DM_DEV_REMOVE"); else unlink(path);
        close(control); return result!=0;
    }
    header->flags=DM_READONLY_FLAG;
    if (ioctl(control,DM_DEV_CREATE,header)) { perror("DM_DEV_CREATE"); close(control); return 1; }
    bool node=false; int status=1;
    prepare(argv[2]); header->flags=DM_READONLY_FLAG; header->target_count=targets;
    size_t used=header->data_start; __u64 logical=0;
    for (int i=0;i<targets;i++) {
        struct dm_target_spec *spec=(struct dm_target_spec *)(data+used);
        char *params=(char *)(spec+1);
        spec->sector_start=logical; spec->length=lengths[i]; strcpy(spec->target_type,"linear");
        strcpy(params,"/dev/block/by-name/super "); strcpy(params+strlen(params),argv[4+2*i]);
        spec->next=(sizeof(*spec)+strlen(params)+1+7)&~7;
        used+=spec->next; logical+=lengths[i];
    }
    header->data_size=used;
    if (ioctl(control,DM_TABLE_LOAD,header)) { perror("DM_TABLE_LOAD"); goto out; }
    prepare(argv[2]); header->flags=DM_READONLY_FLAG;
    if (ioctl(control,DM_DEV_SUSPEND,header)) { perror("DM resume"); goto out; }
    prepare(argv[2]);
    if (ioctl(control,DM_DEV_STATUS,header) || !(header->flags&DM_READONLY_FLAG)) {
        puts("Mapping is not read-only"); goto out;
    }
    if (mknod(path,S_IFBLK|0600,(dev_t)header->dev)) { perror("mknod mapping"); goto out; }
    node=true;
    int block=open(path,O_RDONLY|O_CLOEXEC); __u64 bytes=0; int ro=0;
    if (block<0) { perror("mapping open"); goto out; }
    int verify=ioctl(block,BLKROGET,&ro) || ioctl(block,BLKGETSIZE64,&bytes);
    close(block);
    if (verify || ro!=1 || bytes!=total*512) { puts("Read-only mapping verification failed"); goto out; }
    printf("%s: read-only, %llu bytes, %d extents\n",path,(unsigned long long)bytes,targets);
    status=0;
out:
    if (status) {
        prepare(argv[2]);
        if (ioctl(control,DM_DEV_REMOVE,header)) perror("mapping rollback");
        if (node) unlink(path);
    }
    close(control); return status;
}
