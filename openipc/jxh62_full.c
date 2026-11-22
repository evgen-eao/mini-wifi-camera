#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "type.h"
#include "common.h"
#include "comm_video.h"
#include "comm_isp.h"
#include "comm_sns.h"
#include "sns_ctrl.h"
#include "gk_api_isp.h"
#include "gk_api_ae.h"
#include "gk_api_awb.h"

extern int ioctl(int fd, unsigned long request, ...);

extern GK_S32 HI_MPI_ISP_SensorRegCallBack(VI_PIPE ViPipe, ISP_SNS_ATTR_INFO_S *pstSnsAttrInfo, ISP_SENSOR_REGISTER_S *pstRegister);
extern GK_S32 HI_MPI_AE_SensorRegCallBack(VI_PIPE ViPipe, ALG_LIB_S *pstAeLib, ISP_SNS_ATTR_INFO_S *pstSnsAttrInfo, AE_SENSOR_REGISTER_S *pstRegister);
extern GK_S32 HI_MPI_AWB_SensorRegCallBack(VI_PIPE ViPipe, ALG_LIB_S *pstAwbLib, ISP_SNS_ATTR_INFO_S *pstSnsAttrInfo, AWB_SENSOR_REGISTER_S *pstRegister);
extern GK_S32 HI_MPI_ISP_SensorUnRegCallBack(VI_PIPE ViPipe, SENSOR_ID SensorId);
extern GK_S32 HI_MPI_AE_SensorUnRegCallBack(VI_PIPE ViPipe, ALG_LIB_S *pstAeLib, SENSOR_ID SensorId);
extern GK_S32 HI_MPI_AWB_SensorUnRegCallBack(VI_PIPE ViPipe, ALG_LIB_S *pstAwbLib, SENSOR_ID SensorId);

#ifndef I2C_SLAVE_FORCE
#define I2C_SLAVE_FORCE 0x0706
#endif

#define JXH62_ID                    62
#define JXH62_I2C_ADDR              0x60
#define JXH62_ADDR_BYTE             1
#define JXH62_DATA_BYTE             1
#define JXH62_FULL_LINES_MAX        0xFFFF
#define JXH62_FULL_LINES_STD        750     /* 0x02EE */
#define JXH62_INCREASE_LINES        4
#define JXH62_VMAX_720P30_LINEAR    (JXH62_FULL_LINES_STD + JXH62_INCREASE_LINES)

static int g_fd[ISP_MAX_PIPE_NUM] = {[0 ... (ISP_MAX_PIPE_NUM - 1)] = -1};
static ISP_SNS_STATE_S *g_pastJxh62[ISP_MAX_PIPE_NUM] = {GK_NULL};
static ISP_SNS_COMMBUS_U g_aunJxh62BusInfo[ISP_MAX_PIPE_NUM] = {
    [0] = { .s8I2cDev = 0 },
    [1 ... ISP_MAX_PIPE_NUM - 1] = { .s8I2cDev = -1 }
};
static GK_U32 g_au32InitExposure[ISP_MAX_PIPE_NUM] = {0};
static GK_U32 g_au32LinesPer500ms[ISP_MAX_PIPE_NUM] = {0};
static GK_U16 g_au16InitWBGain[ISP_MAX_PIPE_NUM][3] = {{0}};
static GK_U16 g_au16SampleRgain[ISP_MAX_PIPE_NUM] = {0};
static GK_U16 g_au16SampleBgain[ISP_MAX_PIPE_NUM] = {0};

int jxh62_i2c_init(VI_PIPE ViPipe)
{
    char acDevFile[16] = {0};
    GK_U8 u8DevNum;
    if (g_fd[ViPipe] >= 0) {
        return GK_SUCCESS;
    }
    u8DevNum = g_aunJxh62BusInfo[ViPipe].s8I2cDev;
    snprintf(acDevFile, sizeof(acDevFile), "/dev/i2c-%u", u8DevNum);
    g_fd[ViPipe] = open(acDevFile, O_RDWR, S_IRUSR | S_IWUSR);
    if (g_fd[ViPipe] < 0) {
        printf("Open /dev/i2c-%u error!\n", u8DevNum);
        return GK_FAILURE;
    }
    if (ioctl(g_fd[ViPipe], I2C_SLAVE_FORCE, (JXH62_I2C_ADDR >> 1)) < 0) {
        printf("I2C_SLAVE_FORCE error!\n");
        close(g_fd[ViPipe]);
        g_fd[ViPipe] = -1;
        return GK_FAILURE;
    }
    return GK_SUCCESS;
}

