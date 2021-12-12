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

uint32_t buffer[300];
uint32_t buffer_start;

int fd;
uint32_t log_start;
ssize_t entries_in_file;

nfds_t nfds;

void flushbuffer(uint32_t time)
{
    if (buffer_start == 0)  // no buffer, yet
        return;
    
    struct timespec clock_mon;
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);

    if (time == 0)      // timeout
        time = clock_mon.tv_sec;

    assert((time >= buffer_start) && (time < buffer_start + sizeof(buffer)/sizeof(buffer[0])));
    
    uint32_t number_of_entries_to_leave_in_buffer = 290;
    int wstatus = system("/home/pi/timesync.sh");
    if (WIFEXITED(wstatus) && (WEXITSTATUS(wstatus) == 8))
        number_of_entries_to_leave_in_buffer = 3;
        
    if (time <= buffer_start + number_of_entries_to_leave_in_buffer)
        return;
        
    uint32_t buffer_entries_to_flush = time - (buffer_start + number_of_entries_to_leave_in_buffer);
    assert((buffer_entries_to_flush > 0) && (buffer_entries_to_flush <= sizeof(buffer)/sizeof(buffer[0])));
    
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
    struct timespec clock_real;
    assert(clock_gettime(CLOCK_REALTIME, &clock_real) == 0);
    uint32_t clock_offset = clock_real.tv_sec - clock_mon.tv_sec;

    time = clock_real.tv_sec;
    assert(time >= log_start);

    // ensure that there is at least an hour of runway
    if (time - log_start + 60 * 60 > entries_in_file)
    {
        // write two hours of zero_entries
        assert(lseek(fd, sizeof(log_start) + entries_in_file * sizeof(uint32_t), SEEK_SET) == sizeof(log_start) + entries_in_file * sizeof(uint32_t));
        ssize_t entries_to_write = time - log_start + 2 * 60 * 60 - entries_in_file;
        entries_in_file += entries_to_write;
        uint32_t zero_entry = 0;
        while (entries_to_write--)
            assert(write(fd, &zero_entry, sizeof(zero_entry)) == sizeof(zero_entry));
    }
    
    while (buffer_entries_to_flush--)
    {
        assert(lseek(fd, sizeof(log_start) + sizeof(uint32_t) * (buffer_start + clock_offset - log_start), SEEK_SET) == sizeof(log_start) + sizeof(uint32_t) * (buffer_start + clock_offset - log_start));
        buffer[0] |= nfds << 29;
        assert(write(fd, buffer, sizeof(buffer[0])) == sizeof(buffer[0]));
        
        memmove(buffer, buffer + 1, (sizeof(buffer) / sizeof(buffer[0]) - 1) * sizeof(buffer[0]));
        memset(buffer + (sizeof(buffer) / sizeof(buffer[0]) - 1), 0, sizeof(buffer[0]));
        buffer_start++;
    }
}

void child()
{
    FILE *logfile = fopen("/home/pi/ramdisk/logger.log", "a");
    if (logfile)
    {
        fprintf(logfile, "restart %d\n", (int) time(NULL));
        fclose(logfile);
    }
    
    fd = open("log.bin", O_RDWR | O_CREAT, 0666);
    assert(fd > 2);
    
    struct stat st;
    assert(fstat(fd, &st) == 0);
    
    assert(lseek(fd, 0, SEEK_SET) == 0);
    

    struct timespec clock_real;
    assert(clock_gettime(CLOCK_REALTIME, &clock_real) == 0);
    uint32_t time = clock_real.tv_sec;
    
    if (st.st_size >= sizeof(log_start))
    {
        assert(read(fd, &log_start, sizeof(log_start)) == sizeof(log_start));
        assert(time >= log_start);
        entries_in_file = (st.st_size - sizeof(log_start)) / sizeof(uint32_t);
    }
    else
    {
        log_start = time;
        assert(write(fd, &log_start, sizeof(log_start)) == sizeof(log_start));
        entries_in_file = 0;
    }
        
    // ensure that there is at least an hour of runway
    if (time - log_start + 60 * 60 > entries_in_file)
    {
        // write two hours of zero_entries
        assert(lseek(fd, sizeof(log_start) + entries_in_file * sizeof(uint32_t), SEEK_SET) == sizeof(log_start) + entries_in_file * sizeof(uint32_t));
        ssize_t entries_to_write = time - log_start + 2 * 60 * 60 - entries_in_file;
        entries_in_file += entries_to_write;
        uint32_t zero_entry = 0;
        while (entries_to_write--)
            assert(write(fd, &zero_entry, sizeof(zero_entry)) == sizeof(zero_entry));
    }
    
    nfds = 1;
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
    
    memset(buffer, 0, sizeof(buffer));
    buffer_start = 0;

    while (1)
    {
        fds[0].events = POLLIN | POLLPRI;
        
        int i;
        for (i = 1; i < nfds; i++)
            fds[i].events = POLLIN | POLLPRI;
        
        int pollret = poll(fds, nfds, 250);
        assert(pollret != -1);
        
        if (pollret)
        {

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
                    
                    if (buffer_start == 0)
                        buffer_start = message.time;
                    assert(message.time >= buffer_start - 2);
                    if (message.time < buffer_start)
                    {
                        size_t bytes_to_shift = (buffer_start - message.time) * sizeof(buffer[0]);
                        memmove(buffer + bytes_to_shift, buffer, sizeof(buffer) - bytes_to_shift);
                        memset(buffer, 0, bytes_to_shift);
                        buffer_start = message.time;
                    }
                    
                    flushbuffer(message.time);
                    
                    int idx = message.time - buffer_start;
                    assert((idx >= 0) && (idx < sizeof(buffer) / sizeof(buffer[0])));
                    buffer[idx] &= ~message.mask;
                    buffer[idx] |= message.data;
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
        }
        else
            flushbuffer(0);             // timeout
    }
    
    exit(-1);
}

void mommy()
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
