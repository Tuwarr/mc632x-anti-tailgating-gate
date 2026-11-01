/**
 * Copyright (c) 2015-2019 Shanghai Fullhan Microelectronics Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-01-08     songyh    the first version
 *
 */

/*****************************************************************************
 *  Include Section
 *  add all #include here
 *****************************************************************************/
#include <rtthread.h>
#include <rtdbg.h>
#include <rthw.h>
#include <mmu.h>
#include "fh_def.h"
#include "board_info.h"
#include "gpio.h"
#include "fh_chip.h"
#include "platform_def.h"
#include "fh_pmu.h"
#include <types/bufCtrl.h>

#ifdef RT_USING_CALIBRATE
#include <calibrate.h>
#endif

#include <uart.h>

#include <timer.h>
#ifdef RT_USING_DFS
#include <dfs_fs.h>
#endif

#include <libc.h>
#ifdef RT_USING_SDIO
#include <mmc.h>
#endif

#include "pinctrl.h"
#ifdef RT_USING_RTC
#include "fh_rtc.h"
#endif

#ifdef FH_USING_DMA_MEM
#include "dma_mem.h"
#endif

#ifdef FH_USING_DMA
#include "fh_dma.h"
#endif

#ifdef FH_USING_CLOCK
#include "fh_clock.h"
#endif

#ifdef RT_USING_FINSH
#include <shell.h>
#endif

#ifdef RT_USING_TIMEKEEPING
#include <timekeeping.h>
#include "hrtimer.h"
#endif

#ifdef FH_USING_SADC
#include "fh_sadc.h"
#endif

#ifdef RT_USING_SPI
#include "ssi.h"
#endif

#ifdef RT_USING_FAL_SFUD_ADAPT
#include "fh_fal_sfud_adapt.h"
#endif

#ifdef RT_USING_GPIO
#include "gpio.h"
#endif

#ifdef RT_USING_DFS
#include "dfs.h"
#endif /* RT_USING_DFS */

#if defined(RT_USING_DFS_ELMFAT)
#include "dfs_elm.h"
#endif

#if defined(RT_USING_DFS_DEVFS)
#include "devfs.h"
#endif

#ifdef RT_USING_DFS_RAMFS
#include "dfs_ramfs.h"
#endif /* RT_USING_DFS_RAMFS */

#ifdef RT_USING_DFS_ROMFS
#include "dfs_romfs.h"
#endif

#if defined(RT_USING_DFS_UFFS)
#include "dfs_uffs.h"
#endif

#ifdef RT_USING_LWIP
#include "lwip/sys.h"
#include "netif/ethernetif.h"
extern void lwip_sys_init(void);
#endif

#ifdef FH_USING_GMAC
#include "fh_gmac.h"
#endif
#ifdef FH_USING_QOS_GMAC
#include "fh_qos_gmac.h"
#endif

#ifdef RT_USING_DM9051
#include "dm9051.h"
#endif

#ifdef RT_USING_I2C
#include "i2c.h"
#endif

#ifdef RT_USING_DFS_JFFS2
#include "dfs_jffs2.h"
#endif
#ifdef RT_USING_DFS_YAFFS2
extern int dfs_yaffs2_init(void);
#endif

#ifdef UBIFS_FS
#include "rt_ubi_api.h"
#endif

#ifdef FH_USING_AES
#include "fh_aes.h"
#endif

#if defined(FH_USING_I2S) || defined(FH_USING_MOL_I2S)
#include "fh_i2s.h"
#endif

#ifdef RT_USING_ULOG
#include "ulog.h"
#endif

#ifdef RT_USING_SPISLAVE
#include "spi_slave.h"
#endif

#ifdef FH_USING_FH_PERF
#include "fh_perf_mon.h"
#endif

#ifdef FH_USING_NAND_FLASH
#include "fh_fal_sfud_adapt.h"
#endif

#include "flash_layout.h"

#ifdef RT_USING_PWM
extern void rt_hw_pwm_init(void);
#endif

#ifdef FH_USING_FH_STEPMOTOR
#include "fh_stepmotor.h"
#endif

