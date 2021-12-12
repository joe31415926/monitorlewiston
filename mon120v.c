#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <string.h>
#include <poll.h>
#include <assert.h>

// $ gcc -O3 -o mon120v mon120v.c -lm -pthread


// keep historical samples for an hour for analysis
// 16777216 / ~3300 / 60 / 60 = ~1.4

#define MASSIVE_RINGBUFFER_SIZE (16777216)
int16_t *ringbuffer;              // raw ADC samples
struct timespec *ringbuffert;     // The time that each sample was taken
int16_t *ringbuffers;             // sine wave against which we are comparing the raw samples
int32_t *ringbufferrunning;       // the running average of the difference between the two
long *ringbufferphase;
uint32_t ringbuffer_idx;

// the LUT has one second of 60Hz sine waves (to account for every possible value of tv_nsec >> 9)
//     1000000000ns >> 9 = 1953125
#define SAMPLES_IN_ONE_SECOND_LUT (1000000000L >> 9)

// but we also have to add two extra waves to account for phase shift!
//     and to give us a LUT of sin() as well as cos()
#define LUT_WAVELENGTH (SAMPLES_IN_ONE_SECOND_LUT / 60)

double *lut;
long lut_phase; // the phase is constantly adjusted to keep the samples and the lut lined up with each other

// 0.998 ^ 550 = 1/e
// there are 55 samples per wavelength so the leaky bucket will cover about 10 wavelengths
#define LEAKY_BUCKET_NUMERATOR   (998)
#define LEAKY_BUCKET_DENOMINATOR (1000)

// adjust to taste. This doesn't create too many false positives and hopefully no false negatives
#define EVENT_RUNNING_THRESHOLD (6000)

// The debouncing logic prevents duplicate events that are closer than 2 seconds apart
// so this size will certainly keep at least a few minutes of historical data
// and probably months of historical data!!
#define HISTORICAL_EVENT_SIZE (100)
time_t events_transition_time[HISTORICAL_EVENT_SIZE];
int history_idx;

void closeandopen(struct pollfd *fd)
{
    if (fd->fd != -1)
        close(fd->fd);    // ignore error - hey, we're just trying to be nice here in a bad situation!

    // power down the ads1015
    FILE *gpiodev = fopen("/sys/class/gpio/gpio4/value", "w");
    assert(gpiodev != NULL);
    assert(fprintf(gpiodev, "0\n") == 2);
    assert(fclose(gpiodev) == 0);

    // wait for awhile
    assert(usleep(30000) == 0); // 30ms

    // power it back up again
    gpiodev = fopen("/sys/class/gpio/gpio4/value", "w");
    assert(gpiodev != NULL);
    assert(fprintf(gpiodev, "1\n") == 2);
    assert(fclose(gpiodev) == 0);

    // wait for awhile
    assert(usleep(30000) == 0); // 30ms

    // attempt to open it
    fd->fd = open("/dev/i2c-1", O_RDWR);
    assert(fd->fd > 0);
    assert(ioctl(fd->fd, I2C_SLAVE, 0x49) == 0);
}

void reseti2cpin(struct pollfd *fd)
{
    // keep attempting to reset and configure the ads1015 until successful!
    
    ssize_t nb = 0;
    while (nb != 3)
    {
        closeandopen(fd);

        uint16_t OS = 0;            // 0 : No effect
        uint16_t MUX = 0;            // AINP = AIN0 and AINN = AIN1
        uint16_t PGA = 0;            // 000 : FS = ±6.144V
        uint16_t MODE = 0;            // 0 : Continuous conversion mode
        uint16_t DR = 7;             // 111 : 3300SPS

        uint16_t COMP_MODE = 0;     // 0 : Traditional comparator with hysteresis (default)
        uint16_t COMP_POL = 0;         // 0 : Active low (default)
        uint16_t COMP_LAT = 0;         // 0 : Non-latching comparator (default)
        uint16_t COMP_QUE = 3;         // 11 : Disable comparator (default)

        uint16_t config_register = (OS << 15) | (MUX << 12) | (PGA << 9) | (MODE << 8) |
                                    (DR << 5) | (COMP_MODE << 4) | (COMP_POL << 3) | (COMP_LAT << 2) | (COMP_QUE << 0);
        uint16_t MSB = config_register >> 8;
        uint16_t LSB = config_register & 0xff;

        char buf[3];
        buf[0] = 0x01;
        buf[1] = MSB;
        buf[2] = LSB;

        struct timespec timeout;
        timeout.tv_sec = 0;
        timeout.tv_nsec = 1000000;  // 1ms
        
        fd->events = POLLOUT;
        if (ppoll(fd, 1, &timeout, NULL) == 1)
            nb = write(fd->fd, buf, 3);            
    }
}

