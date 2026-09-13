// SPDX-License-Identifier: MIT
#include "nolibc.h"
#include <linux/ioctl.h>
#include "mc_user.h"
_Static_assert(sizeof(struct mc_version_info) == 96, "Unexpected version ABI");
int main(void)
{
    int fd=open("/dev/mobicore-user", O_RDWR|O_CLOEXEC);
    if (fd<0) { perror("open mobicore-user"); return 1; }
    struct mc_version_info v={0};
    if (ioctl(fd, MC_IO_VERSION, &v)) { perror("MC_IO_VERSION"); close(fd); return 1; }
    v.product_id[sizeof(v.product_id)-1]=0;
    printf("product=%s MCI=%u.%u NWD=%u.%u TL_API=%u.%u DR_API=%u.%u\n",v.product_id,
        v.version_mci>>16,v.version_mci&65535,v.version_nwd>>16,v.version_nwd&65535,
        v.version_tl_api>>16,v.version_tl_api&65535,v.version_dr_api>>16,v.version_dr_api&65535);
    return close(fd)!=0;
}