int jxh62_i2c_exit(VI_PIPE ViPipe)
{
    if (g_fd[ViPipe] >= 0) {
        close(g_fd[ViPipe]);
        g_fd[ViPipe] = -1;
        return GK_SUCCESS;
    }
    return GK_FAILURE;
}

int jxh62_read_register(VI_PIPE ViPipe, int addr)
{
    return 0;
}

int jxh62_write_register(VI_PIPE ViPipe, int addr, int data)
{
    if (g_fd[ViPipe] < 0) {
        return GK_SUCCESS;
    }
    char buf[2];
    buf[0] = addr & 0xFF;
    buf[1] = data & 0xFF;
    if (write(g_fd[ViPipe], buf, 2) < 0) {
        printf("I2C_WRITE error!\n");
        return GK_FAILURE;
    }
    return GK_SUCCESS;
}

void jxh62_standby(VI_PIPE ViPipe) {}
void jxh62_restart(VI_PIPE ViPipe) {}

void jxh62_linear_720p30_init(VI_PIPE ViPipe)
{
    usleep(100000);
    jxh62_write_register(ViPipe, 0x12, 0x40);
    jxh62_write_register(ViPipe, 0x0E, 0x11);
    jxh62_write_register(ViPipe, 0x0F, 0x08);
    jxh62_write_register(ViPipe, 0x10, 0x20);
    jxh62_write_register(ViPipe, 0x11, 0x80);
    jxh62_write_register(ViPipe, 0x19, 0x68);
    jxh62_write_register(ViPipe, 0x20, 0x60);
    jxh62_write_register(ViPipe, 0x21, 0x09);
    jxh62_write_register(ViPipe, 0x22, 0xEE);
    jxh62_write_register(ViPipe, 0x23, 0x02);
    jxh62_write_register(ViPipe, 0x24, 0x00);
    jxh62_write_register(ViPipe, 0x25, 0xD0);
    jxh62_write_register(ViPipe, 0x26, 0x25);
    jxh62_write_register(ViPipe, 0x27, 0xEE);
    jxh62_write_register(ViPipe, 0x28, 0x15);
    jxh62_write_register(ViPipe, 0x29, 0x02);
    jxh62_write_register(ViPipe, 0x2A, 0x70);
    jxh62_write_register(ViPipe, 0x2B, 0x21);
    jxh62_write_register(ViPipe, 0x2C, 0x08);
    jxh62_write_register(ViPipe, 0x2D, 0x01);
    jxh62_write_register(ViPipe, 0x2E, 0xBB);
    jxh62_write_register(ViPipe, 0x2F, 0xC0);
    jxh62_write_register(ViPipe, 0x41, 0x88);
    jxh62_write_register(ViPipe, 0x42, 0x12);
    jxh62_write_register(ViPipe, 0x39, 0x90);
    jxh62_write_register(ViPipe, 0x1D, 0xFF);
    jxh62_write_register(ViPipe, 0x1E, 0x9F);
    jxh62_write_register(ViPipe, 0x7A, 0x80);
    jxh62_write_register(ViPipe, 0x31, 0x0C);
    jxh62_write_register(ViPipe, 0x33, 0x4C);
    jxh62_write_register(ViPipe, 0x34, 0x2F);
    jxh62_write_register(ViPipe, 0x35, 0xA3);
    jxh62_write_register(ViPipe, 0x36, 0x05);
    jxh62_write_register(ViPipe, 0x38, 0x53);
    jxh62_write_register(ViPipe, 0x3A, 0x08);
    jxh62_write_register(ViPipe, 0x56, 0x02);
    jxh62_write_register(ViPipe, 0x60, 0x02);
    jxh62_write_register(ViPipe, 0x0D, 0x50);
    jxh62_write_register(ViPipe, 0x57, 0x80);
    jxh62_write_register(ViPipe, 0x58, 0x33);
    jxh62_write_register(ViPipe, 0x5A, 0x04);
    jxh62_write_register(ViPipe, 0x5B, 0xB6);
    jxh62_write_register(ViPipe, 0x5C, 0x08);
    jxh62_write_register(ViPipe, 0x5D, 0x67);
    jxh62_write_register(ViPipe, 0x5E, 0x04);
    jxh62_write_register(ViPipe, 0x5F, 0x08);
    jxh62_write_register(ViPipe, 0x66, 0x28);
    jxh62_write_register(ViPipe, 0x67, 0xF8);
    jxh62_write_register(ViPipe, 0x68, 0x04);
    jxh62_write_register(ViPipe, 0x69, 0x74);
    jxh62_write_register(ViPipe, 0x6A, 0x3F);
    jxh62_write_register(ViPipe, 0x63, 0x82);
    jxh62_write_register(ViPipe, 0x6C, 0xC0);
    jxh62_write_register(ViPipe, 0x6E, 0x5C);
    jxh62_write_register(ViPipe, 0x82, 0x01);
    jxh62_write_register(ViPipe, 0x0C, 0x00);
    jxh62_write_register(ViPipe, 0x46, 0xC2);
    jxh62_write_register(ViPipe, 0x48, 0x7E);
    jxh62_write_register(ViPipe, 0x62, 0x40);
    jxh62_write_register(ViPipe, 0x7D, 0x57);
    jxh62_write_register(ViPipe, 0x7E, 0x28);
    jxh62_write_register(ViPipe, 0x80, 0x00);
    jxh62_write_register(ViPipe, 0x4A, 0x05);
    jxh62_write_register(ViPipe, 0x49, 0x10);
    jxh62_write_register(ViPipe, 0x13, 0x81);
    jxh62_write_register(ViPipe, 0x59, 0x97);
    jxh62_write_register(ViPipe, 0x12, 0x00);
    jxh62_write_register(ViPipe, 0x47, 0x47);
    usleep(500000);
    jxh62_write_register(ViPipe, 0x47, 0x44);
    jxh62_write_register(ViPipe, 0x1F, 0x01);
    printf("h62 init\n");
}