void *simply_measure_thread(void *stupid)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7356);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int sock = socket(PF_INET, SOCK_STREAM, 0);
    assert(sock != -1);

    assert(connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == 0);
    
    int log_err_read_failed = 0;
    int log_err_write_failed = 0;
    int log_err_event_happened = 0;
    
    struct {
        uint32_t time;
        uint32_t data;
        uint32_t mask;
    } message;
    
    struct timespec clock_mon;
    assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
    message.time = clock_mon.tv_sec;

    int consequtive_zeros = 0;
    int32_t running = 0;

    struct pollfd fd;
    fd.fd = -1;
    reseti2cpin(&fd);
    
    // get measurements forever
    while (1)
    {
        // try and try to get a measurement until successful
        while (1)
        {
            struct timespec timeout;
            timeout.tv_sec = 0;
            timeout.tv_nsec = 1000000;  // wait a maximum of 1ms for a sample and then reset
            
            fd.events = POLLOUT;

            if (ppoll(&fd, 1, &timeout, NULL) == 1)
            {
                if (fd.revents == POLLOUT) // explicitly disallow any errors
                {
                    char buf[2] = {0};
                    if (write(fd.fd, buf, 1) == 1)
                    {
                        timeout.tv_sec = 0;
                        timeout.tv_nsec = 1000000;  // 1ms
                        
                        fd.events = POLLIN;
                        
                        if (ppoll(&fd, 1, &timeout, NULL) == 1)
                        {
                            if (fd.revents == POLLIN) // explicitly disallow any errors
                            {
                                if (read(fd.fd, buf, 2) == 2)
                                {
                                    ringbuffer[ringbuffer_idx] = (buf[0] << 8) | buf[1];
                                    
                                    if (ringbuffer[ringbuffer_idx] == 0)
                                        consequtive_zeros++;
                                    else
                                        consequtive_zeros = 0;

                                    if (consequtive_zeros < 10000)
                                        break;    // we're free!!!
                                }
                                else
                                    log_err_read_failed = 1;
                            }
                        }
                    }
                    else
                        log_err_write_failed = 1;
                }
            }
                
            consequtive_zeros = 0;
            reseti2cpin(&fd);
        }
        
        assert(clock_gettime(CLOCK_MONOTONIC_RAW, ringbuffert + ringbuffer_idx) == 0);
        
        if (ringbuffert[ringbuffer_idx].tv_sec != message.time)
        {
            message.data = 0x08 | (log_err_read_failed << 2) | (log_err_write_failed << 1) | log_err_event_happened;
            message.data <<= 6;
            
            uint32_t running_to_record = running;
            if (running_to_record > EVENT_RUNNING_THRESHOLD)
                running_to_record = EVENT_RUNNING_THRESHOLD;
            running_to_record = (running_to_record * 0x3f) / EVENT_RUNNING_THRESHOLD;
            assert((running_to_record >= 0) && (running_to_record <= 0x3f));
            message.data |= running_to_record;
            message.mask = (0x3ff);  // 1 + 1 + 1 + 1 + 6 = 10

            message.data <<= 4;
            message.mask <<= 4;

            ssize_t bytessent = send(sock, &message, sizeof(message), 0);
            assert(bytessent == sizeof(message));

            log_err_read_failed = 0;
            log_err_write_failed = 0;
            log_err_event_happened = 0;
            message.time = ringbuffert[ringbuffer_idx].tv_sec;
        }
        
        uint32_t lut_idx = ringbuffert[ringbuffer_idx].tv_nsec >> 9;
        ringbuffers[ringbuffer_idx] = lut[lut_idx + lut_phase];
        ringbufferphase[ringbuffer_idx] = lut_phase;
        
        int32_t delta = abs(ringbuffer[ringbuffer_idx] - ringbuffers[ringbuffer_idx]);
        
        // update the running average so we can compare against the threshold to see if there has been an event
        int32_t new_running = (LEAKY_BUCKET_NUMERATOR * running + delta) / LEAKY_BUCKET_DENOMINATOR;
        ringbufferrunning[ringbuffer_idx] = new_running;
        if (((new_running >= EVENT_RUNNING_THRESHOLD) && (running < EVENT_RUNNING_THRESHOLD)) || ((running >= EVENT_RUNNING_THRESHOLD) && (new_running < EVENT_RUNNING_THRESHOLD)))
        {
            log_err_event_happened = 1;
            // there has been an event. In other words, the running average has crossed the threshold one way or another
            // debounce. We don't need two events within 2 seconds of each other
            int previous_idx = (history_idx + HISTORICAL_EVENT_SIZE - 1) % HISTORICAL_EVENT_SIZE;
            if (events_transition_time[previous_idx] + 2 < ringbuffert[ringbuffer_idx].tv_sec)
            {
                events_transition_time[history_idx] = ringbuffert[ringbuffer_idx].tv_sec;
                history_idx = (history_idx + 1) % HISTORICAL_EVENT_SIZE;
            }
        }
        running = new_running;

        ringbuffer_idx = (ringbuffer_idx + 1) % MASSIVE_RINGBUFFER_SIZE;
    }
}

