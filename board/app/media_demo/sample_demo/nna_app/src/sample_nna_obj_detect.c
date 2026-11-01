#include <rtthread.h>
#include "nna_app/include/sample_common_nna.h"
#include "overlay/include/sample_common_overlay.h"

static char *model_name = MODEL_TAG_DEMO_NNA_OBJ_DET;

static FH_UINT32 g_nna_grpid[MAX_GRP_NUM] = {0};
static FH_SINT32 g_nna_running[MAX_GRP_NUM] = {0};
static FH_SINT32 g_nna_stop[MAX_GRP_NUM] = {0};

/* qr_gate 防尾随: 每帧把人形框交给门禁 (qr_gate/tg_gate.c 有强定义; 没编进 qr_gate 时为空) */
__attribute__((weak)) void qr_gate_nn_report(int grp_id, int n, const float *cx, const float *cy, const float *conf)
{
    (void)grp_id; (void)n; (void)cx; (void)cy; (void)conf;
}

static FH_VOID report_to_gate(FH_SINT32 grp_id, const NN_RESULT *r)
{
    static float cx[MAX_GRP_NUM][NN_RESULT_NUM], cy[MAX_GRP_NUM][NN_RESULT_NUM], cf[MAX_GRP_NUM][NN_RESULT_NUM];
    FH_SINT32 i, n = r->result_det.nn_result_num;

    if (n < 0)
        n = 0;
    if (n > NN_RESULT_NUM)
        n = NN_RESULT_NUM;
    for (i = 0; i < n; i++)
    {
        const NN_RESULT_DET_INFO *d = &r->result_det.nn_result_info[i];
        cx[grp_id][i] = d->box.x + d->box.w * 0.5f;
        cy[grp_id][i] = d->box.y + d->box.h * 0.5f;
        cf[grp_id][i] = d->conf;
    }
    qr_gate_nn_report(grp_id, n, cx[grp_id], cy[grp_id], cf[grp_id]);
}

static FH_SINT32 draw_box(FH_SINT32 grp_id, FH_SINT32 chn_id, NN_RESULT nn_result, FH_FLOAT conf)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    FH_UINT32 chn_w = 0;
    FH_UINT32 chn_h = 0;
    /* 原来是全局一份; 两路检测线程同时画框会互相踩, 改成每个 group 一份 */
    static OSD_GBOX gbox_g[MAX_GRP_NUM][BOX_MAX_NUM];
    OSD_GBOX *gbox = gbox_g[grp_id];
    FH_SINT32 i = 0;
    static NN_RESULT last_result_g[MAX_GRP_NUM];
#define last_result (last_result_g[grp_id])

    if (get_vpu_chn_w(grp_id, chn_id) == 0) /* 该组无 OSD 画布通道(如降内存关掉 CH0)时跳过画框 */
        return FH_SDK_SUCCESS;

    if (nn_result.result_det.nn_result_num == 0)
    {
        for (i = 0; i < last_result.result_det.nn_result_num; i++)
        {
            gbox[i].enable = 0;

            ret = sample_set_gbox(grp_id, chn_id, &gbox[i]);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }

        last_result.result_det.nn_result_num = 0;

        return FH_SDK_SUCCESS;
    }

    SDK_CHECK_MAX_VALID(model_name, nn_result.result_det.nn_result_num, BOX_MAX_NUM, "nn_result_num > BOX_MAX_NUM:%d\n", BOX_MAX_NUM);

    chn_w = get_vpu_chn_w(grp_id, chn_id);
    chn_h = get_vpu_chn_h(grp_id, chn_id);

    for (i = 0; i < nn_result.result_det.nn_result_num; i++)
    {
        gbox[i].gbox_id = i;
        gbox[i].color.fAlpha = 255;
        gbox[i].color.fRed = 255;
        gbox[i].color.fGreen = 0;
        gbox[i].color.fBlue = 0;
        gbox[i].x = nn_result.result_det.nn_result_info[i].box.x * chn_w;
        gbox[i].y = nn_result.result_det.nn_result_info[i].box.y * chn_h;
        gbox[i].w = nn_result.result_det.nn_result_info[i].box.w * chn_w;
        gbox[i].h = nn_result.result_det.nn_result_info[i].box.h * chn_h;

        if (nn_result.result_det.nn_result_info[i].conf < conf)
        {
            gbox[i].enable = 0;
        }
        else
        {
            gbox[i].enable = 1;
        }

        ret = sample_set_gbox(grp_id, chn_id, &gbox[i]);
        SDK_FUNC_ERROR_CONTINUE(model_name, ret);
    }

    if (last_result.result_det.nn_result_num > nn_result.result_det.nn_result_num)
    {
        for (; i < last_result.result_det.nn_result_num; i++)
        {
            gbox[i].enable = 0;

            ret = sample_set_gbox(grp_id, chn_id, &gbox[i]);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }
    }

    last_result.result_det.nn_result_num = nn_result.result_det.nn_result_num;

    return ret;