void jxh62_init(VI_PIPE ViPipe)
{
    jxh62_i2c_init(ViPipe);
    jxh62_linear_720p30_init(ViPipe);
    if (g_pastJxh62[ViPipe] != GK_NULL) {
        g_pastJxh62[ViPipe]->bInit = GK_TRUE;
    }
}

void jxh62_exit(VI_PIPE ViPipe)
{
    jxh62_i2c_exit(ViPipe);
}

static GK_S32 cmos_get_ae_default(VI_PIPE ViPipe, AE_SENSOR_DEFAULT_S *pstAeSnsDft)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    if (pstAeSnsDft == GK_NULL || pstSnsState == GK_NULL) {
        return GK_FAILURE;
    }
    memset(&pstAeSnsDft->stAERouteAttr, 0, sizeof(ISP_AE_ROUTE_S));
    pstAeSnsDft->u32FullLinesStd = pstSnsState->u32FLStd;
    pstAeSnsDft->u32FlickerFreq = 50 * 256;
    pstAeSnsDft->u32FullLinesMax = JXH62_FULL_LINES_MAX;

    pstAeSnsDft->stIntTimeAccu.enAccuType = AE_ACCURACY_LINEAR;
    pstAeSnsDft->stIntTimeAccu.f32Accuracy = 1;
    pstAeSnsDft->stIntTimeAccu.f32Offset = 0;

    pstAeSnsDft->stAgainAccu.enAccuType = AE_ACCURACY_TABLE;
    pstAeSnsDft->stAgainAccu.f32Accuracy = 1;

    pstAeSnsDft->stDgainAccu.enAccuType = AE_ACCURACY_TABLE;
    pstAeSnsDft->stDgainAccu.f32Accuracy = 1;

    pstAeSnsDft->u32ISPDgainShift = 8;
    pstAeSnsDft->u32MinISPDgainTarget = 1 << pstAeSnsDft->u32ISPDgainShift;
    pstAeSnsDft->u32MaxISPDgainTarget = 8 << pstAeSnsDft->u32ISPDgainShift;
    pstAeSnsDft->enMaxIrisFNO = ISP_IRIS_F_NO_1_0;
    pstAeSnsDft->enMinIrisFNO = ISP_IRIS_F_NO_32_0;
    pstAeSnsDft->bAERouteExValid = GK_FALSE;
    pstAeSnsDft->stAERouteAttr.u32TotalNum = 0;
    pstAeSnsDft->stAERouteAttrEx.u32TotalNum = 0;

    pstAeSnsDft->u32InitExposure = (g_au32InitExposure[ViPipe] == 0) ? 76151 : g_au32InitExposure[ViPipe];
    pstAeSnsDft->u32LinesPer500ms = (g_au32LinesPer500ms[ViPipe] == 0) ? (((GK_U64)pstSnsState->u32FLStd * 30) >> 1) : g_au32LinesPer500ms[ViPipe];

    pstAeSnsDft->au8HistThresh[0] = 0x0D;
    pstAeSnsDft->au8HistThresh[1] = 0x28;
    pstAeSnsDft->au8HistThresh[2] = 0x60;
    pstAeSnsDft->au8HistThresh[3] = 0x80;

    pstAeSnsDft->u8AeCompensation = 70;
    pstAeSnsDft->u8AERunInterval = 1;
    pstAeSnsDft->enAeExpMode = AE_EXP_LOWLIGHT_PRIOR;

    pstAeSnsDft->u32MaxIntTime = pstSnsState->u32FLStd - 4;
    pstAeSnsDft->u32MinIntTime = 2;
    pstAeSnsDft->u32MaxIntTimeTarget = 65535;
    pstAeSnsDft->u32MinIntTimeTarget = 2;

    pstAeSnsDft->u32MaxAgain = 22924;
    pstAeSnsDft->u32MinAgain = 1024;
    pstAeSnsDft->u32MaxAgainTarget = pstAeSnsDft->u32MaxAgain;
    pstAeSnsDft->u32MinAgainTarget = pstAeSnsDft->u32MinAgain;

    pstAeSnsDft->u32MaxDgain = 1024;
    pstAeSnsDft->u32MinDgain = 1024;
    pstAeSnsDft->u32MaxDgainTarget = pstAeSnsDft->u32MaxDgain;
    pstAeSnsDft->u32MinDgainTarget = pstAeSnsDft->u32MinDgain;

    return GK_SUCCESS;
}

