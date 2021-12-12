#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>

void child()
{
    FILE *logfile = fopen("/home/pi/ramdisk/monping.log", "a");
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
    
    struct pollfd fds;
    fds.fd = socket(PF_INET, SOCK_RAW, IPPROTO_ICMP);
    assert(fds.fd != -1);
    
    struct {
        uint32_t time;
        uint32_t data;
        uint32_t mask;
    } message;
    
    int counts[4];

    struct timespec clock_mon;
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
    message.time = clock_mon.tv_sec;
    memset(counts, 0, sizeof(counts));

    while (1)
    {
        fds.events = POLLIN | POLLPRI;

        int pollret = poll(&fds, 1, 250);
        assert(pollret != -1);

        assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
        uint32_t newtime = clock_mon.tv_sec;
        if (newtime != message.time)
        {
            int i;
            message.data = 1;
            for (i = 0; i < 4; i++)
            {
                assert(counts[i] >= 0);
                if (counts[i] > 3) counts[i] = 3;
                
                message.data <<= 2;
                message.data |= counts[i];
            }
            message.mask = (0x1ff);  // 9bits: 1bit plus 4 x 2bits

            message.data <<= 14;
            message.mask <<= 14;

            ssize_t bytessent = send(sock, &message, sizeof(message), 0);
            assert(bytessent == sizeof(message));

            message.time = newtime;
            memset(counts, 0, sizeof(counts));
        }  

        if (pollret == 1)
        {
            assert(fds.revents == POLLIN);
            
            unsigned char res[100];
            int ressponse = recv(fds.fd, res, sizeof(res), 0);
            assert(ressponse > 0);
            
            printf("%4d ", ressponse);
            for(int i = 0; i < ressponse; i++)
            printf("%02x %s", res[i], ((i + 1)  % 10 == 0) ? "," : "");
            printf(" (%d)\n", res[64]);
            
            assert((res[64] >= 0) && (res[64] < 4));
            counts[res[64]]++;
        }
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