#undef last_result
}

FH_VOID *nna_obj_detect(FH_VOID *args)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    NN_HANDLE nn_handle = NULL;
    NN_TYPE nn_type;
    FH_CHAR model_path[256];
    VPU_INFO *vpu_info;
    VPU_CHN_INFO *vpu_chn_info;
    NN_INPUT_DATA nn_input_data;
    NN_RESULT nn_result;
    FH_SINT32 grp_id = *((FH_SINT32 *)args);
    FH_CHAR thread_name[20];
    FH_VIDEO_FRAME pstVpuframeinfo;
    FH_UINT32 handle_lock;
    FH_FLOAT fps = 0;
    FH_UINT64 time_start = 0;
    FH_UINT64 time_now = 0;
    FH_UINT64 time_bak = 0;
    FH_UINT64 time_lapse = 1000 / FH_NN_DET_FPS;
    INI_APP_CFG *ini_app_cfg = NULL;

    vpu_info = get_vpu_config(grp_id);

    if (vpu_info->enable)
    {
        vpu_chn_info = get_vpu_chn_config(grp_id, NN_CHN);
        if (!vpu_chn_info->enable)
        {
            SDK_ERR_PRT(model_name, "VPU[%d] CHN[%d] not enable !\n", grp_id, NN_CHN);
            goto Exit;
        }
    }
    else
    {
        SDK_ERR_PRT(model_name, "VPU GRP[%d] not enable !\n", grp_id);
        goto Exit;
    }

    ini_app_cfg = get_ini_app_cfg();
    if (ini_app_cfg)
    {
        if (!ini_app_cfg->nn_enable)
        {
            goto Exit;
        }
        time_lapse = 1000 / ini_app_cfg->nn_fps;
    }

#ifdef FH_NN_PERSON_DETECT_ENABLE
    nn_type = PERSON_DET;
    strcpy(model_path, "./models/persondet.nbg");
#elif defined(FH_NN_C2D_ENABLE)
    nn_type = PERSON_VEHICLE_DET;
    strcpy(model_path, "./models/c2det.nbg");
#elif defined(FH_NN_FACE_DET_ENABLE)
    nn_type = FACE_DET;
    strcpy(model_path, "./models/facedet.nbg");
#elif defined(FH_NN_PVF_DET_ENABLE)
    nn_type = PERSON_VEHICLE_FIRE_DET;
    strcpy(model_path, "./models/c3det.nbg");
#elif defined(FH_APP_OPEN_LPD_ENABLE)
    nn_type = LPR_DET;
    strcpy(model_path, "./models/lpdet.nbg");
#else
    SDK_ERR_PRT(model_name, "nn_type NULL !\n");
    goto Exit;
