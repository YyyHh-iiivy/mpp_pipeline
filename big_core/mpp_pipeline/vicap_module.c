#include "mpp_pipeline.h"

/* ================================================================
 * Step 3: VICAP (VI) 配置 1080P
 *
 * 查询传感器 → 配置 device → 配置 channel → init
 * ================================================================ */
/**
 * @brief 尝试配置VICAP模块以连接指定类型的传感器
 * 
 * 该函数执行以下步骤：
 * 1. 查询指定传感器类型的信息
 * 2. 根据传感器信息配置VICAP设备属性
 * 3. 配置通道属性为1080P分辨率，NV12格式，无裁剪和缩放
 * 4. 初始化VICAP设备
 * 
 * @param sensor_type 传感器类型
 * @return k_s32 成功返回0，失败返回错误码
 */
k_s32 vicap_try_config(k_vicap_sensor_type sensor_type)
{
    k_s32 ret;                                    // 返回值，用于存储API调用结果
    k_vicap_dev_attr dev_attr;                    // VICAP设备属性结构体，用于配置设备参数
    k_vicap_chn_attr chn_attr;                    // VICAP通道属性结构体，用于配置通道参数
    k_vicap_sensor_info sensor_info;              // 传感器信息结构体，用于存储传感器详细信息
    k_u32 width = ENC_WIDTH;                      // 输出宽度，从ENC_WIDTH宏获取默认值
    k_u32 height = ENC_HEIGHT;                    // 输出高度，从ENC_HEIGHT宏获取默认值

    // 清零各个配置结构体，确保所有字段初始化为0
    memset(&dev_attr, 0, sizeof(dev_attr));       // 清零设备属性结构体
    memset(&chn_attr, 0, sizeof(chn_attr));       // 清零通道属性结构体
    memset(&sensor_info, 0, sizeof(sensor_info)); // 清零传感器信息结构体

    /* 3.1 查询传感器信息 */
    sensor_info.sensor_type = sensor_type;         // 设置要查询的传感器类型
    ret = kd_mpi_vicap_get_sensor_info(sensor_info.sensor_type, &sensor_info);  // 获取传感器详细信息
    if (ret) {                                    // 检查API调用是否成功
        LOG("kd_mpi_vicap_get_sensor_info failed! ret=0x%x (sensor_type=%d)", ret, sensor_type);  // 记录错误日志
        return ret;                               // 返回错误码
    }
    LOG("Sensor: %s %ux%u@%ufps csi=%d lanes=%d type=%d",
        sensor_info.sensor_name, sensor_info.width, sensor_info.height,  // 打印传感器名称、宽高
        sensor_info.fps, sensor_info.csi_num, sensor_info.mipi_lanes, sensor_type);  // 打印帧率、CSI号、MIPI通道数和类型

    /* 3.2 配置 device */
    dev_attr.acq_win.width  = sensor_info.width;   // 设置采集窗口宽度为传感器实际宽度
    dev_attr.acq_win.height = sensor_info.height;  // 设置采集窗口高度为传感器实际高度
    dev_attr.mode           = VICAP_WORK_ONLINE_MODE;  // 设置工作模式为在线模式

    dev_attr.pipe_ctrl.bits.ae_enable  = 1;        // 启用自动曝光(AE)功能
    dev_attr.pipe_ctrl.bits.awb_enable = 1;        // 启用自动白平衡(AWB)功能

    memcpy(&dev_attr.sensor_info, &sensor_info, sizeof(k_vicap_sensor_info));  // 将传感器信息复制到设备属性中

    ret = kd_mpi_vicap_set_dev_attr(VICAP_DEV, dev_attr);  // 设置VICAP设备属性
    if (ret) {                                    // 检查设置是否成功
        LOG("kd_mpi_vicap_set_dev_attr failed! ret=0x%x", ret);  // 记录错误日志
        return ret;                               // 返回错误码
    }

    /* 3.3 配置 channel: 1080P NV12, 不裁剪不缩放 */
    width = ALIGN_UP(width, 16);                  // 宽度按16字节对齐，满足硬件要求
    chn_attr.out_win.width  = width;              // 设置输出窗口宽度
    chn_attr.out_win.height = height;             // 设置输出窗口高度

    chn_attr.crop_win     = chn_attr.out_win;     // 裁剪窗口设为与输出窗口相同（无裁剪）
    chn_attr.scale_win    = chn_attr.out_win;     // 缩放窗口设为与输出窗口相同（无缩放）
    chn_attr.crop_enable  = K_FALSE;              // 禁用裁剪功能
    chn_attr.scale_enable = K_FALSE;              // 禁用缩放功能
    chn_attr.chn_enable   = K_TRUE;               // 启用通道
    chn_attr.alignment    = 12;                   /* 4096 字节对齐 */
    chn_attr.pix_format   = PIXEL_FORMAT_YUV_SEMIPLANAR_420;  /* NV12像素格式 */
    chn_attr.buffer_num   = INPUT_BUF_CNT;        // 设置输入缓冲区数量
    chn_attr.buffer_size  = CHN_BUF_SIZE;         // 设置通道缓冲区大小

    ret = kd_mpi_vicap_set_chn_attr(VICAP_DEV, VICAP_CHN, chn_attr);  // 设置VICAP通道属性
    if (ret) {                                    // 检查设置是否成功
        LOG("kd_mpi_vicap_set_chn_attr failed! ret=0x%x", ret);  // 记录错误日志
        return ret;                               // 返回错误码
    }

    /* 3.4 初始化 VICAP */
    ret = kd_mpi_vicap_init(VICAP_DEV);           // 初始化VICAP设备
    if (ret) {                                    // 检查初始化是否成功
        LOG("kd_mpi_vicap_init failed! ret=0x%x", ret);  // 记录错误日志
        kd_mpi_vicap_deinit(VICAP_DEV);           // 反初始化设备
        return ret;                               // 返回错误码
    }
    g_status = STATUS_VICAP_INIT;                 // 更新全局状态为VICAP已初始化

    LOG("VICAP dev=%d chn=%d init OK", VICAP_DEV, VICAP_CHN);  // 记录初始化成功日志
    return 0;                                     // 返回成功
}