#ifdef FH_USING_FH_HASH
#include "fh_hash.h"
#endif

#if defined(CONFIG_ARCH_FH865x) || defined(CONFIG_ARCH_FH8636_FH8852V20X) || defined(CONFIG_ARCH_FH885xV310) || \
    defined(CONFIG_ARCH_FH8852V301_FH8662V100)
#if !defined(CONFIG_CHIP_FH8626V200)
#include "dsp/fh_nna_mpi.h"
#endif
#endif

#include "flash_load.h"

#ifdef RT_USING_WDT
extern void rt_hw_wdt_init(void);
#endif

#ifdef FH_FAST_BOOT
static int g_fastvideo_init_ok = -1;
#else
extern rt_sem_t g_init_app_wait_load;
extern rt_sem_t g_init_load_wait_app;
#endif

#ifdef FH_USING_CESA
extern void rt_cesa_init(void);
#endif

#ifdef FH_USING_EFUSE
extern void rt_efuse_init(void);
#endif

extern void rt_hw_clock_init(void);
extern int clock_time_system_init(void);
extern void fh_platform_info_register(void);

extern void intc_init(void);

extern void first_code_run(void);
extern void user_code_load(void);
extern void fourth_driver_init(void);
extern void wait_init_done(void);
extern void user_main(void);

extern int __fastvideo_init(void);
extern void __fastvideo_startup(void);
extern int usb_init(void);
extern int import_flash_layout_info(enum _flash_type type);
#ifdef FH_FAST_BOOT
extern void rt_show_version(void);
#endif

#undef APP_FLASH_PART_OFFSET
static unsigned char __nnbg_part_idx = -1;
static unsigned char __vbus_part_idx = -1;
static unsigned char __main_part_idx = -1;
static unsigned char __root_part_idx = -1;

int __get_code_part_idx(int code)
{
    switch (code)
    {
    case CODE_ARC:
        return __vbus_part_idx;
    case CODE_ARM_SEG2:
        return __main_part_idx;
    case CODE_NBG_FILE:
        return __nnbg_part_idx;
    case CODE_ROOTFS:
        return __root_part_idx;
    default:
        return -1;
    }
}