#endif

    ret = nna_init(grp_id, &nn_handle, nn_type, model_path, vpu_chn_info->width, vpu_chn_info->height);
    SDK_FUNC_ERROR_GOTO(model_name, ret);

    memset(&nn_input_data, 0, sizeof(nn_input_data));
    nn_input_data.width = vpu_chn_info->width;
    nn_input_data.height = vpu_chn_info->height;
    nn_input_data.stride = vpu_chn_info->width;

    SDK_PRT(model_name, "nna_obj_detect[%d] Start!\n", grp_id);

    /* 双路各自初始化 OSD 画布: demo 只给 FH_APP_GRP_ID(G0) 初始化过,
     * G1 无画布时 draw_box 会被 SDK 拒绝(0xa0c84002), 画框就不显示 */
    sample_fh_overlay_start(grp_id);

    g_nna_running[grp_id] = 1;

    memset(thread_name, 0, sizeof(thread_name));
    sprintf(thread_name, "nna_obj_det_%d", grp_id);
    prctl(PR_SET_NAME, thread_name);
    sleep(1);
    time_start = time_bak = time_now = get_ms();
    while (!g_nna_stop[grp_id])
    {
        ret = lock_vpu_stream(grp_id, NN_CHN, &pstVpuframeinfo, 500, &handle_lock, 1);
        if (ret != 0)
        {
            /* 单路降级/管线未起时 grp 会持续 lock 失败: 在失败分支放慢重试,
             * 否则两路 NN 线程紧密空转 lock_vpu_stream 拖死系统 */
            rt_thread_mdelay(3000);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }
        /* lock 成功: 帧已到手, 立即处理(此前 3 秒等待错放在成功路径, 会长时间占住帧) */

        time_now = get_ms();
        if (time_now - time_bak > time_lapse)
        {
            time_bak += time_lapse;

            ret = getVPUStreamToNN(pstVpuframeinfo, &nn_input_data);
            if (!ret)
            {
                ret = nna_get_obj_det_result(nn_handle, nn_input_data, &nn_result);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);
                fps++;

                ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);

                report_to_gate(grp_id, &nn_result);

                ret = draw_box(grp_id, 0, nn_result, 0.9f);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);

                if (time_now - time_start > 10 * 1000)
                {
                    SDK_PRT(model_name, "NN[%d] Det FPS: %f\n", grp_id, fps / 10);
                    fps = 0;
                    time_start = time_now;
                }
            }
            else
            {
                ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
                SDK_FUNC_ERROR_CONTINUE(model_name, ret);
            }
        }
        else
        {
            ret = unlock_vpu_stream(grp_id, NN_CHN, handle_lock, &pstVpuframeinfo);
            SDK_FUNC_ERROR_CONTINUE(model_name, ret);
        }
    }

Exit:
    ret = nna_release(grp_id, &nn_handle);
    SDK_FUNC_ERROR_GOTO(model_name, ret);
    SDK_PRT(model_name, "nna_obj_detect[%d] End!\n", grp_id);

    g_nna_running[grp_id] = 0;

    return NULL;
}

FH_SINT32 sample_fh_nna_obj_detect_start(FH_SINT32 grp_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;
    pthread_t thread;
    pthread_attr_t attr;

    if (g_nna_running[grp_id])
    {
        SDK_PRT(model_name, "nna_obj_detect[%d] already running!\n", grp_id);
        return FH_SDK_SUCCESS;
    }

    g_nna_grpid[grp_id] = grp_id;
    g_nna_stop[grp_id] = 0;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef __RTTHREAD_OS__
    pthread_attr_setstacksize(&attr, 256 * 1024);
#endif
    if (pthread_create(&thread, &attr, nna_obj_detect, &g_nna_grpid[grp_id]))
    {
        SDK_ERR_PRT(model_name, "pthread_create failed\n");
        goto Exit;
    }

    return ret;

Exit:
    return -1;
}

FH_SINT32 sample_fh_nna_obj_detect_stop(FH_SINT32 grp_id)
{
    FH_SINT32 ret = FH_SDK_SUCCESS;

    if (g_nna_running[grp_id])
    {
        g_nna_stop[grp_id] = 1;

        while (g_nna_running[grp_id])
        {
            usleep(10);
        }
    }
    else
    {
        SDK_PRT(model_name, "nna_obj_detect[%d] not running!\n", grp_id);
    }

    return ret;
}
