#include "hal_locker.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

#include <sys/stat.h>   // 用于文件操作


/* 帧头固定4字节（十六进制） */
static const uint8_t FRAME_HEADER[4] = {0x57, 0x4B, 0x4C, 0x59};

/* ---------- 内部辅助函数 ---------- */

/**
 * @brief 计算异或校验和（XOR）
 * @param data 数据指针
 * @param len  字节数
 * @return 校验值
 */
static uint8_t compute_xor(const uint8_t *data, uint16_t len)
{
    uint8_t xor_sum = 0;
    for (uint16_t i = 0; i < len; i++) {
        xor_sum ^= data[i];
    }
    printf("xor_sum = 0x%02X\n", xor_sum);
    return xor_sum;
}

/**
 * @brief 发送数据并等待回复（带超时）
 * @param fd         串口fd
 * @param tx_buf     发送缓冲区
 * @param tx_len     发送长度
 * @param rx_buf     接收缓冲区（用于存放回复）
 * @param rx_len     期望接收的字节数（可小于实际，但建议精确）
 * @param timeout_ms 超时毫秒
 * @return 0=成功收到期望字节数, -1=超时, -2=读取出错
 */
static int send_and_wait_reply(int fd, const uint8_t *tx_buf, int tx_len,
                               uint8_t *rx_buf, int rx_len, int timeout_ms)
{
    // 清空串口缓冲区（可选，避免读到旧数据）
    tcflush(fd, TCIFLUSH);

    // ---------- 打印信息 ----------
    printf("TX Frame: ");
    for (int i = 0; i < tx_len; i++) {
        printf("%02X ", tx_buf[i]);
    }
    printf("\n");
    fflush(stdout);

    // ---------- 发送数据 ----------
    ssize_t written = write(fd, tx_buf, tx_len);
    if (written != tx_len) {
        // gpio_set_value(0); // 异常时记得恢复接收模式
        return -2;
    }
    // tcdrain(fd);          // 等待发送完成（硬件FIFO发完）

    // ---------- 切换为接收模式 ----------

    if (tcdrain(fd) < 0) {
        perror("tcdrain");
        // gpio_set_value(0);
        return -2;
    }

    // 如果不需要回复（rx_len==0），直接返回
    if (rx_len == 0) {
        return 0;
    }

    // 使用select实现超时
    fd_set read_fds;
    struct timeval tv;
    int ret;
    int total_read = 0;
    int remain = rx_len;

    while (remain > 0) {
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        ret = select(fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            return -2;  // select出错
        } else if (ret == 0) {
            printf("RX timeout, received %d/%d bytes: ",
           total_read, rx_len);
            for (int i = 0; i < total_read; i++) {
                printf("%02X ", rx_buf[i]);
            }
            printf("\n");
            return -1;  // 超时
        }

        // 可读，读取剩余字节
        ssize_t n = read(fd, rx_buf + total_read, remain);
        if (n <= 0) {
            return -2;  // 读取错误
        }
        total_read += n;
        remain -= n;
    }
    // ---------- 打印信息 ----------
    printf("RX Frame (%d bytes): ", total_read);

    for (int i = 0; i < total_read; i++) {
        printf("%02X ", rx_buf[i]);
    }

    printf("\n");
    return 0;
}

/**
 * @brief 解析回复帧，验证指令字和状态
 * @param rx_buf    接收到的数据
 * @param rx_len    实际接收长度（应为至少11字节？但不同指令长度不同）
 * @param expected_cmd 期望的指令码
 * @param status    输出状态（回复帧的第8个字节，索引7）
 * @return 0=成功, -2=格式错误
 */
static int parse_reply(const uint8_t *rx_buf, int rx_len, uint8_t expected_cmd, uint8_t *status)
{
    if (rx_len < 9) {
        return -2;  // 最小长度（开全部锁回复9字节）
    }
    // 检查帧头
    if (memcmp(rx_buf, FRAME_HEADER, 4) != 0) {
        return -2;
    }
    // 检查指令字（索引6）
    if (rx_buf[6] != expected_cmd) {
        return -2;
    }
    // 状态字节：对于大部分回复，状态在索引7（但开锁单通道回复是索引7，开全部锁是索引7？）
    // 开锁单通道回复：起始符4 + 长度1 + 地址1 + 指令1 + 状态1 + 通道1 + 锁状态1 + 校验1 = 11字节，状态在索引7
    // 开全部锁回复：起始符4 + 长度1 + 地址1 + 指令1 + 状态1 + 校验1 = 9字节，状态在索引7
    // 所以我们统一用索引7作为状态
    if (status) {
        *status = rx_buf[7];
    }
    return 0;
}

