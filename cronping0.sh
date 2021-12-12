while :
do
ping -w 10 -p 0 -I wlan0 `cat /home/pi/ramdisk2/ip`
done