struct _tag_flash_layout_info g_flash_tmp_info[MAX_FLASH_PART_NUM] = {0};
int import_flash_layout_info(enum _flash_type type)
{
    int sz_fal_adapt, i;
    unsigned int offset = 0;
    struct fh_fal_partition_adapt_info *fal_info;

    sz_fal_adapt = sizeof(struct fh_fal_partition_adapt_info);
    fal_info = rt_malloc(MAX_FLASH_PART_NUM * (sz_fal_adapt));
    if (fal_info == RT_NULL)
    {
        rt_kprintf("malloc failed.\n");
        return -ENOMEM;
    }
    rt_memset(fal_info, 0, MAX_FLASH_PART_NUM * (sz_fal_adapt));

    if (type == FLASH_NOR)
    {
        rt_memcpy(g_flash_tmp_info, g_flash_layout_info,
            sizeof(struct _tag_flash_layout_info) * MAX_FLASH_PART_NUM);
    }
#ifdef FH_USING_NAND_FLASH
    else if (type == FLASH_NAND)
    {
        rt_memcpy(g_flash_tmp_info, g_flash_layout_nand_info,
            sizeof(struct _tag_flash_layout_info) * MAX_FLASH_PART_NUM);
    }
#endif
    else
        return 0;

    for (i = 0; i < MAX_FLASH_PART_NUM; i++)
    {
        if (g_flash_tmp_info[i].part_size == 0)
            break;
        fal_info[i].name = g_flash_tmp_info[i].part_name;
        fal_info[i].offset = offset;
        fal_info[i].size = g_flash_tmp_info[i].part_size;
#ifdef FH_USING_NAND_FLASH
        fal_info[i].erase_block_size = FH_FAL_PART_BLK_SIZE_128K;
#else
        if (g_flash_tmp_info[i].erase_size == BLOCK_64K)
            fal_info[i].erase_block_size = FH_FAL_PART_BLK_SIZE_64K;
        else if (g_flash_tmp_info[i].erase_size == BLOCK_4K)
            fal_info[i].erase_block_size = FH_FAL_PART_BLK_SIZE_4K;
        else
            fal_info[i].erase_block_size = FH_FAL_PART_BLK_SIZE_64K;
#endif

        if ((g_flash_tmp_info[i].part_type & PART_TMAX) == PART_APPLICATION)
        {
            __main_part_idx = i;
        }

        if ((g_flash_tmp_info[i].part_type & PART_TMAX) == PART_ARCFIRM)
        {
            __vbus_part_idx = i;
        }
        if ((g_flash_tmp_info[i].part_type & PART_TMAX) == PART_NN_MODEL)
        {
            __nnbg_part_idx = i;
        }

        if (g_flash_tmp_info[i].part_type & PART_ROOT)
        {
            __root_part_idx = i;
        }
        offset += fal_info[i].size;
    }

    if (type == FLASH_NOR)
    {
#ifdef RT_USING_FAL_SFUD_ADAPT
        extern struct fh_fal_sfud_adapt_plat_info
            fh_fal_sfud_platform_data;
        fh_fal_sfud_platform_data.parts = fal_info;
        fh_fal_sfud_platform_data.nr_parts = i;
#endif
    }
    else if (type == FLASH_NAND)
    {
#ifdef FH_USING_NAND_FLASH
        extern struct fh_fal_sfud_adapt_plat_info
            fh_fal_sfud_platform_nand_data;
        fh_fal_sfud_platform_nand_data.parts = fal_info;
        fh_fal_sfud_platform_nand_data.nr_parts = i;
#endif
    }

    if (__main_part_idx < 0)
    {
        rt_kprintf("\n\nApplication partition NOT found.\n");
        return -ENOENT;
    }

    return 0;
}

extern void startup_log(unsigned int val);
extern void rt_hw_dw_timer_init(void);
void rt_hw_board_init(void)
{
    int ret,flash_type;
    /* todo: initialize the system clock */
    intc_init();

    rt_hw_clock_init();
#if defined(CONFIG_ARCH_FH8862) || defined(CONFIG_ARCH_FH885xV500)
    fh_pinctrl_init(PIN_REG_BASE);
#elif defined(CONFIG_ARCH_MC632X)
    fh_pinctrl_init(CEN_PIN_REG_BASE + PAD_REG_START_OFF);
#else
    fh_pinctrl_init(PMU_REG_BASE + PAD_REG_START_OFF);
#endif
#ifdef FH_USING_DMA_MEM
    fh_dma_mem_init((rt_uint32_t *)FH_RTT_OS_MEM_END, FH_DMA_MEM_SIZE);
#endif

#ifdef FH_ENABLE_VIDEO
    bufferInit((unsigned char *)FH_SDK_MEM_START, FH_SDK_MEM_SIZE);
#endif
    /* initialize uart */
    rt_hw_uart_init();
#ifdef RT_USING_CONSOLE
    rt_console_set_device(RT_CONSOLE_DEVICE_NAME);
#endif
    /* todo: initialize timer1 */
    rt_hw_timer_init();

    /* here to initialize platform flash partition infor */
    for (flash_type = 0; flash_type < FLASH_NR; flash_type++)
    {
        ret = import_flash_layout_info(flash_type);
        if (ret < 0)
        {
            rt_kprintf("\n\n\n **** bad flash layout(%d)! ****\n\n\n", ret);
        }
    }

    fh_board_info_init();
    fh_platform_info_register();

#ifdef RT_USING_SMP
    rt_hw_interrupt_install(RT_SCHEDULE_IPI, rt_scheduler_ipi_handler, 0, "IPI_HANDLER");
#endif
}

