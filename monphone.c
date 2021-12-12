#define _GNU_SOURCE
#include <stdio.h>
#include <assert.h>
#include <termios.h>
#include <unistd.h>     // read, write, fork, sleep
#include <fcntl.h>      // open
#include <time.h>       // time, localtime, strftime
#include <stdlib.h>     // exit
#include <poll.h>
#include <sys/wait.h>

FILE *logfile;
char buffer[100];
char *ts(void)
{
    time_t now = time(NULL);
    char formattedtimebuffer[100];
    assert(strftime(formattedtimebuffer, sizeof(formattedtimebuffer), "%F %T", localtime(&now)) < sizeof(formattedtimebuffer));
    assert(snprintf(buffer, sizeof(buffer), "%d, %s,", (int) now, formattedtimebuffer) < sizeof(buffer));
    return buffer;
}

void drain(struct pollfd fds, int timeout)
{
    int spit_out_a_timestamp = 1;
    char very_small_buffer;
    fds.events = POLLIN | POLLPRI | POLLRDHUP;
    int poll_ret;
    
    while (((poll_ret = poll(&fds, 1, timeout)) == 1) && (fds.revents == POLLIN))
    {
        ssize_t nread = read(fds.fd, &very_small_buffer, 1);
        if (nread != 1)
        {
            if (!spit_out_a_timestamp) fprintf(logfile, "\n");
            fprintf(logfile, "%s fatal error nread: %d\n", ts(), (int) nread);
            close(fds.fd);
            fclose(logfile);
            exit(-1);
        }
        if (spit_out_a_timestamp)
        {
            assert(fprintf(logfile, "%s ", ts()) > 10);
            spit_out_a_timestamp = 0;
        }
        assert(fprintf(logfile, "%c", very_small_buffer) == 1);
        if (very_small_buffer == '\n')
        {
            assert(fflush(logfile) == 0);
            spit_out_a_timestamp = 1;
        }
    }

    if (!spit_out_a_timestamp)
    {
        assert(fprintf(logfile, "\n") == 1);
        assert(fflush(logfile) == 0);
    }
    
    if ((poll_ret != 0) || (fds.revents != 0))
    {
        fprintf(logfile, "%s fatal error poll: %d %08x\n", ts(), poll_ret, fds.revents);
        close(fds.fd);
        fclose(logfile);
        exit(-1);
    }
}

void child()
{
    FILE *errlogfile = fopen("/home/pi/ramdisk/monphone.log", "a");
    if (errlogfile)
    {
        fprintf(errlogfile, "restart %d\n", (int) time(NULL));
        fclose(errlogfile);
    }

    logfile = fopen("/home/pi/call_snooping.txt", "a");
    assert(logfile != NULL);
    
    fprintf(logfile, "%s child starting POLLIN %08x, POLLPRI %08x, POLLRDHUP %08x, POLLERR %08x, POLLHUP %08x, POLLNVAL %08x\n", ts(), POLLIN, POLLPRI, POLLRDHUP, POLLERR, POLLHUP, POLLNVAL);
    fflush(logfile);

    struct pollfd fds;
    fds.fd = open("/dev/ttyACM0", O_RDWR | O_NOCTTY | O_SYNC);
    assert(fds.fd > 1);
    
    struct termios tty;
    assert(tcgetattr(fds.fd, &tty) == 0);
    
    assert(cfsetspeed(&tty, B57600) == 0); // tested and work: B9600, B19200, B57600, B115200

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls, enable reading
    tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    tty.c_iflag &= ~IGNBRK;         // disable break processing
    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

    tty.c_lflag = 0;                // no signaling chars, no echo, no canonical processing
    tty.c_oflag = 0;                // no remapping, no delays
    tty.c_cc[VMIN]  = 0;            // read doesn't block
    tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout
        
    assert(tcsetattr(fds.fd, TCSANOW, &tty) == 0);

    drain(fds, 5000);    // flush all input until there is a full 5 seconds with no input
    
    assert(fprintf(logfile, "%s configuring\n", ts()) > 10);
    assert(fflush(logfile) == 0);

    assert(write(fds.fd, "ATE1\r", 5) == 5);    // enable echo so the commands appear in the output
    assert(write(fds.fd, "ATI0\r", 5) == 5);    // Modem speed <- various info requests which return stuff..
    assert(write(fds.fd, "ATI3\r", 5) == 5);    // controller code version
    assert(write(fds.fd, "ATI5\r", 5) == 5);    // board name
    assert(write(fds.fd, "AT&V\r", 5) == 5);    // active profile
    assert(write(fds.fd, "AT+VCID=1\r", 10) == 10); // enable caller id
    
    assert(fprintf(logfile, "%s listening\n", ts()) > 10);
    assert(fflush(logfile) == 0);

    drain(fds, -1);
    
    assert(fprintf(logfile, "%s finished\n", ts()) > 10);
    assert(fflush(logfile) == 0);
}

void mommy()
{
    while (1)
    {
        if (!fork()) child();
        wait(NULL);
        sleep(1);    // wait for a second so we don't spin super quickly
    }
}

int main()
{
    if (!fork()) mommy();
    return 0;
}