static GK_VOID cmos_fps_set(VI_PIPE ViPipe, GK_FLOAT f32Fps, AE_SENSOR_DEFAULT_S *pstAeSnsDft)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    GK_U32 u32VMAX = JXH62_VMAX_720P30_LINEAR;
    if (f32Fps <= 30.0f && f32Fps >= 0.5f) {
        u32VMAX = (JXH62_VMAX_720P30_LINEAR * 30) / f32Fps;
    } else {
        return;
    }
    u32VMAX = (u32VMAX > JXH62_FULL_LINES_MAX) ? JXH62_FULL_LINES_MAX : u32VMAX;
    pstSnsState->astRegsInfo[0].astI2cData[2].u32Data = (u32VMAX & 0xFF);
    pstSnsState->astRegsInfo[0].astI2cData[3].u32Data = ((u32VMAX & 0xFF00) >> 8);
    pstSnsState->u32FLStd = u32VMAX;
    pstAeSnsDft->f32Fps = f32Fps;
    pstAeSnsDft->u32LinesPer500ms = (pstSnsState->u32FLStd * f32Fps) / 2;
    pstAeSnsDft->u32FullLinesStd = pstSnsState->u32FLStd;
    pstAeSnsDft->u32MaxIntTime = pstSnsState->u32FLStd - 4;
    pstSnsState->au32FL[0] = pstSnsState->u32FLStd;
    pstAeSnsDft->u32FullLines = pstSnsState->au32FL[0];
}

static GK_VOID cmos_slow_framerate_set(VI_PIPE ViPipe, GK_U32 u32FullLines, AE_SENSOR_DEFAULT_S *pstAeSnsDft)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    u32FullLines = (u32FullLines > JXH62_FULL_LINES_MAX) ? JXH62_FULL_LINES_MAX : u32FullLines;
    pstSnsState->au32FL[0] = u32FullLines;
    pstSnsState->astRegsInfo[0].astI2cData[2].u32Data = (u32FullLines & 0xFF);
    pstSnsState->astRegsInfo[0].astI2cData[3].u32Data = ((u32FullLines & 0xFF00) >> 8);
    pstAeSnsDft->u32FullLines = pstSnsState->au32FL[0];
    pstAeSnsDft->u32MaxIntTime = pstSnsState->au32FL[0] - 4;
}

static GK_VOID cmos_inttime_update(VI_PIPE ViPipe, GK_U32 u32IntTime)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    pstSnsState->astRegsInfo[0].astI2cData[0].u32Data = (u32IntTime & 0xFF);
    pstSnsState->astRegsInfo[0].astI2cData[1].u32Data = ((u32IntTime & 0xFF00) >> 8);
}

static GK_VOID cmos_again_calc_table(VI_PIPE ViPipe, GK_U32 *pu32AgainLin, GK_U32 *pu32AgainDb)
{
    if (pu32AgainLin == GK_NULL || pu32AgainDb == GK_NULL) {
        return;
    }
    GK_U32 again = *pu32AgainLin;
    if (again > 15872) {
        again = 15872;
    }
    GK_U32 shift = 0;
    while (again > 2047 && shift < 3) {
        again >>= 1;
        shift++;
    }
    GK_U32 fine = (again - 1024) >> 6;
    if (fine > 15) {
        fine = 15;
    }
    *pu32AgainDb = (shift << 4) | fine;
    *pu32AgainLin = ((1024 + (fine << 6)) << shift);
}