/**
 * @brief 配置VICAP模块，包含故障转移机制
 * 
 * 该函数首先尝试使用指定的传感器类型进行配置，如果失败，则依次尝试预定义的备用传感器类型
 * 
 * @param sensor_type 主要传感器类型
 * @return k_s32 成功返回0，所有尝试都失败则返回最后一个错误码
 */
k_s32 vicap_config(k_vicap_sensor_type sensor_type)
{
    k_s32 ret;                                    // 返回值，存储配置结果
    const k_vicap_sensor_type fallback_types[] = {  // 备用传感器类型数组，按优先级排序
        SENSOR_TYPE,                              // 默认传感器类型
        GC2093_MIPI_CSI0_1920X1080_30FPS_10BIT_LINEAR,  // GC2093传感器，CSI0接口
        GC2093_MIPI_CSI1_1920X1080_30FPS_10BIT_LINEAR,  // GC2093传感器，CSI1接口
    };

    ret = vicap_try_config(sensor_type);          // 首先尝试使用指定的传感器类型进行配置
    if (ret == 0)                                 // 检查主要配置是否成功
        return 0;                                 // 如果成功则直接返回

    LOG("Primary sensor_type=%d failed. Trying GC2093 CSI fallbacks...", sensor_type);  // 记录主要配置失败的日志
    for (k_u32 i = 0; i < sizeof(fallback_types) / sizeof(fallback_types[0]); i++) {  // 遍历备用传感器类型数组
        if (fallback_types[i] == sensor_type)     // 检查备用类型是否与原类型相同
            continue;                             // 相同则跳过，避免重复尝试

        ret = vicap_try_config(fallback_types[i]);  // 尝试使用备用传感器类型进行配置
        if (ret == 0)                             // 检查备用配置是否成功
            return 0;                             // 成功则直接返回
    }

    return ret;                                   // 所有尝试都失败，返回最后的错误码
}

/* ================================================================
 * Step 5: 启动 VICAP 码流
 * ================================================================ */
/**
 * @brief 启动VICAP码流
 * 
 * 该函数启动VICAP设备的数据流，使设备开始捕获并输出视频数据
 * 
 * @return k_s32 成功返回0，失败返回错误码
 */
k_s32 vicap_start(void)
{
    k_s32 ret;                                  // 返回值，用于存储API调用结果

    ret = kd_mpi_vicap_start_stream(VICAP_DEV); // 启动VICAP设备的数据流，参数为设备号
    if (ret) {                                  // 检查启动操作是否成功
        LOG("kd_mpi_vicap_start_stream failed! ret=0x%x", ret);  // 记录启动失败的日志
        return ret;                             // 返回错误码
    }
    g_status = STATUS_STREAM_STARTED;           // 更新全局状态为码流已启动

    LOG("VICAP stream started");              // 记录启动成功的日志
    return 0;                                   // 返回成功状态
}