/* ---------- 对外接口实现 ---------- */

int lock_serial_init(const char *device_path, int baudrate)
{
    int fd;
    struct termios tio;
    struct serial_rs485 rs485;

    if (device_path == NULL) {
        device_path = LOCK_DEVICE;
    }
    if (baudrate == 0) {
        baudrate = LOCK_BAUDRATE;
    }

    // fd = open(device_path, O_RDWR | O_NOCTTY | O_NDELAY); 
    // 你的程序本身已经用 select() 做超时了，完全没必要再把 UART 文件描述符设置成 non-blocking

    fd = open(device_path, O_RDWR | O_NOCTTY);

    if (fd < 0) {
        perror("lock_serial_init open");
        return -1;
    }

    // 获取当前配置
    if (tcgetattr(fd, &tio) < 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    // 设为原始模式
    cfmakeraw(&tio);

    // 设置波特率
    cfsetispeed(&tio, baudrate);
    cfsetospeed(&tio, baudrate);

    // 数据位8、无校验、1停止位（cfmakeraw已经设置了8N1，但显式设置更清晰）
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CRTSCTS;   // 无硬件流控
    tio.c_iflag &= ~(IXON | IXOFF | IXANY); // 禁用软件流控

    // 使能接收
    tio.c_cflag |= CLOCAL | CREAD;

    // 设置读取超时（非规范模式，使用VMIN和VTIME，但我们用select，可以不用）
    // 这里设置最小字符数为0，超时0，避免阻塞
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    // 应用配置
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    // if (gpio_init() < 0) {
    //     printf("GPIO 初始化失败，但继续执行...\n");
    //     // 根据你的需求，可以返回 -1
    // }

    memset(&rs485, 0, sizeof(rs485));

    // rs485.flags = SER_RS485_ENABLED |
    //             SER_RS485_RTS_ON_SEND;

    rs485.flags = SER_RS485_ENABLED |
              SER_RS485_RTS_AFTER_SEND;

    rs485.delay_rts_before_send = 0;
    rs485.delay_rts_after_send = 0;

    if (ioctl(fd, TIOCSRS485, &rs485) < 0) {
        perror("TIOCSRS485");
        return -1;
    }
    // // ---------- 配置RS485模式（针对IMX6ULL） ----------
    // memset(&rs485, 0, sizeof(rs485));
    // // 使能RS485模式，RTS在发送时置高
    // rs485.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
    // rs485.delay_rts_before_send = 0;
    // rs485.delay_rts_after_send = 0;
    // if (ioctl(fd, TIOCSRS485, &rs485) < 0) {
    //     // 不是所有平台都支持，如果失败则警告但不影响
    //     perror("TIOCSRS485 (警告)");
    //     // 但继续，因为可能不需要硬件自动控制RTS，用户之前代码中启用也成功了
    //     // 若你的硬件必须，则返回错误
    //     // close(fd); return -1;
    // }
    struct serial_rs485 check;
    memset(&check, 0, sizeof(check));

    if (ioctl(fd, TIOCGRS485, &check) < 0) {
        perror("TIOCGRS485");
    } else {
        printf("RS485 flags = 0x%08X\n", check.flags);
        printf("ENABLED    = %d\n",
            !!(check.flags & SER_RS485_ENABLED));
        printf("ON_SEND    = %d\n",
            !!(check.flags & SER_RS485_RTS_ON_SEND));
        printf("AFTER_SEND = %d\n",
            !!(check.flags & SER_RS485_RTS_AFTER_SEND));
    }
    return fd;
}

void lock_serial_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}