static GK_VOID cmos_dgain_calc_table(VI_PIPE ViPipe, GK_U32 *pu32DgainLin, GK_U32 *pu32DgainDb)
{
    if (pu32DgainLin == GK_NULL || pu32DgainDb == GK_NULL) {
        return;
    }
    *pu32DgainLin = 1024;
    *pu32DgainDb = 0;
}

static GK_VOID cmos_gains_update(VI_PIPE ViPipe, GK_U32 u32Again, GK_U32 u32Dgain)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    pstSnsState->astRegsInfo[0].astI2cData[4].u32Data = (u32Again & 0xFF);
}

static const AWB_CCM_S g_stAwbCcm = {
    .u16CCMTabNum = 4,
    .astCCMTab = {
        {6150, {728, 33279, 36,    32881, 545, 32944, 32796, 33063, 579}},
        {4850, {656, 33187, 19,    32901, 546, 32925, 32812, 33125, 657}},
        {3650, {641, 33164, 11,    32914, 511, 32877, 32846, 33204, 770}},
        {2650, {566, 33041, 32805, 32901, 515, 32894, 32988, 33564, 1272}}
    }
};

static const AWB_AGC_TABLE_S g_stAwbAgcTable = {
    .bValid = GK_TRUE,
    .au8Saturation = {105, 105, 105, 100, 80, 60, 50, 50, 40, 40, 40, 40, 40, 40, 40, 40}
};

static GK_S32 cmos_get_awb_default(VI_PIPE ViPipe, AWB_SENSOR_DEFAULT_S *pstAwbSnsDft)
{
    if (pstAwbSnsDft == GK_NULL) {
        return GK_FAILURE;
    }
    memset(pstAwbSnsDft, 0, sizeof(AWB_SENSOR_DEFAULT_S));

    pstAwbSnsDft->u16WbRefTemp = 4950;
    pstAwbSnsDft->au16GainOffset[0] = 350;
    pstAwbSnsDft->au16GainOffset[1] = 256;
    pstAwbSnsDft->au16GainOffset[2] = 256;
    pstAwbSnsDft->au16GainOffset[3] = 405;

    pstAwbSnsDft->as32WbPara[0] = 207;
    pstAwbSnsDft->as32WbPara[1] = -141;
    pstAwbSnsDft->as32WbPara[2] = -190;
    pstAwbSnsDft->as32WbPara[3] = 186726;
    pstAwbSnsDft->as32WbPara[4] = 128;
    pstAwbSnsDft->as32WbPara[5] = -136275;

    memcpy(&pstAwbSnsDft->stCcm, &g_stAwbCcm, sizeof(AWB_CCM_S));
    memcpy(&pstAwbSnsDft->stAgcTbl, &g_stAwbAgcTable, sizeof(AWB_AGC_TABLE_S));

    return GK_SUCCESS;
}

static const ISP_CMOS_DEMOSAIC_S g_stIspDemosaic = {
    .bEnable = GK_TRUE,
    .au8NonDirStr = {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64},
    .au8NonDirMFDetailEhcStr = {32, 24, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16},
    .au8NonDirHFDetailEhcStr = {0, 0, 3, 5, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7},
    .au8DetailSmoothRange = {2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 7}
};

static const ISP_CMOS_DRC_S g_stIspDrc = {
    .bEnable = GK_TRUE,
    .u16ManualStrength = 512,
    .u16AutoStrength = 512,
    .u8SpatialFltCoef = 4,
    .u8RangeFltCoef = 8,
    .u8ContrastControl = 8,
    .s8DetailAdjustFactor = -8,
    .u8FltScaleFine = 8,
    .u8FltScaleCoarse = 3,
    .u8GradRevMax = 2,
    .u8GradRevThr = 10,
    .u8Asymmetry = 2,
    .u8SecondPole = 170,
    .u8Stretch = 50,
    .u8Compress = 100,
    .u8CurveSel = 0
};

static GK_S32 cmos_get_isp_default(VI_PIPE ViPipe, ISP_CMOS_DEFAULT_S *pstDef)
{
    if (pstDef == GK_NULL) {
        return GK_FAILURE;
    }
    memset(pstDef, 0, sizeof(ISP_CMOS_DEFAULT_S));
    return GK_SUCCESS;
}

