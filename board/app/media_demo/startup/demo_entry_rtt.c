#include <optparse.h>
#include "sample_common.h"
#include "vlcview/vlcview.h"

void user_main(void)
{
#if !defined(FH_FAST_BOOT) && !defined(RPC_MEDIA)
    extern FH_SINT32 fh_nna_module_init(FH_VOID);
    int ret;
    ret = fh_nna_module_init();
    if (ret)
    {
        printf("fh_nna_module_init failed %x !\n", ret);
    }
#endif
}

FH_VOID vlcview_exit(FH_VOID)
{
#ifdef UVC_ENABLE
    printf("UVC not support exit!\n");
    return;
#endif
    _vlcview_exit();

    buffer_reset_vmm(); /* decomment it after lib reupdate  */
}

FH_VOID vlcview_usage(FH_VOID)
{
    printf("\nusage:  vlcview  [-i: | p: | -q | -h]\n\n");
    printf("        -i ip    :ip address (only for pes)\n");
    printf("        -p port  :port (only for pes and rtsp)\n");
    printf("        -q       :quit\n");
    printf("        -h       :help\n\n");
    printf("example: \n");
    printf("        vlcview -i 10.81.187.8 -p 1234\n");
    printf("        vlcview -q\n");
}

FH_VOID vlcview(int argc, char *argv[])
{
    struct optparse options;
    FH_CHAR *dst_ip = NULL;
    unsigned int port = 1234;
    int cmd;
    int get_cmd = 0;

    optparse_init(&options, argv);
    while ((cmd = optparse(&options, "i:p:qh")) != -1)
    {
        get_cmd = 1;
        switch (cmd)
        {
        case 'i':
        {
            dst_ip = options.optarg;
            break;
        }
        case 'p':
        {
            port = strtol(options.optarg, NULL, 0);
            break;
        }
        case 'q':
        {
            vlcview_exit();
            return;
        }
        default:
        {
            vlcview_usage();
            return;
        }
        }
    }

    if (!get_cmd)
    {
#ifndef UVC_ENABLE
        vlcview_usage();
        return;
#endif
    }

    _vlcview(dst_ip, port);
}

#include <rttshell.h>
SHELL_CMD_EXPORT(vlcview, vlcview());
#ifdef FH_APP_OPEN_AUDIO
SHELL_CMD_EXPORT(sample_audio, sample_audio());
#endif
