while [ ! -f /home/pi/ramdisk2/ip ]
do
sleep 1
done

while :
do
/usr/bin/ssh -N -R 8000:localhost:22 -o StrictHostKeyChecking=no -o ServerAliveInterval=15 -i /home/pi/.ssh/weaksecurity ubuntu@`cat /home/pi/ramdisk2/ip`
done