static GK_S32 cmos_get_isp_black_level(VI_PIPE ViPipe, ISP_CMOS_BLACK_LEVEL_S *pstBlackLevel)
{
    if (pstBlackLevel == GK_NULL) {
        return GK_FAILURE;
    }
    pstBlackLevel->bUpdate = GK_FALSE;
    pstBlackLevel->au16BlackLevel[0] = 68;
    pstBlackLevel->au16BlackLevel[1] = 68;
    pstBlackLevel->au16BlackLevel[2] = 68;
    pstBlackLevel->au16BlackLevel[3] = 68;
    return GK_SUCCESS;
}

static GK_VOID cmos_set_pixel_detect(VI_PIPE ViPipe, GK_BOOL bEnable)
{
    if (bEnable) {
        jxh62_write_register(ViPipe, 0x23, 0x05);
        jxh62_write_register(ViPipe, 0x22, 0xEE);
        jxh62_write_register(ViPipe, 0x02, 0x05);
        jxh62_write_register(ViPipe, 0x01, 0xEA);
        jxh62_write_register(ViPipe, 0x00, 0x00);
    } else {
        jxh62_write_register(ViPipe, 0x23, 0x02);
        jxh62_write_register(ViPipe, 0x22, 0xEE);
        if (g_pastJxh62[ViPipe]) {
            g_pastJxh62[ViPipe]->bSyncInit = GK_FALSE;
        }
    }
}

static GK_S32 cmos_set_wdr_mode(VI_PIPE ViPipe, GK_U8 u8Mode)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    pstSnsState->bSyncInit = GK_FALSE;
    pstSnsState->enWDRMode = WDR_MODE_NONE;
    pstSnsState->u32FLStd = JXH62_VMAX_720P30_LINEAR;
    pstSnsState->au32FL[0] = pstSnsState->u32FLStd;
    pstSnsState->au32FL[1] = pstSnsState->au32FL[0];
    return GK_SUCCESS;
}

static GK_S32 cmos_get_sns_regs_info(VI_PIPE ViPipe, ISP_SNS_REGS_INFO_S *pstSnsRegsInfo)
{
    GK_S32 i;
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    if (pstSnsRegsInfo == GK_NULL || pstSnsState == GK_NULL) {
        return GK_FAILURE;
    }
    if ((pstSnsState->bSyncInit == GK_FALSE) || (pstSnsRegsInfo->bConfig == GK_FALSE)) {
        pstSnsState->astRegsInfo[0].enSnsType = ISP_SNS_I2C_TYPE;
        pstSnsState->astRegsInfo[0].unComBus.s8I2cDev = g_aunJxh62BusInfo[ViPipe].s8I2cDev;
        pstSnsState->astRegsInfo[0].u8Cfg2ValidDelayMax = 2;
        pstSnsState->astRegsInfo[0].u32RegNum = 5;
        for (i = 0; i < pstSnsState->astRegsInfo[0].u32RegNum; i++) {
            pstSnsState->astRegsInfo[0].astI2cData[i].bUpdate = GK_TRUE;
            pstSnsState->astRegsInfo[0].astI2cData[i].u8DevAddr = JXH62_I2C_ADDR;
            pstSnsState->astRegsInfo[0].astI2cData[i].u32AddrByteNum = JXH62_ADDR_BYTE;
            pstSnsState->astRegsInfo[0].astI2cData[i].u32DataByteNum = JXH62_DATA_BYTE;
        }
        pstSnsState->astRegsInfo[0].astI2cData[0].u8DelayFrmNum = 0;
        pstSnsState->astRegsInfo[0].astI2cData[0].u32RegAddr = 0x01;
        pstSnsState->astRegsInfo[0].astI2cData[1].u8DelayFrmNum = 0;
        pstSnsState->astRegsInfo[0].astI2cData[1].u32RegAddr = 0x02;
        pstSnsState->astRegsInfo[0].astI2cData[2].u8DelayFrmNum = 0;
        pstSnsState->astRegsInfo[0].astI2cData[2].u32RegAddr = 0x22;
        pstSnsState->astRegsInfo[0].astI2cData[3].u8DelayFrmNum = 0;
        pstSnsState->astRegsInfo[0].astI2cData[3].u32RegAddr = 0x23;
        pstSnsState->astRegsInfo[0].astI2cData[4].u8DelayFrmNum = 0;
        pstSnsState->astRegsInfo[0].astI2cData[4].u32RegAddr = 0x00;
        pstSnsState->bSyncInit = GK_TRUE;
    } else {
        for (i = 0; i < pstSnsState->astRegsInfo[0].u32RegNum; i++) {
            if (pstSnsState->astRegsInfo[0].astI2cData[i].u32Data == pstSnsState->astRegsInfo[1].astI2cData[i].u32Data) {
                pstSnsState->astRegsInfo[0].astI2cData[i].bUpdate = GK_FALSE;
            } else {
                pstSnsState->astRegsInfo[0].astI2cData[i].bUpdate = GK_TRUE;
            }
        }
    }
    pstSnsRegsInfo->bConfig = GK_FALSE;
    memcpy(pstSnsRegsInfo, &pstSnsState->astRegsInfo[0], sizeof(ISP_SNS_REGS_INFO_S));
    memcpy(&pstSnsState->astRegsInfo[1], &pstSnsState->astRegsInfo[0], sizeof(ISP_SNS_REGS_INFO_S));
    pstSnsState->au32FL[1] = pstSnsState->au32FL[0];
    return GK_SUCCESS;
}