extern void fbv_nna_start_facerec(void);
extern void fbv_nna_start_do_detect(void);
extern void load_application(void);
void user_code_load(void)
{
#if defined(FH_FAST_BOOT) && defined(ENABLE_NNA_DET)
#ifdef ENABLE_Person_Detect
    fbv_nna_start_do_detect();
#endif
#ifdef ENABLE_Face_Recognition
    fbv_nna_start_facerec();
#endif
#endif
    load_application();
    /* wait fbv_init thread exits   */
    while (1)
    {
        if (rt_thread_find("fbv_init") == RT_NULL)
            break;
        rt_thread_delay(1);
    }
}

#if defined(FH_USING_GMAC) || defined(FH_USING_QOS_GMAC)
extern void get_init_mac_addr(unsigned char *macbuf);
extern void fh_gmac_initmac(unsigned char *gmac);
static void _init_mac_addr(void)
{
    unsigned char macbuf[6];

    get_init_mac_addr(macbuf);
    fh_gmac_initmac(macbuf);
}

void _gmac_init(void)
{
    struct clk *rmii_clk, *ref_clk;

    ref_clk = clk_get(NULL, "eth_ref_clk");
    rmii_clk = clk_get(NULL, "eth_rmii_clk");

    if ((ref_clk != RT_NULL) && (rmii_clk != RT_NULL))
    {
        clk_enable(ref_clk);
        clk_enable(rmii_clk);
    }
}
#endif

static void third_driver_init(void)
{
/* #ifdef FH_FAST_BOOT  */ /* fix gmac costs 100% CPU BUG */
#if defined(FH_USING_GMAC) || defined(FH_USING_QOS_GMAC)
    _gmac_init();
#endif
    /* #endif   */
}

#if defined(RT_USING_LWIP)
/* negociation may cost some time, put it in a thread */
static void network_thread(void *parameter)
{
    rt_thread_delay(10);
    /* init lwip system */
    lwip_sys_init();
    eth_system_device_init();

#if defined(FH_USING_GMAC) || defined(FH_USING_QOS_GMAC)
extern int rt_app_fh_gmac_init(void);
    _init_mac_addr();
    rt_app_fh_gmac_init();
#elif defined(RT_USING_DM9051)
    dm9051_attach(DM9051_SPI_DEVICE);
#endif
}

void network_init(void)
{
    /* register ethernetif device */
    rt_thread_t net_thread;

    net_thread = rt_thread_create("net_thrd", network_thread, RT_NULL, 4096, 79, 5);
    if (net_thread != RT_NULL)
        rt_thread_startup(net_thread);
}
#endif

#ifdef RT_USING_SDIO
static void sdcard_mount(void *parameter)
{
    if (mmcsd_wait_cd_changed(RT_TICK_PER_SECOND) == MMCSD_HOST_PLUGED)
    {
#if defined(RT_USING_DFS_JFFS2) || defined(RT_USING_DFS_YAFFS2)
        mkdir("/mnt", 0);
        if (dfs_mount("mmcblk0p1", "/mnt", "elm", 0, 0) != 0)
#else
        if (dfs_mount("mmcblk0p1", "/", "elm", 0, 0) != 0)
#endif
            rt_kprintf("fat File System initialization failed!\n");
        else
            rt_kprintf("fat File System initialization OK!\n");
    }
}
#endif

#if defined(RT_USING_DFS_ROMFS)
static void _rom_fsinit(void)
{
    dfs_romfs_init();
    if (dfs_mount(RT_NULL, "/rom", "rom", 0, &romfs_root) != 0)
        rt_kprintf("ROM File System initialzation failed!\n");
}
#endif

#if defined(RT_USING_DFS_UFFS)
static void _uffs_fsinit(void)
{
    /* init the uffs filesystem */
    dfs_uffs_init();

    mkdir("/nand0", 0);
    /* mount flash device as flash directory */
    if (dfs_mount("nand0", "/nand0", "uffs", 0, 0) != 0)
        rt_kprintf("UFFS File System initialzation failed!\n");
}
#endif

