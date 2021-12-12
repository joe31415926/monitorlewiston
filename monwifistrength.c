#include <stdio.h>
#include <assert.h>
#include <unistd.h>     // read, write, fork, sleep
#include <fcntl.h>      // open
#include <stdlib.h>     // exit()
#include <string.h>     // strstr()
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <error.h>

void child()
{
    FILE *logfile = fopen("/home/pi/ramdisk/monwifistrength.log", "a");
    if (logfile)
    {
        fprintf(logfile, "restart %d\n", (int) time(NULL));
        fclose(logfile);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7356);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);


    int sock = socket(PF_INET, SOCK_STREAM, 0);
    assert(sock != -1);

    assert(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    
    while (1)
    {
        int fd = open("/proc/net/wireless", O_RDONLY);
        assert(fd > 1);
        
        char buffer[1000];
        ssize_t numchar = read(fd,  buffer, sizeof(buffer));
        assert(numchar > 200);
        buffer[numchar] = '\0';
        
        char *wlan0 = strstr(buffer, "wlan0");
        assert(wlan0 != NULL);
        
        int a, b, c;
        assert(sscanf(wlan0, "wlan0: %d %d. %d.  -256", &a, &b, &c) == 3);
        
        struct {
            uint32_t time;
            uint32_t data;
            uint32_t mask;
        } message;
        
        // c is typically -56 .. -38
        c += 64;
        // c is typically   8 ..  26
        assert((c >= 0) && (c < 32));
        
        struct timespec clock_mon;
        assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
        message.time = clock_mon.tv_sec;
        message.data = ((1 << 5) | c);  // 1 bit + c is 5 bits = 6 bits
        message.mask = (0x3f);
        
        message.data <<= 23;
        message.mask <<= 23;

        ssize_t bytessent = send(sock, &message, sizeof(message), 0);
        assert(bytessent == sizeof(message));        
        close(fd);
        usleep(250000);
    }
}


void mommy()
{
    while (1)
    {
        if (!fork()) child();
        wait(NULL);
        sleep(3);    // wait for a second so we don't spin super quickly
    }
}

int main()
{
    if (!fork()) mommy();
    return 0;
}
