#include <stdio.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#define OFFSET_TO_NFDS (29)
#define MASK_FOR_NFDS (0x07)

#define OFFSET_TO_SIGNAL (23)
#define MASK_FOR_SIGNAL (0x3f)

#define OFFSET_TO_PING (14)
#define MASK_FOR_PING (0x1ff)

#define OFFSET_TO_120V (4)
#define MASK_FOR_120V (0x3ff)

int main()
{
    int fd = open("log.bin", O_RDONLY);
    assert(fd != -1);
    
    struct stat st;
    assert(fstat(fd, &st) == 0);
    
    uint32_t *d = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(d != MAP_FAILED);

    assert(close(fd) == 0);
    
    int i;
    for (i = 1; i < st.st_size / sizeof(uint32_t); i++)
    {        
         time_t t = d[0] + i - 1;
         if (t > time(NULL))
            break;

        int num_fds = ((d[i] >> OFFSET_TO_NFDS) & MASK_FOR_NFDS);
        int logger_running = (num_fds > 0);
        
        int signal = ((d[i] >> OFFSET_TO_SIGNAL) & MASK_FOR_SIGNAL);
        int signal_reported = (signal & 0x20) ? 1 : 0;
        signal &= 0x1f;
        signal -= 64;
        
        int ping = ((d[i] >> OFFSET_TO_PING) & MASK_FOR_PING);
        int ping_reported = (ping & 0x100) ? 1 : 0;
        int p0 = (ping >> 6) & 0x03;
        int p1 = (ping >> 4) & 0x03;
        int p2 = (ping >> 2) & 0x03;
        int p3 = (ping >> 0) & 0x03;
        
        int volt = ((d[i] >> OFFSET_TO_120V) & MASK_FOR_120V);
        int volt_reported =     (volt & 0x200) ? 1 : 0;
        int read_err =          (volt & 0x100) ? 1 : 0;
        int write_err =         (volt & 0x80) ? 1 : 0;
        int event_happened =   (volt & 0x40) ? 1 : 0;
        volt &= 0x3f;

        char formattedtime[100];
        assert(strftime(formattedtime, sizeof(formattedtime), "%F %T", localtime(&t)) );

        printf("%d %s", (int) t, formattedtime);
        if (logger_running) printf(" logger %d", num_fds);
        if (signal_reported) printf(" signal %d", signal);
        if (ping_reported) printf(" ping %d %d %d %d", p0, p1, p2, p3);
        if (volt_reported) printf(" volt %d %d %d %d", read_err, write_err, event_happened, volt);
        printf("\n");
    }
    
    assert(munmap(d, st.st_size) == 0);
}