#ifdef RT_USING_DFS_JFFS2
static void _jffs2_fsinit(void)
{
    int idx;
    char mtdname[20];
    rt_device_t dev = NULL;

    dev = rt_device_find("fh_flash");
    if (dev == NULL)
        return;

    dfs_jffs2_init();
    idx = __get_code_part_idx(CODE_ROOTFS);
    if (idx > 0)
    {
        rt_sprintf(mtdname, "mtd%d", idx);
        if (dfs_mount(mtdname, "/", "jffs2", 0, 0) != 0)
            rt_kprintf("jffs2 System initialzation failed!\n");
        else
            rt_kprintf("jffs2 System initialzation ok!:%s\n", mtdname);
    }
}
#endif

#ifdef RT_USING_DFS_YAFFS2
static void yaffs2_fsinit(void)
{
    int idx;
    char mtdname[20];

    dfs_yaffs2_init();
    idx = __get_code_part_idx(CODE_ROOTFS);
    if (idx > 0)
    {
        rt_sprintf(mtdname, "mtd%d", idx);
        if (dfs_mount(mtdname, "/", "yaffs2", 0, 0) != 0)
            rt_kprintf("yaffs2 System initialzation failed!:%s\n", mtdname);
        else
            rt_kprintf("yaffs2 System initialzation ok!:%s\n", mtdname);
    }
}
#endif

#ifdef RT_USING_DFS_RAMFS
static void _ram_fsinit(void)
{
    rt_uint8_t *ramfs_pool = RT_NULL;
    struct dfs_ramfs *ramfs;

    dfs_ramfs_init();

    ramfs_pool = rt_malloc(0x800000);
    if (ramfs_pool)
    {
        ramfs = (struct dfs_ramfs *)dfs_ramfs_create(
            (rt_uint8_t *)ramfs_pool, 0x800000);
        if (ramfs != RT_NULL)
        {
            if (dfs_mount(RT_NULL, "/", "ram", 0, ramfs) != 0)
            {
                rt_kprintf("Mount RAMDisk failed.\n");
            }
        }
    }
    else
    {
        rt_kprintf("alloc ramfs poll failed\n");
    }
}
#endif

#ifdef RT_USING_DFS_NFS
static void _nfs_fsinit(void)
{
    extern int nfs_init(void);

    nfs_init();

    mkdir("/nfs", 0);
    if (dfs_mount(RT_NULL, "/nfs", "nfs", 0, RT_NFS_HOST_EXPORT) == 0)
    {
        rt_kprintf("NFSv3 File System initialized!\n");
    }
    else
        rt_kprintf("NFSv3 File System initialzation failed!\n");
}
#endif

#ifdef UBIFS_FS
static void ubifs_fsinit(void)
{
    const char *mtdname[] = {"mtd8"};
    rt_ubiinit(mtdname, 1);
    int ret = dfs_mount("mtd8", "/", "ubifs", 0, "ubi0_0");
    if (ret == 0)
    {
        rt_kprintf("File System on mtd8  mounted!\n");
        mkdir("/app", 777);
    }
    else
    {
        rt_kprintf("File System on mtd8  mount fail. %d\n", ret);
    }
}
#endif
void filesystem_init(void)
{
#ifdef RT_USING_DFS
    dfs_init();

#if defined(RT_USING_DFS_JFFS2)
    _jffs2_fsinit();
#endif

#if defined(UBIFS_FS)
    ubifs_fsinit();
#endif

#if defined(RT_USING_DFS_YAFFS2)
    yaffs2_fsinit();
#endif
#if defined(RT_USING_DFS_ELMFAT)
    elm_init();
#ifdef RT_USING_SDIO
    rt_thread_t sd_mnt_thread;

    sd_mnt_thread = rt_thread_create("sd_mnt_thrd", sdcard_mount, RT_NULL, 4096, 79, 5);
    if (sd_mnt_thread != RT_NULL)
        rt_thread_startup(sd_mnt_thread);
#endif
#endif

#if defined(RT_USING_DFS_ROMFS)
    _rom_fsinit();
#endif

#if defined(RT_USING_DFS_UFFS)
    _uffs_fsinit();
#endif

#ifdef RT_USING_DFS_RAMFS
    _ram_fsinit();
#endif

#ifdef RT_USING_DFS_NFS
    _nfs_fsinit();
#endif

#endif /* END RT_USING_DFS */
}