int lock_open_single(int fd, uint8_t box_id, int timeout_ms)
{
    uint8_t tx_buf[9];
    uint8_t rx_buf[11]; // 回复最大11字节
    int ret;
    uint8_t status;

    if (fd < 0 || box_id < 1 || box_id > 12) {
        return -2;
    }
    if (timeout_ms <= 0) {
        timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;
    }

    // 构建发送帧
    tx_buf[0] = FRAME_HEADER[0];
    tx_buf[1] = FRAME_HEADER[1];
    tx_buf[2] = FRAME_HEADER[2];
    tx_buf[3] = FRAME_HEADER[3];
    tx_buf[4] = 0x09;  // 帧长度：4头+1长度+1地址+1指令+1通道+1校验 = 9
    tx_buf[5] = DEFAULT_BOARD_ADDR;
    tx_buf[6] = CMD_OPEN_SINGLE;
    tx_buf[7] = box_id;
    tx_buf[8] = compute_xor(tx_buf, 8); // 校验前面8字节
  

    // 发送并等待回复（期望11字节）
    ret = send_and_wait_reply(fd, tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf), timeout_ms);
    if (ret < 0) {
        return ret; // -1超时, -2错误
    }

    // 解析回复
    ret = parse_reply(rx_buf, sizeof(rx_buf), CMD_OPEN_SINGLE, &status);
    if (ret < 0) {
        return -2;
    }
    // 检查状态
    if (status != LOCK_OP_SUCCESS) {
        return -2; // 可细化错误码
    }

    // 可选：检查回复中的通道号是否一致，这里省略

    return 0;
}

int lock_read_single(int fd, uint8_t box_id, uint8_t *status, int timeout_ms)
{
    uint8_t tx_buf[9];
    uint8_t rx_buf[11];
    int ret;
    uint8_t recv_status;

    if (fd < 0 || box_id < 1 || box_id > 12) {
        return -2;
    }
    if (timeout_ms <= 0) {
        timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;
    }

    tx_buf[0] = FRAME_HEADER[0];
    tx_buf[1] = FRAME_HEADER[1];
    tx_buf[2] = FRAME_HEADER[2];
    tx_buf[3] = FRAME_HEADER[3];
    tx_buf[4] = 0x09;
    tx_buf[5] = DEFAULT_BOARD_ADDR;
    tx_buf[6] = CMD_READ_SINGLE;
    tx_buf[7] = box_id;
    tx_buf[8] = compute_xor(tx_buf, 8);

    ret = send_and_wait_reply(fd, tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf), timeout_ms);
    if (ret < 0) {
        return ret;
    }

    ret = parse_reply(rx_buf, sizeof(rx_buf), CMD_READ_SINGLE, &recv_status);
    if (ret < 0) {
        return -2;
    }
    // recv_status是执行状态，但锁状态在索引9（第10字节）？
    // 根据协议，回复帧：起始符4 + 长度1 + 地址1 + 指令1 + 状态1 + 通道1 + 锁状态1 + 校验1
    // 所以锁状态在索引9（从0开始：0-3头,4长度,5地址,6指令,7状态,8通道,9锁状态,10校验）
    // 但我们的parse_reply只取了状态，需要额外解析
    if (rx_buf[7] != LOCK_OP_SUCCESS) {
        return -2;
    }
    if (status) {
        *status = rx_buf[9]; // 锁状态（0关闭，1打开）
    }
    return 0;
}

int lock_open_all(int fd, int timeout_ms)
{
    uint8_t tx_buf[8];
    uint8_t rx_buf[9];
    int ret;
    uint8_t status;

    if (fd < 0) return -2;
    if (timeout_ms <= 0) timeout_ms = LOCK_DEFAULT_TIMEOUT_MS;

    tx_buf[0] = FRAME_HEADER[0];
    tx_buf[1] = FRAME_HEADER[1];
    tx_buf[2] = FRAME_HEADER[2];
    tx_buf[3] = FRAME_HEADER[3];
    tx_buf[4] = 0x08;  // 长度8：无数据域
    tx_buf[5] = DEFAULT_BOARD_ADDR;
    tx_buf[6] = CMD_OPEN_ALL;
    tx_buf[7] = compute_xor(tx_buf, 7); // 校验前面7字节

    ret = send_and_wait_reply(fd, tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf), timeout_ms);
    if (ret < 0) {
        return ret;
    }

    ret = parse_reply(rx_buf, sizeof(rx_buf), CMD_OPEN_ALL, &status);
    if (ret < 0) {
        return -2;
    }
    if (status != LOCK_OP_SUCCESS) {
        return -2;
    }
    return 0;
}

