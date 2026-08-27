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

// 在 hal_locker.c 中添加
int hal_serial_send(int fd, const uint8_t *data, int len)
{
    if (fd < 0 || data == NULL || len <= 0) return -1;

    // 1. 切换 RS485 为发送模式（GPIO 拉高）
    //    （假设你有对应的 GPIO 操作，或通过 ioctl TIOCSRS485 自动控制）
    //    如果之前用 ioctl 设置了 SER_RS485_ENABLED，内核会自动处理，
    //    但为了保险，你可以手动拉高 GPIO。

    // 2. 发送数据
    ssize_t written = write(fd, data, len);
    if (written != len) {
        perror("hal_serial_send write");
        return -1;
    }

    // 3. 等待发送完成
    if (tcdrain(fd) < 0) {
        perror("tcdrain");
        return -1;
    }

    // 4. 延时 2ms，让锁控板有时间切换状态（针对半双工）
    usleep(2000);

    // 5. 切换回接收模式（GPIO 拉低）
    //    如果使用 ioctl TIOCSRS485 且设置了 RTS_AFTER_SEND，内核会在发送后自动拉低，
    //    否则需要手动操作。

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

    memset(&rs485, 0, sizeof(rs485));

    rs485.flags = SER_RS485_ENABLED |
              SER_RS485_RTS_AFTER_SEND;

    rs485.delay_rts_before_send = 0;
    rs485.delay_rts_after_send = 0;

    if (ioctl(fd, TIOCSRS485, &rs485) < 0) {
        perror("TIOCSRS485");
        return -1;
    }
    
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
