while :
do
ping -w 10 -p 2 -I eth0 `cat /home/pi/ramdisk2/ip`
done