static GK_S32 cmos_set_image_mode(VI_PIPE ViPipe, ISP_CMOS_SENSOR_IMAGE_MODE_S *pstSensorImageMode)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    pstSnsState->bSyncInit = GK_FALSE;
    pstSnsState->u32FLStd = JXH62_VMAX_720P30_LINEAR;
    pstSnsState->u8ImgMode = 0;
    return GK_SUCCESS;
}

static GK_VOID sensor_global_init(VI_PIPE ViPipe)
{
    ISP_SNS_STATE_S *pstSnsState = g_pastJxh62[ViPipe];
    pstSnsState->bInit = GK_FALSE;
    pstSnsState->bSyncInit = GK_FALSE;
    pstSnsState->u8ImgMode = 0;
    pstSnsState->enWDRMode = WDR_MODE_NONE;
    pstSnsState->u32FLStd = JXH62_VMAX_720P30_LINEAR;
    pstSnsState->au32FL[0] = JXH62_VMAX_720P30_LINEAR;
    pstSnsState->au32FL[1] = JXH62_VMAX_720P30_LINEAR;
    memset(&pstSnsState->astRegsInfo[0], 0, sizeof(ISP_SNS_REGS_INFO_S));
    memset(&pstSnsState->astRegsInfo[1], 0, sizeof(ISP_SNS_REGS_INFO_S));
}

static GK_S32 sensor_register_callback(VI_PIPE ViPipe, ALG_LIB_S *pstAeLib, ALG_LIB_S *pstAwbLib)
{
    GK_S32 s32Ret;
    ISP_SENSOR_REGISTER_S stIspRegister;
    AE_SENSOR_REGISTER_S  stAeRegister;
    AWB_SENSOR_REGISTER_S stAwbRegister;
    ISP_SNS_ATTR_INFO_S   stSnsAttrInfo;

    if (g_pastJxh62[ViPipe] == GK_NULL) {
        g_pastJxh62[ViPipe] = (ISP_SNS_STATE_S *)malloc(sizeof(ISP_SNS_STATE_S));
        if (g_pastJxh62[ViPipe] == GK_NULL) {
            return GK_FAILURE;
        }
    }
    memset(g_pastJxh62[ViPipe], 0, sizeof(ISP_SNS_STATE_S));
    sensor_global_init(ViPipe);
    stSnsAttrInfo.eSensorId = JXH62_ID;

    memset(&stIspRegister, 0, sizeof(ISP_SENSOR_REGISTER_S));
    stIspRegister.stSnsExp.pfn_cmos_sensor_init = jxh62_init;
    stIspRegister.stSnsExp.pfn_cmos_sensor_exit = jxh62_exit;
    stIspRegister.stSnsExp.pfn_cmos_sensor_global_init = sensor_global_init;
    stIspRegister.stSnsExp.pfn_cmos_set_image_mode = cmos_set_image_mode;
    stIspRegister.stSnsExp.pfn_cmos_set_wdr_mode = cmos_set_wdr_mode;
    stIspRegister.stSnsExp.pfn_cmos_get_isp_default = cmos_get_isp_default;
    stIspRegister.stSnsExp.pfn_cmos_get_isp_black_level = cmos_get_isp_black_level;
    stIspRegister.stSnsExp.pfn_cmos_set_pixel_detect = cmos_set_pixel_detect;
    stIspRegister.stSnsExp.pfn_cmos_get_sns_reg_info = cmos_get_sns_regs_info;
    s32Ret = HI_MPI_ISP_SensorRegCallBack(ViPipe, &stSnsAttrInfo, &stIspRegister);
    if (s32Ret != GK_SUCCESS) {
        return s32Ret;
    }

    memset(&stAeRegister, 0, sizeof(AE_SENSOR_REGISTER_S));
    stAeRegister.stSnsExp.pfn_cmos_get_ae_default = cmos_get_ae_default;
    stAeRegister.stSnsExp.pfn_cmos_fps_set = cmos_fps_set;
    stAeRegister.stSnsExp.pfn_cmos_slow_framerate_set = cmos_slow_framerate_set;
    stAeRegister.stSnsExp.pfn_cmos_inttime_update = cmos_inttime_update;
    stAeRegister.stSnsExp.pfn_cmos_gains_update = cmos_gains_update;
    stAeRegister.stSnsExp.pfn_cmos_again_calc_table = cmos_again_calc_table;
    stAeRegister.stSnsExp.pfn_cmos_dgain_calc_table = cmos_dgain_calc_table;
    s32Ret = HI_MPI_AE_SensorRegCallBack(ViPipe, pstAeLib, &stSnsAttrInfo, &stAeRegister);
    if (s32Ret != GK_SUCCESS) {
        return s32Ret;
    }

    memset(&stAwbRegister, 0, sizeof(AWB_SENSOR_REGISTER_S));
    stAwbRegister.stSnsExp.pfn_cmos_get_awb_default = cmos_get_awb_default;
    s32Ret = HI_MPI_AWB_SensorRegCallBack(ViPipe, pstAwbLib, &stSnsAttrInfo, &stAwbRegister);
    if (s32Ret != GK_SUCCESS) {
        return s32Ret;
    }

    return GK_SUCCESS;
}

