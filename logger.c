#include <stdio.h>
#include <assert.h>
#include <unistd.h>     // read, write, fork, sleep
#include <fcntl.h>      // open
#include <stdlib.h>     // exit()
#include <string.h>     // memmove()
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct {
    uint32_t time;
    uint32_t buf;
} cache[300];
int ncache;

int fd;
ssize_t entries_in_file;
uint32_t log_start;
nfds_t nfds;

void flushbuffer(void)
{
    // wait until `/usr/bin/timedatectl -p NTPSynchronized show` returns NTPSynchronized=yes
    int wstatus = system("/home/pi/timesync.sh");
    if (!WIFEXITED(wstatus) || (WEXITSTATUS(wstatus) != 8))
        return;
        
    // OK, time is synchronized.
    struct timespec clock_real;
    assert(clock_gettime(CLOCK_REALTIME, &clock_real) == 0);
    struct timespec clock_mon;
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
    uint32_t clock_delta = clock_real.tv_sec - clock_mon.tv_sec;
    
    // Do we have anything to write?
    if (ncache < 1) return;

    int i;    
    uint32_t earliest_time = cache[0].time;
    for (i = 1; i < ncache; i++)
        if (earliest_time > cache[i].time)
            earliest_time = cache[i].time;
    
    // return if the earliest data hasn't "aged" enough.
    if (earliest_time + 5 > clock_mon.tv_sec)
        return;
    
    uint32_t earliest_real_time = earliest_time + clock_delta;
    
    // Has the file been opened?
    if (fd == -1)
    {
        assert(entries_in_file == -1);
        assert(log_start == 0);
        
        fd = open("log.bin", O_RDWR | O_CREAT, 0666);
        assert(fd != -1);
        
        assert(lseek(fd, 0, SEEK_SET) == 0);
        
        struct stat st;
        assert(fstat(fd, &st) == 0);
        if (st.st_size >= sizeof(log_start))
        {
            assert(read(fd, &log_start, sizeof(log_start)) == sizeof(log_start));
            assert(earliest_real_time >= log_start);
            entries_in_file = (st.st_size - sizeof(log_start)) / sizeof(uint32_t);
        }
        else
        {
            log_start = earliest_real_time;
            assert(write(fd, &log_start, sizeof(log_start)) == sizeof(log_start));
            entries_in_file = 0;
        }
    }

    // do we have at least an hour's worth of runway?
    if (earliest_real_time + 60 * 60 - log_start > entries_in_file)
    {
        // write two hours of zero_entries just to be safe
        assert(lseek(fd, sizeof(log_start) + entries_in_file * sizeof(uint32_t), SEEK_SET) == sizeof(log_start) + entries_in_file * sizeof(uint32_t));
        
        ssize_t entries_to_write = earliest_real_time + 60 * 60 - log_start - entries_in_file;
        entries_to_write += 60 * 60;    // add an extra hour
        entries_in_file += entries_to_write;
        
        ssize_t bytes_to_write = entries_to_write * sizeof(uint32_t);
        
        FILE *logfile = fopen("/home/pi/ramdisk/logger.log", "a");
        if (logfile)
        {
            fprintf(logfile, "%d resize %ld + %ld entries %ld bytes\n", (int) time, (long) entries_in_file, (long) entries_to_write, (long) bytes_to_write);
            fclose(logfile);
        }

        void *zerobuf = calloc(bytes_to_write, 1);
        assert(zerobuf);
        
        while (bytes_to_write--)
        {
            ssize_t bytes_written = write(fd, zerobuf, bytes_to_write);
            assert(bytes_written > 0);
            bytes_to_write -= bytes_written;
        }
        free(zerobuf);
        zerobuf = NULL;
    }
    
    for (i = 0; i < ncache; i++)
        if (cache[i].time + 5 < clock_mon.tv_sec)
        {
            uint32_t real_time = cache[i].time + clock_delta;
            assert((real_time >= log_start) && (real_time - log_start < entries_in_file));
            
            // write it
            assert(lseek(fd, sizeof(log_start) + sizeof(uint32_t) * (real_time - log_start), SEEK_SET) == sizeof(log_start) + sizeof(uint32_t) * (real_time - log_start));
            cache[i].buf |= nfds << 29;
            assert(write(fd, &cache[i].buf, sizeof(cache[i].buf)) == sizeof(cache[i].buf));
            
            // remove it
            cache[i] = cache[ncache - 1];
            ncache--;
            i--;
        }
}

void child(void)
{
    ncache = 0;
    fd = -1;
    entries_in_file = -1;
    log_start = 0;
    nfds = 1;
    
    FILE *logfile = fopen("/home/pi/ramdisk/logger.log", "a");
    if (logfile)
    {
        struct timespec clock_real;
        assert(clock_gettime(CLOCK_REALTIME, &clock_real) == 0);
        struct timespec clock_mon;
        assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
        fprintf(logfile, "restart %d %d %d\n", (int) time(NULL), (int) clock_mon.tv_sec, (int) clock_real.tv_sec);
        fclose(logfile);
    }
    
    struct pollfd *fds = calloc(nfds, sizeof(fds[0]));
    assert(fds != NULL);
    
    fds[0].fd = socket(PF_INET, SOCK_STREAM, 0);
    assert(fds[0].fd != -1);
    
    int yes = 1;
    assert(setsockopt(fds[0].fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == 0);
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7356);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    assert(bind(fds[0].fd, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    
    assert(listen(fds[0].fd, 10) == 0);

    while (1)
    {
        int i;
        for (i = 0; i < nfds; i++)
            fds[i].events = POLLIN | POLLPRI;
        
        int pollret = poll(fds, nfds, 250);
        assert(pollret != -1);
        
        for (i = 1; i < nfds; i++)
        {
            assert((fds[i].revents == 0) || (fds[i].revents == POLLIN));
            if (fds[i].revents == POLLIN)
            {
                struct {
                    uint32_t time;
                    uint32_t data;
                    uint32_t mask;
                } message;
                assert(recv(fds[i].fd, &message, sizeof(message), 0) == sizeof(message));
                
                int idx = 0;
                while ((idx < ncache) && (cache[idx].time != message.time))
                    idx++;
                if (idx == ncache)
                {
                    ncache++;
                    assert(ncache < sizeof(cache) / sizeof(cache[0]));
                    cache[idx].time = message.time;
                    cache[idx].buf = 0;
                }
                cache[idx].buf &= ~message.mask;
                cache[idx].buf |= message.data;
            } 
        }
        assert((fds[0].revents == 0) || (fds[0].revents == POLLIN));
        if (fds[0].revents == POLLIN)
        {
            nfds++;
            fds = realloc(fds, nfds * sizeof(struct pollfd));
            assert(fds);
            fds[nfds-1].fd = accept(fds[0].fd, NULL, NULL);
            assert(fds[nfds-1].fd != -1);
        } 
        
        flushbuffer();             // timeout
    }
}

void mommy(void)
{
    while (1)
    {
        if (!fork()) child();
        wait(NULL);
        sleep(3);    // wait for a few seconds so we don't spin super quickly
    }
}

int main()
{
    if (!fork()) mommy();
    return 0;
}
