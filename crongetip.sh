while :
do
/usr/bin/curl -o /home/pi/ramdisk2/ip_tmp http://joeruff.com/ip
sleep 1
if [ -f /home/pi/ramdisk2/ip_tmp ]
then
mv /home/pi/ramdisk2/ip_tmp /home/pi/ramdisk2/ip
sleep 60
fi
done