void _timekeeping_init(void)
{
#ifdef RT_USING_TIMEKEEPING
    timekeeping_init();
    clocksource_pts_register();
    clockevent_timer0_register();
    hrtimers_init();
#endif
}

void _ircut_init(void)
{
#ifndef CONFIG_IRCUT_ON
    gpio_request(CONFIG_IRCUT_ON_GPIO);
    gpio_direction_output(CONFIG_IRCUT_ON_GPIO, 0);
    gpio_release(CONFIG_IRCUT_ON_GPIO);
    gpio_request(CONFIG_IRCUT_OFF_GPIO);
    gpio_direction_output(CONFIG_IRCUT_OFF_GPIO, 0);
    gpio_release(CONFIG_IRCUT_OFF_GPIO);
#endif
}

static void reset_thrd_entry(void *args)
{
#if defined(FH_ENABLE_VIDEO) && !defined(FH_FAST_BOOT)
    /* reset sensor */
    gpio_request(CONFIG_CIS_RESET_GPIO);
    gpio_direction_output(CONFIG_CIS_RESET_GPIO, 0);
#endif

#if defined(RT_USING_SDIO) && defined(WIFI_USING_SDIOWIFI)
    /* sdio reset */
    gpio_request(HW_WIFI_POWER_GPIO);
    gpio_direction_output(HW_WIFI_POWER_GPIO, HW_WIFI_POWER_GPIO_ON_LEVEL);
    gpio_release(HW_WIFI_POWER_GPIO);

    gpio_request(WIFI_ENABLE_GPIO);
    gpio_direction_output(WIFI_ENABLE_GPIO, HW_WIFI_POWER_GPIO_ON_LEVEL);
#endif

#if defined(FH_ENABLE_VIDEO) && !defined(FH_FAST_BOOT)
    /* pull up sensor */
    rt_thread_delay(3);
    gpio_direction_output(CONFIG_CIS_RESET_GPIO, 1);
#endif

#if defined(RT_USING_SDIO) && defined(WIFI_USING_SDIOWIFI)
    /* sdio wifi reset */
    rt_thread_delay(15);
    gpio_direction_output(WIFI_ENABLE_GPIO, !HW_WIFI_POWER_GPIO_ON_LEVEL);
    gpio_release(WIFI_ENABLE_GPIO);
#endif
}

void peripheral_reset(void)
{
    /* startup reset thread to do reset work */
    rt_thread_t reset_thrd;

    /* give this thread a high priority, it does nothing but sleep */
    reset_thrd = rt_thread_create("reset_thrd", reset_thrd_entry, RT_NULL, 4096, 10, 10);
    if (reset_thrd != RT_NULL)
        rt_thread_startup(reset_thrd);
}

void first_driver_init(void)
{
#ifdef RT_USING_GPIO
    rt_hw_gpio_init();
#endif

#ifdef RT_USING_I2C
    rt_hw_i2c_init();
#endif

#ifdef FH_USING_CLOCK
    rt_clk_dev_init();
#endif

#ifdef RT_USING_PWM
    rt_hw_pwm_init();
#endif
}

static void sys_driver_init(void *param);
static void fbv_init_thrd(void *args)
{
    _timekeeping_init();

#ifdef FH_FAST_BOOT
    g_fastvideo_init_ok = __fastvideo_init();
#endif

    clock_time_system_init();
    sys_driver_init(RT_NULL);
}