static GK_S32 sensor_unregister_callback(VI_PIPE ViPipe, ALG_LIB_S *pstAeLib, ALG_LIB_S *pstAwbLib)
{
    HI_MPI_ISP_SensorUnRegCallBack(ViPipe, JXH62_ID);
    HI_MPI_AE_SensorUnRegCallBack(ViPipe, pstAeLib, JXH62_ID);
    HI_MPI_AWB_SensorUnRegCallBack(ViPipe, pstAwbLib, JXH62_ID);
    if (g_pastJxh62[ViPipe] != GK_NULL) {
        free(g_pastJxh62[ViPipe]);
        g_pastJxh62[ViPipe] = GK_NULL;
    }
    return GK_SUCCESS;
}

static GK_S32 sensor_set_init(VI_PIPE ViPipe, ISP_INIT_ATTR_S *pstInitAttr)
{
    g_au32InitExposure[ViPipe]  = pstInitAttr->u32Exposure;
    g_au32LinesPer500ms[ViPipe] = pstInitAttr->u32LinesPer500ms;
    g_au16InitWBGain[ViPipe][0] = pstInitAttr->u16WBRgain;
    g_au16InitWBGain[ViPipe][1] = pstInitAttr->u16WBGgain;
    g_au16InitWBGain[ViPipe][2] = pstInitAttr->u16WBBgain;
    g_au16SampleRgain[ViPipe]   = pstInitAttr->u16SampleRgain;
    g_au16SampleBgain[ViPipe]   = pstInitAttr->u16SampleBgain;
    return GK_SUCCESS;
}

static GK_S32 cmos_set_bus_info(VI_PIPE ViPipe, ISP_SNS_COMMBUS_U unSNSBusInfo)
{
    g_aunJxh62BusInfo[ViPipe].s8I2cDev = unSNSBusInfo.s8I2cDev;
    return GK_SUCCESS;
}

ISP_SNS_OBJ_S stSnsSoiH62Obj = {
    .pfnRegisterCallback   = sensor_register_callback,
    .pfnUnRegisterCallback = sensor_unregister_callback,
    .pfnStandby            = jxh62_standby,
    .pfnRestart            = jxh62_restart,
    .pfnMirrorFlip         = GK_NULL,
    .pfnWriteReg           = jxh62_write_register,
    .pfnReadReg            = jxh62_read_register,
    .pfnSetBusInfo         = cmos_set_bus_info,
    .pfnSetInit            = sensor_set_init
};

extern ISP_SNS_OBJ_S stSnsJxH62Obj __attribute__((alias("stSnsSoiH62Obj")));
extern ISP_SNS_OBJ_S stSnsJxh62Obj __attribute__((alias("stSnsSoiH62Obj")));
extern ISP_SNS_OBJ_S stSnsH62Obj __attribute__((alias("stSnsSoiH62Obj")));
extern ISP_SNS_OBJ_S stSnsSoiSensorObj __attribute__((alias("stSnsSoiH62Obj")));
