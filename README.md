Compiling
NOTE: You may need need to change the ZNC module path. The example below is for ZNC systems configured from the Running ZNC as a system daemon section in the ZNC wiki.

Holo@znc:~$ curl -o ~/push.cpp https://znc.iglooirc.com/push.cpp
Holo@znc:~$ znc-buildmod push.cpp
Holo@znc:~$ sudo cp push.so /var/lib/znc/modules/
Holo@znc:~$ rm ~/push.so
Holo@znc:~$ sudo systemctl restart znc.service
NOTE: If you are having problems compiling see the Compiling modules section in the ZNC wiki.

Now, load the module in ZNC:

/msg *status loadmod --type=user push

Configuration
Once you have the device token copied (from settings inside Igloo), we need to set up the push service to send push notifications to the device.
/msg *push set device1 token

NOTE: You can have up to 5 different devices configured for push. Replace device1 in the above command with device2 etc.

Test to see if push notifications are working
/msg *push send test