void child()
{
    FILE *logfile = fopen("/home/pi/ramdisk/mon120v.log", "a");
    if (logfile)
    {
        fprintf(logfile, "restart %d\n", (int) time(NULL));
        fclose(logfile);
    }
    
    // configure the gpio pin - in case this is the first time we've run since machine boot
    //     gpio4 is the pin supplying power to the ads1015
    FILE *gpiodev = fopen("/sys/class/gpio/export", "w");
    assert(gpiodev != NULL);
    assert(fprintf(gpiodev, "4\n") == 2);
    fclose(gpiodev); // don't check the error because I don't think it is possible to close this.
    
    sleep(2);

    gpiodev = fopen("/sys/class/gpio/gpio4/direction", "w");
    assert(gpiodev != NULL);
    assert(fprintf(gpiodev, "out\n") == 4);
    assert(fclose(gpiodev) == 0);
    
    gpiodev = fopen("/sys/class/gpio/gpio4/value", "w");
    assert(gpiodev != NULL);
    assert(fprintf(gpiodev, "1\n") == 2);
    assert(fclose(gpiodev) == 0);

    ringbuffer = malloc(sizeof(ringbuffer[0]) * MASSIVE_RINGBUFFER_SIZE);
    ringbuffert = malloc(sizeof(ringbuffert[0]) * MASSIVE_RINGBUFFER_SIZE);
    ringbufferrunning = malloc(sizeof(ringbufferrunning[0]) * MASSIVE_RINGBUFFER_SIZE);
    ringbufferphase  = malloc(sizeof(ringbufferphase[0]) * MASSIVE_RINGBUFFER_SIZE);
    ringbuffers = malloc(sizeof(ringbuffers[0]) * MASSIVE_RINGBUFFER_SIZE);
    assert((ringbuffer != NULL) && (ringbuffert != NULL) && (ringbufferrunning != NULL) && (ringbufferphase != NULL) && (ringbuffers != NULL));
    ringbuffer_idx = 0;

    // The power line should be 120V RMS, which is 120.0 * M_SQRT2 peak-to-peak
    //    Tha 16-bit ADC is configured for +/- 6.144V full scale
    //    The voltage is measured across a voltage divider using a 1M Ohm resistor and a 22k Ohm resistor

    lut = malloc(sizeof(lut[0]) * (SAMPLES_IN_ONE_SECOND_LUT + 2 * LUT_WAVELENGTH));
    assert(lut != NULL);    
    int idx;
    for (idx = 0; idx < SAMPLES_IN_ONE_SECOND_LUT + 2 * LUT_WAVELENGTH; idx++)
        lut[idx] = 120.0 * M_SQRT2 * 32768.0 / 6.144 * 22000.0 / (1000000.0 + 22000.0) * sin(2.0 * M_PI / (double) LUT_WAVELENGTH * (double) idx);
    lut_phase = 0;
    history_idx = 0;
    memset(events_transition_time, 0, sizeof(events_transition_time));
    
    pthread_t pth;
    assert(pthread_create(&pth, NULL, simply_measure_thread, NULL) == 0);
    
    char filename[3][50];
    memset(filename, 0, sizeof(filename));
    int filenameidx = 0;

    uint32_t first_phase_adjustment_time = 0;
    
    int last_event_idx = 0;
    while (1)
    {
        // give the sample thread some time to gather a reasonable amount of data to fit the sine wave to
        // (about one tenth of a second, or 330 samples)
        usleep(50000 + (random() % 100000));

        double sin_accumulator = 0.0;
        double cos_accumulator = 0.0;
        double num = 0.0;
        // grab 3333 samples, or about one second of data
        uint32_t last_ringbuffer_idx = (ringbuffer_idx + MASSIVE_RINGBUFFER_SIZE - 3333) % MASSIVE_RINGBUFFER_SIZE;
        uint32_t remember_last_ringbuffer_idx = last_ringbuffer_idx;
        while (last_ringbuffer_idx != ringbuffer_idx)
        {
            uint32_t idx = ringbuffert[last_ringbuffer_idx].tv_nsec >> 9;
            sin_accumulator += (double) ringbuffer[last_ringbuffer_idx] * lut[idx];
            cos_accumulator += (double) ringbuffer[last_ringbuffer_idx] * lut[idx + LUT_WAVELENGTH / 4];
            num += 1.0;
            last_ringbuffer_idx = (last_ringbuffer_idx + 1) % MASSIVE_RINGBUFFER_SIZE;
        }
        // normalize
        //       Also, the infinite integral of sin^2 is 1/2 so multiply to 2 to get that back.
        // none of this is actually necessary if we don't care about the amplitude, which we don't
        sin_accumulator *= 2.0 / num;
        cos_accumulator *= 2.0 / num;
        lut_phase = LUT_WAVELENGTH * atan2(cos_accumulator, sin_accumulator) / (2.0 * M_PI);
        
        if (first_phase_adjustment_time == 0)
        {
            struct timespec clock_mon;
            assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
            first_phase_adjustment_time = clock_mon.tv_sec;
        }

        // for debugging, write a file of all the latest results (but write it to a ramdisk!)
        if (filename[filenameidx][0])
            unlink(filename[filenameidx]);
        sprintf(filename[filenameidx], "/home/pi/ramdisk/waveform_%d.txt", ringbuffert[remember_last_ringbuffer_idx].tv_sec);
        FILE *myoutputfile = fopen(filename[filenameidx], "w");
        filenameidx = (filenameidx + 1) % (sizeof(filename) / sizeof(filename[0]));
        if (myoutputfile)
        {
            struct timespec starttime = ringbuffert[remember_last_ringbuffer_idx];
            while (remember_last_ringbuffer_idx != last_ringbuffer_idx)
            {
            //    uint32_t idx = (ringbuffert[remember_last_ringbuffer_idx].tv_nsec) >> 9;
                fprintf(myoutputfile, "%ld.%09ld %d %d %d %d\n", (long) ringbuffert[remember_last_ringbuffer_idx].tv_sec, (long) ringbuffert[remember_last_ringbuffer_idx].tv_nsec, ringbuffer[remember_last_ringbuffer_idx], ringbuffers[remember_last_ringbuffer_idx], lut_phase, ringbufferrunning[remember_last_ringbuffer_idx]);
                remember_last_ringbuffer_idx = (remember_last_ringbuffer_idx + 1) % MASSIVE_RINGBUFFER_SIZE;
            }
            fclose(myoutputfile);
        }
        
        int most_recent_event_idx = (history_idx + HISTORICAL_EVENT_SIZE - 1) % HISTORICAL_EVENT_SIZE;
        int most_recent_ringbuffer_idx = (ringbuffer_idx + MASSIVE_RINGBUFFER_SIZE - 1) % MASSIVE_RINGBUFFER_SIZE;
        int age_of_most_recent_event = ringbuffert[most_recent_ringbuffer_idx].tv_sec - events_transition_time[most_recent_event_idx];
        
        // we dump 2 seconds of data AFTER the event, so make sure we actually have 10 seconds of data in the ringbuffer before dumping the event
        if ((last_event_idx != history_idx) && (age_of_most_recent_event > 10))
        {
            // eat all events up until 2 seconds after first phase adjustment
            if (events_transition_time[most_recent_event_idx] > first_phase_adjustment_time + 2)
            {
                struct timespec clock_mon;
                assert(clock_gettime(CLOCK_MONOTONIC_RAW, &clock_mon) == 0);
                struct timespec clock_real;
                assert(clock_gettime(CLOCK_REALTIME, &clock_real) == 0);
                uint32_t clock_offset = clock_real.tv_sec - clock_mon.tv_sec;
    
                long starttime = events_transition_time[last_event_idx] - 2;
                long endtime = events_transition_time[most_recent_event_idx] + 2;
                
                mkdir("/home/pi/120V/", 0777);
                char eventfilename[50];
                sprintf(eventfilename, "/home/pi/120V/event_%d.txt", ringbuffert[most_recent_ringbuffer_idx].tv_sec + clock_offset);
                myoutputfile = fopen(eventfilename, "w");
                if (myoutputfile)
                {
                    int endidx = ringbuffer_idx;
                    int idx = (endidx + 1) % MASSIVE_RINGBUFFER_SIZE;
                    while (idx != endidx)
                    {
                        if ((ringbuffert[idx].tv_sec >= starttime) && (ringbuffert[idx].tv_sec <= endtime))
                            fprintf(myoutputfile, "%ld.%09ld %d %d %d %ld\n", (long) ringbuffert[idx].tv_sec + clock_offset, (long) ringbuffert[idx].tv_nsec, ringbuffer[idx], ringbuffers[idx], ringbufferrunning[idx], ringbufferphase[idx]);
                        idx = (idx + 1) % MASSIVE_RINGBUFFER_SIZE;
                    }
                    fclose(myoutputfile);
                }
            }
            
            last_event_idx = history_idx;
        }
    }
}

void mommy()
{
    while (1)
    {
        if (!fork()) child();
            wait(NULL);
    }
}

int main()
{
    if (!fork()) mommy();
    return 0;
}
