# monitorlewiston
Raspberry Pi software to monitor my parent's house

## Goals

1. Detect Power Outages
    * A resistor is connected across two prongs of a standard house outlet plug.
    * An ADC continously measures the voltage across the resistor at 3k SPS.
    * The Raspberry Pi is powered by a UPS battery backup.
1. Monitor WiFi Strength
    * Connect to the home network via Raspberry Pi's built in WiFi.
    * Use standard linux reporting of WiFi Strength.
1. Monitor Network Health
    * The Raspberry Pi continuosly sends ICMP pings and monitors the responses.
    * pings are sent via both ethernet and WiFi.
    * pings are sent to the WiFi Router.
    * pings are sent to the Cable Modem.
    * pings are sent to a static server on the Internet.
    * All networking equipment is also powered by the UPS battery backup.
1. Provide CallerID Display
    * Incoming phone calls and CallerID are detected via a USB modem.
1. Store All Data Locally
    * Data from all monitors are stored on the Raspberry Pi's flash drive.
    * Data is normalized in a minimal format to allow years of data to be stored.
    * All data is initally timestamped via the local clock.
    * Timestamps are converted to UTC when Network Time Protocol is available.
    * Writes to the flash memory are minimized to prolong hardware lifespan.
    * Ramdisks are used to store rapidly changing dynamic data.
1. Provide Remote Access
    * Remote diagnostic access is provided via reverse ssh tunneling.
    * The remote ssh server can be changed at will.
    * The dynamic server configuration is hosted by a well known static server.
    
## Associated Hardware

1. Raspberry Pi 3 Model B Rev 1.2 + power supply
1. 16GB flash
1. APC UPS Battery Backup Surge Protector, 425VA Backup Battery Power Supply
1. two resistors (Voltage divider) + old power plug
1. ADS1015 ADC + Jumper Wires
1. USB 56K External Dial Up Fax Data Modem

## Linux configurations

1. create a ramdisk
    ```
    $ sudo su -
    # cat >> /etc/fstab 
    tmpfs /home/pi/ramdisk tmpfs size=800000 0 0
    tmpfs /home/pi/ramdisk2 tmpfs size=800000 0 0
    #
    ```
1. install an ssh key which provides access to remote server
    ```
    $ cat > ~/.ssh/reversetunnel.pem
    -----BEGIN OPENSSH PRIVATE KEY-----
    ...
    -----END OPENSSH PRIVATE KEY-----
    $
    ```
1. the `monping` program which listens for raw ICMP must run as root
    ```
    $ echo "@reboot /home/pi/monping" | sudo crontab -
    $ sudo crontab -l
    @reboot /home/pi/monping
    $ 
    ```
1. the rest of the programs can run as user pi
    ```
    $ cat | crontab -
    @reboot /home/pi/crongetip.sh
    @reboot /home/pi/cronautossh.sh
    @reboot /home/pi/cronping0.sh
    @reboot /home/pi/cronping1.sh
    @reboot /home/pi/cronping2.sh
    @reboot /home/pi/cronping3.sh
    @reboot /home/pi/logger
    @reboot /home/pi/monphone
    @reboot /home/pi/monwifistrength
    @reboot /home/pi/mon120v
    $
    ```
