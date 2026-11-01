#include <rtconfig.h>
#include "flash_layout.h"
/*
 *  flash 分区信息说明：
 *
 *  1. 最多支持15个分区
 *  2. 分区名称最大支持15个字符
 *  3. 必须定义应用代码存放分区，分区类型为PART_APPLICATION(2)
 *  4. 如果支持VBUS，请定义分区类型为PART_ARCFIRM(3)
 *  5. 其它用户自定义分区，名称自由定义，但不得超过15字节长度
 *  6. 如需自动挂载根分区，请定义根分区属性PART_ROOT
 *  7. 分区名称不能相同，否则，分区创建会出异常
 *
 */

struct _tag_flash_layout_info g_flash_layout_info[MAX_FLASH_PART_NUM] = {
    /* reserved partition, will be ignored */
    {"reservd1",    0x020000, BLOCK_64K, PART_RESERVED},
    /* uboot environment partition, can be reserved. */
    {"u-boot-env",  0x020000, BLOCK_64K, PART_RESERVED},
    /* uboot partition, can be reserved too. */
    {"u-boot",      0x080000, BLOCK_64K, PART_RESERVED},
    /* if you use VBUS, this partition MUST be defined */
#ifdef RT_USING_PM
    {"cp-firmware", 0x260000, BLOCK_64K, PART_ARCFIRM},
#else
    {"cp-firmware", 0x060000, BLOCK_64K, PART_ARCFIRM},
#endif
    /* application partition to hold firmware */
    {"application", 0x300000, BLOCK_64K, PART_APPLICATION},
    /* there can be more than one reserved partition */
    {"reservd2",    0x0e0000, BLOCK_64K, PART_RESERVED},
    /* other user partition */
    {"user-datum",  0x200000, BLOCK_64K, PART_ROOT | PART_USER},
    /* NBG 模型分区(与 fast_board 一致): nbg.img 烧在 0x700000, nna 从这里读人形模型
     * 必须带 PART_NN_MODEL 标志, 否则 __nnbg_part_idx=-1 -> mtd255 -> NBG 加载失败;
     * 大小 0x400000 与 fast_board/物理烧写区(0x700000..0xB00000)一致, nbg.img 1.1MB > 1MB */
    {"user-prog",   0x400000, BLOCK_64K, PART_NN_MODEL | PART_USER},
    /* another reserved partition */
#ifdef RT_USING_PM
    {"reservd3",    0x200000, BLOCK_64K, PART_RESERVED},
#else
    {"reservd3",    0x400000, BLOCK_64K, PART_RESERVED},
#endif
};

struct _tag_flash_layout_info g_flash_layout_nand_info[MAX_FLASH_PART_NUM] = {
    /* reserved partition, will be ignored */
    {"reservd1",    0x020000, BLOCK_128K, PART_RESERVED},
    /* uboot environment partition, can be reserved. */
    {"u-boot-env",  0x020000, BLOCK_128K, PART_RESERVED},
    /* uboot partition, can be reserved too. */
    {"u-boot",      0x080000, BLOCK_128K, PART_RESERVED},
    /* if you use VBUS, this partition MUST be defined */
    {"cp-firmware", 0x060000, BLOCK_128K, PART_ARCFIRM},
    /* application partition to hold firmware */
    {"application", 0x300000, BLOCK_128K, PART_APPLICATION},
    /* there can be more than one reserved partition */
    {"reservd2",    0x100000, BLOCK_128K, PART_RESERVED},
    /* other user partition */
    {"user-datum",  0x200000, BLOCK_128K, PART_USER},
    {"user-prog",   0x100000, BLOCK_128K, PART_USER},
    /* another reserved partition */
    {"reservd3",    0x800000, BLOCK_128K, PART_RESERVED},
};