void first_code_run(void)
{
    rt_thread_t tid;

    peripheral_reset(); /* reset, pull down */

    tid = rt_thread_create("fbv_init", fbv_init_thrd, RT_NULL, 4096, 20, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
}

void second_driver_init(void)
{
/* maybe delayed to second_driver_init */
#if defined(FH_USING_DMA) || defined(FH_USING_AXI_DMA) || defined(FH_USING_MOL_DMA)
    fh_dma_init();
#endif

#ifdef RT_USING_SPI
    rt_hw_spi_init();
#endif

#ifdef RT_USING_FAL_SFUD_ADAPT
    fh_fal_sfud_adapt_init();
#endif
#ifdef FH_USING_NAND_FLASH
    fh_nand_flash_init();
#endif

#ifdef RT_USING_RTC
    rt_hw_rtc_init();
#endif
}

void wait_init_done(void)
{
    /* wait reset_thread finish */
    while (rt_thread_find("reset_thrd") != RT_NULL)
    {
        rt_thread_delay(1);
    }
}

extern void rt_cesa_init(void);
extern void rt_efuse_init(void);
static void normal_driver_init(void)
{
    _ircut_init();

#if defined(RT_USING_CALIBRATE)
    calibrate_delay();
#endif

#ifdef RT_USING_WDT
    rt_hw_wdt_init();
#endif

#ifdef FH_USING_MODULE_CASE
    fh_module_case_probe();
#endif

#ifdef FH_USING_SADC
    rt_hw_sadc_init();
#endif

#ifdef FH_USING_ALGORITHM
    rt_algo_init();
#endif
#ifdef FH_USING_AES
    rt_hw_aes_init();
#endif
#ifdef FH_USING_CESA
    rt_cesa_init();
#endif
#ifdef FH_USING_EFUSE
    rt_efuse_init();
#endif
}

void fourth_driver_init(void)
{
#ifndef FH_FAST_BOOT /* now only multi-load support ulog */
#if defined(RT_USING_ULOG) && defined(ULOG_BACKEND_USING_CONSOLE)
    ulog_sys_init(ULOG_BE_TYPE_CONSOLE);
#endif
#endif

#ifdef RT_USING_LWIP
    network_init();
#endif

#ifdef RT_USING_SDIO
    rt_mmcsd_core_init();
    rt_mmcsd_blk_init();
    rt_hw_mmc_init();
#endif
#ifdef RT_USING_SPI_SDIO
    msd_init("spi1_sd0", "ssi1_0");
#endif

#ifdef RT_USING_UHC
    usb_init();
#endif

    /* wait sys driver init done */
    filesystem_init();

#ifdef RT_USING_NEWLIB
    /* init libc */
    libc_system_init();
#endif
    /* init rtc and update system walltime */

#ifndef FH_FAST_BOOT /* now only multi-load support ulog */
#if defined(RT_USING_ULOG) && defined(ULOG_BACKEND_USING_FS)
    /* fs log system must init after filesystem_init*/
    /* otherwise it will lead to mutex assert */
    ulog_sys_init(ULOG_BE_TYPE_FS);
#endif
#endif

#ifdef RT_USING_FINSH /* can be delayed. */
    finsh_system_init();
#if defined(RT_USING_DEVICE) && !defined(RT_USING_POSIX)
    finsh_set_device(RT_CONSOLE_DEVICE_NAME);
#endif
#endif

#if defined(FH_USING_I2S) || defined(FH_USING_MOL_I2S)
    if (fh_i2s_init())
        rt_kprintf("fh_i2s_init failed\n");
#endif

#ifdef RT_USING_SPISLAVE
    rt_hw_spi_slave_init();
#endif

#ifdef FH_USING_FH_PERF
    rt_hw_fhperf_init();
#endif

#ifdef FH_USING_FH_STEPMOTOR
    rt_hw_stepmotor_init();
#endif

#ifdef FH_USING_FH_HASH
    rt_hw_hash_init();
#endif

#ifdef RT_USING_CPLUSPLUS
    extern int cplusplus_system_init(void);
    cplusplus_system_init();
#endif

#ifdef RT_USING_UDC
    extern int udc_usbd_register(void);
    udc_usbd_register();
#endif
}

#ifdef CONFIG_STARTUP_TIMECOST
#define MAX_TIME_LOG (8)
static unsigned int g_time_cost[MAX_TIME_LOG] = {0};
extern void print_os_startup_time(void);   /* in os_startup.c */
extern void print_fastboot_timecost(void); /* in fbv_init.c/fbv_startup.c */
extern void print_loadcode_timecost(void); /* in flash_load.c/cp_load.c */
extern void fbv_time_stat_prinf(char *class_name);

static void do_startup_cost(void *param)
{
    rt_thread_delay(100);
    fbv_time_stat_prinf("fbv_time");
}

static void report_startup_cost(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("timecost", do_startup_cost, RT_NULL, 8192, 250, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
}
#endif

#if defined(FH_ENABLE_VIDEO) && !defined(FH_FAST_BOOT)
extern void fh_media_process_module_init(void);
extern void fh_isp_module_init(void);
extern void fh_vpu_module_init(void);
extern int fh_vise_module_init(void);

#if !defined(CONFIG_ARCH_MC632X)
/* extern int fh_bgm_module_init(void); */
extern void fh_enc_module_init(void);
extern void fh_jpeg_module_init(void);
#else /* MC632X */
extern int venc_module_init(void);
extern int mc_lcm_init(void);
extern int xc00_vou_init(void);
#endif
#define FH_SENSOR_CLK (24 * 1000 * 1000)
static void video_init(void)
{
    struct clk *clk;

    fh_media_process_module_init();
    fh_isp_module_init();
    fh_vise_module_init();

#if !defined(CONFIG_ARCH_MC632X)
    /* fh_bgm_module_init(); */
    fh_vpu_module_init();
    fh_enc_module_init();
    fh_jpeg_module_init();
#else/* MC632X */
    venc_module_init();
    mc_lcm_init();
    xc00_vou_init();
#endif
}
#endif

static void sys_driver_init(void *param)
{
#ifdef FH_FAST_BOOT
    if (g_fastvideo_init_ok == 0)
    {
        __fastvideo_startup();
    }
    else
    {
        rt_kprintf("fastvideo init failed: %d\n", g_fastvideo_init_ok);
    }

    rt_show_version();
#endif
    third_driver_init();

    normal_driver_init();
}

void rt_init_drv_thread_entry(void *parameter)
{
#ifdef CONFIG_STARTUP_TIMECOST
    g_time_cost[0] = read_pts();
#endif

    first_driver_init();
    first_code_run(); /* nothing to do here */

#ifdef CONFIG_STARTUP_TIMECOST
    g_time_cost[1] = read_pts();
#endif
    second_driver_init(); /* init spi and start load */

#ifdef CONFIG_STARTUP_TIMECOST
    g_time_cost[2] = read_pts();
#endif
    user_code_load(); /* just wait load done */

#ifdef CONFIG_STARTUP_TIMECOST
    g_time_cost[3] = read_pts();
#endif
    fourth_driver_init();

#ifdef CONFIG_STARTUP_TIMECOST
    g_time_cost[4] = read_pts();
#endif
    wait_init_done(); /* wait all init work done */

#ifdef CONFIG_STARTUP_TIMECOST
    g_time_cost[5] = read_pts();
#endif
#if defined(FH_ENABLE_VIDEO) && !defined(FH_FAST_BOOT)
    video_init();
#endif

#ifdef RUN_SMP_TEST_THREAD
    extern void test_smp_thread(int cid);
    {
        int i;
        for (i = 0; i < RT_CPUS_NR; i++)
        {
            test_smp_thread(i);
            rt_thread_delay(1);
        }
    }
#endif
    user_main(); /* start user application */

#ifdef CONFIG_STARTUP_TIMECOST
    report_startup_cost();
#endif

    while (1)
    {
        rt_thread_delay(10000);
    }
}

void rt_application_init(void)
{
    rt_thread_t init_drv_thread;

    /*
     * USER_INIT_THREAD_STACK_SIZE configed in rtconfig.h
     */
    init_drv_thread = rt_thread_create("init_drv", rt_init_drv_thread_entry,
                                       RT_NULL, USER_INIT_THREAD_STACK_SIZE, 80, 5);

#ifdef RT_ENABLE_RUNNING_LOG
    runlog_init(0x10000);
#endif
    if (init_drv_thread != RT_NULL)
        rt_thread_startup(init_drv_thread);
}
